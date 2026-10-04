// ldac-win: stream Windows audio to a Bluetooth headset using the LDAC codec.
//
// Pipeline:  WASAPI loopback (what Windows plays)  ->  LDAC encoder (Sony libldac)
//            ->  A2DP source (BTstack)  ->  USB Bluetooth dongle (WinUSB driver)  ->  headset
//
// Usage:
//   ldac-win.exe --list-audio
//   ldac-win.exe --scan
//   ldac-win.exe --connect AA:BB:CC:DD:EE:FF [--device N] [--quality hq|sq|mq] [--no-retry]

#define BTSTACK_FILE__ "main.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "btstack.h"
#include "btstack_run_loop_windows.h"
#include "btstack_stdin.h"
#include "btstack_stdin_windows.h"
#include "btstack_tlv_windows.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "hci_dump.h"
#include "hci_dump_windows_fs.h"
#include "hci_transport.h"
#include "hci_transport_usb.h"

#include "ldacBT.h"
#include "audio_capture.h"

// ---------------------------------------------------------------------------------------------
// LDAC A2DP constants (vendor-specific codec: Sony 0x012D, codec id 0xAA)
// ---------------------------------------------------------------------------------------------
#define LDAC_VENDOR_ID   0x0000012Du
#define LDAC_CODEC_ID    0x00AAu
#define LDAC_INFO_LEN    8

#define LDAC_SF_44100    0x20
#define LDAC_SF_48000    0x10
#define LDAC_SF_88200    0x08
#define LDAC_SF_96000    0x04
#define LDAC_SF_ALL      0x3F

#define LDAC_CM_MONO     0x04
#define LDAC_CM_DUAL     0x02
#define LDAC_CM_STEREO   0x01
#define LDAC_CM_ALL      0x07

#define LDAC_BLOCK_FRAMES  128        // PCM frames per ldacBT_encode() call (fixed by libldac)
#define AUDIO_TICK_MS      4
#define MAX_PACKET_QUEUE   48
#define MAX_PACKET_BYTES   1100

// ---------------------------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------------------------
typedef enum { MODE_NONE, MODE_SCAN, MODE_CONNECT } app_mode_t;

typedef struct {
    uint16_t len;
    uint32_t timestamp;
    uint8_t  data[MAX_PACKET_BYTES];
} packet_t;

static app_mode_t   app_mode = MODE_NONE;
static bd_addr_t    target_addr;
static int          audio_device = -1;
static int          ldac_eqmid = LDACBT_EQMID_SQ;
static bool         auto_retry = true;
static bool         shutdown_triggered = false;

static uint32_t     capture_rate = 0;

// A2DP / AVDTP
static uint16_t     a2dp_cid = 0;
static uint8_t      ldac_local_seid = 0;
static bool         ldac_selected = false;
static bool         ldac_seen = false;
static bool         stream_open = false;
static bool         streaming = false;

static uint8_t ldac_local_caps[LDAC_INFO_LEN] = {
    0x2D, 0x01, 0x00, 0x00,         // vendor id (little endian)
    0xAA, 0x00,                     // codec id (little endian)
    LDAC_SF_ALL,                    // sampling frequencies
    LDAC_CM_ALL                     // channel modes
};
static uint8_t ldac_local_cfg[LDAC_INFO_LEN];
static uint8_t ldac_remote_cfg[LDAC_INFO_LEN];

// encoder
static HANDLE_LDAC_BT ldac_handle = NULL;
static int            ldac_mtu = 0;

// pacing / packet queue
static btstack_timer_source_t audio_timer;
static btstack_timer_source_t retry_timer;
static btstack_timer_source_t stats_timer;
static uint32_t   stream_start_ms;
static uint64_t   frames_encoded;
static uint32_t   rtp_ts_base;
static packet_t   queue[MAX_PACKET_QUEUE];
static int        q_head, q_tail, q_count;
static bool       send_request_pending;

// statistics
static uint32_t   stat_packets, stat_bytes, stat_dropped, stat_silence_blocks, stat_skips;

static btstack_packet_callback_registration_t hci_event_callback_registration;

// persistent storage
static char                    tlv_db_path[100];
static const btstack_tlv_t *   tlv_impl;
static btstack_tlv_windows_t   tlv_context;

// scan state
#define MAX_SCAN_DEVICES 32
typedef struct {
    bd_addr_t addr;
    uint8_t   page_scan_mode;
    uint16_t  clock_offset;
    uint32_t  cod;
    enum { NAME_REQUEST, NAME_PENDING, NAME_DONE } state;
} scan_dev_t;
static scan_dev_t scan_devs[MAX_SCAN_DEVICES];
static int        scan_count;

// ---------------------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------------------
static uint8_t sf_bit_for_rate(uint32_t rate) {
    switch (rate) {
        case 44100: return LDAC_SF_44100;
        case 48000: return LDAC_SF_48000;
        case 88200: return LDAC_SF_88200;
        case 96000: return LDAC_SF_96000;
        default:    return 0;
    }
}

static const char *eqmid_name(int e) {
    switch (e) {
        case LDACBT_EQMID_HQ: return "HQ (990/909 kbps)";
        case LDACBT_EQMID_SQ: return "SQ (660/606 kbps)";
        case LDACBT_EQMID_MQ: return "MQ (330/303 kbps)";
        default: return "?";
    }
}

static void queue_reset(void) {
    q_head = q_tail = q_count = 0;
    send_request_pending = false;
}

static void queue_push(const uint8_t *data, uint16_t len, uint32_t ts) {
    if (len > MAX_PACKET_BYTES) return;
    if (q_count == MAX_PACKET_QUEUE) {          // overflow: drop oldest to keep latency bounded
        q_head = (q_head + 1) % MAX_PACKET_QUEUE;
        q_count--;
        stat_dropped++;
    }
    packet_t *p = &queue[q_tail];
    memcpy(p->data, data, len);
    p->len = len;
    p->timestamp = ts;
    q_tail = (q_tail + 1) % MAX_PACKET_QUEUE;
    q_count++;
}

static void request_send(void) {
    if (!streaming || send_request_pending || q_count == 0) return;
    send_request_pending = true;
    a2dp_source_stream_endpoint_request_can_send_now(a2dp_cid, ldac_local_seid);
}

// ---------------------------------------------------------------------------------------------
// encoder + pacing
// ---------------------------------------------------------------------------------------------
static bool encoder_open(void) {
    int max_payload = a2dp_max_media_payload_size(a2dp_cid, ldac_local_seid); // remote MTU - 12 (RTP)
    if (max_payload <= 0) return false;
    int remote_mtu = max_payload + 12;           // full L2CAP media MTU of the headset
    if (remote_mtu < 679) {
        printf("LDAC: the headset's media MTU is only %d bytes (LDAC needs at least 679).\n", remote_mtu);
        return false;
    }
    // libldac wants the full transport MTU and subtracts its own 18 bytes of header overhead.
    // 679 fits a single 2-DH5 baseband packet (same choice as Android/BlueZ).
    ldac_mtu = 679;

    if (ldac_handle == NULL) ldac_handle = ldacBT_get_handle();
    if (ldac_handle == NULL) return false;
    int r = ldacBT_init_handle_encode(ldac_handle, ldac_mtu, ldac_eqmid, LDACBT_CHANNEL_MODE_STEREO,
                                      LDACBT_SMPL_FMT_S16, (int) capture_rate);
    if (r != 0) {
        printf("LDAC: encoder init failed (error 0x%x)\n", ldacBT_get_error_code(ldac_handle));
        return false;
    }
    printf("LDAC: encoder ready, %u Hz, quality %s, transport MTU %d (headset offers %d)\n",
           (unsigned) capture_rate, eqmid_name(ldac_eqmid), ldac_mtu, remote_mtu);
    return true;
}

static void encode_block(const int16_t *pcm) {
    uint8_t stream[MAX_PACKET_BYTES + 64];
    int pcm_used = 0, stream_sz = 0, frame_num = 0;
    int r = ldacBT_encode(ldac_handle, (void *) pcm, &pcm_used, stream, &stream_sz, &frame_num);
    if (r != 0) {
        int err = ldacBT_get_error_code(ldac_handle);
        if (LDACBT_FATAL(err)) {
            printf("LDAC: fatal encoder error 0x%x\n", err);
        }
        return;
    }
    if (stream_sz > 0 && frame_num > 0) {
        uint8_t pkt[MAX_PACKET_BYTES];
        pkt[0] = (uint8_t) (frame_num & 0x0F);   // LDAC media payload header: frame count
        memcpy(&pkt[1], stream, (size_t) stream_sz);
        queue_push(pkt, (uint16_t) (stream_sz + 1), rtp_ts_base + (uint32_t) frames_encoded);
        stat_packets++;
        stat_bytes += (uint32_t) stream_sz;
    }
}

static void audio_timer_handler(btstack_timer_source_t *ts) {
    if (!streaming) return;

    uint32_t now = btstack_run_loop_get_time_ms();
    uint64_t due = ((uint64_t) (now - stream_start_ms)) * capture_rate / 1000u;

    // never let the pacing clock run away: cap catch-up to 100 ms
    if (due > frames_encoded + capture_rate / 10) {
        frames_encoded = due - capture_rate / 20;
        stat_skips++;
    }
    // keep capture backlog (= latency) bounded
    uint32_t avail = cap_available();
    if (avail > capture_rate * 15 / 100) {
        cap_discard(avail - capture_rate * 5 / 100);
    }

    int16_t pcm[LDAC_BLOCK_FRAMES * 2];
    while (frames_encoded + LDAC_BLOCK_FRAMES <= due) {
        if (cap_available() >= LDAC_BLOCK_FRAMES) {
            cap_read(pcm, LDAC_BLOCK_FRAMES);
        } else if (cap_ms_since_data() > 40) {
            // Windows delivers nothing while nothing plays: send silence to keep the link alive
            memset(pcm, 0, sizeof(pcm));
            stat_silence_blocks++;
        } else {
            break;     // short underrun while audio is playing: wait for the next capture chunk
        }
        encode_block(pcm);
        frames_encoded += LDAC_BLOCK_FRAMES;
    }

    request_send();

    btstack_run_loop_set_timer(ts, AUDIO_TICK_MS);
    btstack_run_loop_add_timer(ts);
}

static void stats_timer_handler(btstack_timer_source_t *ts) {
    if (streaming) {
        printf("[stream] %u packets, %.0f kbps avg last 5s, queue %d, dropped %u, silence blocks %u\n",
               stat_packets, stat_bytes * 8.0 / 5000.0, q_count, stat_dropped, stat_silence_blocks);
        stat_bytes = 0;
    }
    btstack_run_loop_set_timer(ts, 5000);
    btstack_run_loop_add_timer(ts);
}

static void start_streaming(void) {
    cap_discard(cap_available());
    queue_reset();
    stream_start_ms = btstack_run_loop_get_time_ms();
    frames_encoded = 0;
    rtp_ts_base = 0;
    stat_packets = stat_bytes = stat_dropped = stat_silence_blocks = stat_skips = 0;
    streaming = true;

    btstack_run_loop_remove_timer(&audio_timer);
    btstack_run_loop_set_timer_handler(&audio_timer, audio_timer_handler);
    btstack_run_loop_set_timer(&audio_timer, AUDIO_TICK_MS);
    btstack_run_loop_add_timer(&audio_timer);

    btstack_run_loop_remove_timer(&stats_timer);
    btstack_run_loop_set_timer_handler(&stats_timer, stats_timer_handler);
    btstack_run_loop_set_timer(&stats_timer, 5000);
    btstack_run_loop_add_timer(&stats_timer);

    printf("Streaming started. Play something on the selected Windows output device.\n");
}

static void stop_streaming(void) {
    streaming = false;
    btstack_run_loop_remove_timer(&audio_timer);
    btstack_run_loop_remove_timer(&stats_timer);
    queue_reset();
}

// ---------------------------------------------------------------------------------------------
// connection management
// ---------------------------------------------------------------------------------------------
static void connect_now(void) {
    ldac_selected = false;
    ldac_seen = false;
    stream_open = false;
    printf("Connecting to %s ...\n", bd_addr_to_str(target_addr));
    uint8_t status = a2dp_source_establish_stream(target_addr, &a2dp_cid);
    if (status != ERROR_CODE_SUCCESS) {
        printf("a2dp_source_establish_stream failed, status 0x%02x\n", status);
    }
}

static void retry_handler(btstack_timer_source_t *ts) {
    (void) ts;
    if (shutdown_triggered) return;
    connect_now();
}

static void schedule_retry(void) {
    if (!auto_retry || shutdown_triggered) {
        if (!shutdown_triggered) {
            printf("Disconnected. Exiting (--no-retry).\n");
            hci_power_control(HCI_POWER_OFF);
            shutdown_triggered = true;
        }
        return;
    }
    printf("Will retry in 4 seconds ... (press Ctrl+C to quit)\n");
    btstack_run_loop_remove_timer(&retry_timer);
    btstack_run_loop_set_timer_handler(&retry_timer, retry_handler);
    btstack_run_loop_set_timer(&retry_timer, 4000);
    btstack_run_loop_add_timer(&retry_timer);
}

// ---------------------------------------------------------------------------------------------
// A2DP events
// ---------------------------------------------------------------------------------------------
static uint32_t get_vendor_id(const uint8_t *i) {
    return (uint32_t) i[0] | ((uint32_t) i[1] << 8) | ((uint32_t) i[2] << 16) | ((uint32_t) i[3] << 24);
}
static uint16_t get_codec_id(const uint8_t *i) { return (uint16_t) (i[4] | (i[5] << 8)); }

static void a2dp_source_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    uint8_t status;
    uint16_t cid;

    switch (hci_event_a2dp_meta_get_subevent_code(packet)) {

        case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
            status = a2dp_subevent_signaling_connection_established_get_status(packet);
            cid = a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
            if (status != ERROR_CODE_SUCCESS) {
                printf("A2DP: connection failed, status 0x%02x\n", status);
                a2dp_cid = 0;
                schedule_retry();
                break;
            }
            a2dp_cid = cid;
            printf("A2DP: signaling connected, reading headset capabilities ...\n");
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CAPABILITY:
            printf("  headset offers: SBC (seid %u)\n",
                   a2dp_subevent_signaling_media_codec_sbc_capability_get_remote_seid(packet));
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AAC_CAPABILITY:
            printf("  headset offers: AAC (seid %u)\n",
                   a2dp_subevent_signaling_media_codec_mpeg_aac_capability_get_remote_seid(packet));
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY: {
            uint8_t remote_seid = a2dp_subevent_signaling_media_codec_other_capability_get_remote_seid(packet);
            uint16_t len = a2dp_subevent_signaling_media_codec_other_capability_get_media_codec_information_len(packet);
            const uint8_t *info = a2dp_subevent_signaling_media_codec_other_capability_get_media_codec_information(packet);
            if (len < 6) break;
            uint32_t vid = get_vendor_id(info);
            uint16_t cid2 = get_codec_id(info);
            if (vid == LDAC_VENDOR_ID && cid2 == LDAC_CODEC_ID && len >= LDAC_INFO_LEN) {
                printf("  headset offers: LDAC (seid %u, rates 0x%02x, channel modes 0x%02x)\n",
                       remote_seid, info[6], info[7]);
                ldac_seen = true;
                uint8_t sf = sf_bit_for_rate(capture_rate);
                if (!(info[6] & sf)) {
                    printf("ERROR: the headset does not accept %u Hz in LDAC.\n", (unsigned) capture_rate);
                    break;
                }
                uint8_t cm = 0;
                if (info[7] & LDAC_CM_STEREO) cm = LDAC_CM_STEREO;
                else if (info[7] & LDAC_CM_DUAL) cm = LDAC_CM_DUAL;
                if (cm == 0) {
                    printf("ERROR: the headset LDAC endpoint has no stereo/dual channel mode.\n");
                    break;
                }
                memcpy(ldac_remote_cfg, ldac_local_caps, 6);
                ldac_remote_cfg[6] = sf;
                ldac_remote_cfg[7] = cm;
                uint8_t st = a2dp_source_set_config_other(a2dp_cid, ldac_local_seid, remote_seid,
                                                          ldac_remote_cfg, LDAC_INFO_LEN);
                if (st == ERROR_CODE_SUCCESS) {
                    ldac_selected = true;
                } else {
                    printf("ERROR: could not select LDAC endpoint, status 0x%02x\n", st);
                }
            } else {
                printf("  headset offers: other codec (vendor 0x%04x, id 0x%04x, seid %u)\n",
                       (unsigned) vid, cid2, remote_seid);
            }
            break;
        }

        case A2DP_SUBEVENT_SIGNALING_CAPABILITIES_COMPLETE:
            if (!ldac_selected) {
                if (!ldac_seen) {
                    printf("ERROR: this headset does not advertise LDAC to this source.\n"
                           "       Check that it is an LDAC model and LDAC is not disabled in its settings.\n");
                }
                printf("Disconnecting.\n");
                a2dp_source_disconnect(a2dp_cid);
            }
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION:
            printf("A2DP: LDAC configuration accepted by the headset.\n");
            break;

        case A2DP_SUBEVENT_STREAM_ESTABLISHED:
            status = a2dp_subevent_stream_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                printf("A2DP: stream setup failed, status 0x%02x\n", status);
                a2dp_source_disconnect(a2dp_cid);
                break;
            }
            printf("A2DP: stream channel open.\n");
            stream_open = true;
            if (!encoder_open()) {
                a2dp_source_disconnect(a2dp_cid);
                break;
            }
            status = a2dp_source_start_stream(a2dp_cid, ldac_local_seid);
            if (status != ERROR_CODE_SUCCESS) printf("start_stream failed, status 0x%02x\n", status);
            break;

        case A2DP_SUBEVENT_STREAM_STARTED:
            start_streaming();
            break;

        case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
            send_request_pending = false;
            if (streaming && q_count > 0) {
                packet_t *p = &queue[q_head];
                uint8_t st = a2dp_source_stream_send_media_payload_rtp(a2dp_cid, ldac_local_seid, 0,
                                                                       p->timestamp, p->data, p->len);
                if (st == ERROR_CODE_SUCCESS) {
                    q_head = (q_head + 1) % MAX_PACKET_QUEUE;
                    q_count--;
                } else if (st != BTSTACK_ACL_BUFFERS_FULL) {
                    // unrecoverable for this packet: drop it
                    q_head = (q_head + 1) % MAX_PACKET_QUEUE;
                    q_count--;
                    stat_dropped++;
                }
                request_send();
            }
            break;

        case A2DP_SUBEVENT_STREAM_SUSPENDED:
            printf("A2DP: stream suspended by headset.\n");
            stop_streaming();
            // try to resume right away
            if (stream_open) a2dp_source_start_stream(a2dp_cid, ldac_local_seid);
            break;

        case A2DP_SUBEVENT_STREAM_RELEASED:
            printf("A2DP: stream released.\n");
            stop_streaming();
            stream_open = false;
            break;

        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            printf("A2DP: disconnected.\n");
            stop_streaming();
            stream_open = false;
            a2dp_cid = 0;
            if (ldac_handle) ldacBT_close_handle(ldac_handle);
            schedule_retry();
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------------------------
// scan mode
// ---------------------------------------------------------------------------------------------
static int scan_find(bd_addr_t a) {
    for (int i = 0; i < scan_count; i++) if (bd_addr_cmp(a, scan_devs[i].addr) == 0) return i;
    return -1;
}

static void scan_next_name(void) {
    for (int i = 0; i < scan_count; i++) {
        if (scan_devs[i].state == NAME_REQUEST) {
            scan_devs[i].state = NAME_PENDING;
            gap_remote_name_request(scan_devs[i].addr, scan_devs[i].page_scan_mode,
                                    scan_devs[i].clock_offset | 0x8000);
            return;
        }
    }
    printf("\nScan cycle done. Looking again ... (Ctrl+C to quit)\n");
    gap_inquiry_start(10);
}

static void scan_handle(uint8_t *packet) {
    bd_addr_t addr;
    switch (hci_event_packet_get_type(packet)) {
        case GAP_EVENT_INQUIRY_RESULT:
            gap_event_inquiry_result_get_bd_addr(packet, addr);
            if (scan_find(addr) >= 0 || scan_count >= MAX_SCAN_DEVICES) break;
            memcpy(scan_devs[scan_count].addr, addr, 6);
            scan_devs[scan_count].page_scan_mode = gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
            scan_devs[scan_count].clock_offset = gap_event_inquiry_result_get_clock_offset(packet);
            scan_devs[scan_count].cod = gap_event_inquiry_result_get_class_of_device(packet);
            scan_devs[scan_count].state = NAME_REQUEST;
            if (gap_event_inquiry_result_get_name_available(packet)) {
                char name[241];
                int n = gap_event_inquiry_result_get_name_len(packet);
                if (n > 240) n = 240;
                memcpy(name, gap_event_inquiry_result_get_name(packet), (size_t) n);
                name[n] = 0;
                printf("Found %s  \"%s\"  (class 0x%06x)\n", bd_addr_to_str(addr), name,
                       (unsigned) scan_devs[scan_count].cod);
                scan_devs[scan_count].state = NAME_DONE;
            } else {
                printf("Found %s  (class 0x%06x) ...\n", bd_addr_to_str(addr), (unsigned) scan_devs[scan_count].cod);
            }
            scan_count++;
            break;
        case GAP_EVENT_INQUIRY_COMPLETE:
            for (int i = 0; i < scan_count; i++)
                if (scan_devs[i].state == NAME_PENDING) scan_devs[i].state = NAME_REQUEST;
            scan_next_name();
            break;
        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
            reverse_bd_addr(&packet[3], addr);
            {
                int i = scan_find(addr);
                if (i >= 0) {
                    scan_devs[i].state = NAME_DONE;
                    if (packet[2] == 0) printf("  %s = \"%s\"\n", bd_addr_to_str(addr), (const char *) &packet[9]);
                }
            }
            scan_next_name();
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------------------------
// HCI events
// ---------------------------------------------------------------------------------------------
static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;

    bd_addr_t local_addr;
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                gap_local_bd_addr(local_addr);
                printf("Bluetooth dongle ready: %s\n", bd_addr_to_str(local_addr));

                btstack_strcpy(tlv_db_path, sizeof(tlv_db_path), "ldac-win_");
                btstack_strcat(tlv_db_path, sizeof(tlv_db_path), bd_addr_to_str_with_delimiter(local_addr, '-'));
                btstack_strcat(tlv_db_path, sizeof(tlv_db_path), ".tlv");
                tlv_impl = btstack_tlv_windows_init_instance(&tlv_context, tlv_db_path);
                btstack_tlv_set_instance(tlv_impl, &tlv_context);
                hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv_impl, &tlv_context));

                if (app_mode == MODE_SCAN) {
                    printf("Scanning for Bluetooth devices. Put the headset in PAIRING mode.\n\n");
                    gap_inquiry_start(10);
                } else if (app_mode == MODE_CONNECT) {
                    connect_now();
                }
            } else if (btstack_event_state_get_state(packet) == HCI_STATE_OFF) {
                btstack_tlv_windows_deinit(&tlv_context);
                if (shutdown_triggered) {
                    btstack_stdin_reset();
                    printf("Bye.\n");
                    exit(0);
                }
            }
            break;

        case HCI_EVENT_PIN_CODE_REQUEST:
            // legacy pairing fallback
            hci_event_pin_code_request_get_bd_addr(packet, local_addr);
            gap_pin_code_response(local_addr, "0000");
            break;

        default:
            break;
    }
    if (app_mode == MODE_SCAN) scan_handle(packet);
}

static void trigger_shutdown(void) {
    printf("\nShutting down ...\n");
    shutdown_triggered = true;
    stop_streaming();
    hci_power_control(HCI_POWER_OFF);
}

// ---------------------------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------------------------
static int a2dp_init(void) {
    hci_set_master_slave_policy(0);               // we stay master of the link
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);

    l2cap_init();
    sdp_init();

    a2dp_source_init();
    a2dp_source_register_packet_handler(&a2dp_source_packet_handler);

    memcpy(ldac_local_cfg, ldac_local_caps, 6);
    ldac_local_cfg[6] = LDAC_SF_48000;
    ldac_local_cfg[7] = LDAC_CM_STEREO;
    avdtp_stream_endpoint_t *ep = a2dp_source_create_stream_endpoint(
        AVDTP_AUDIO, AVDTP_CODEC_NON_A2DP,
        ldac_local_caps, sizeof(ldac_local_caps),
        ldac_local_cfg, sizeof(ldac_local_cfg));
    if (!ep) {
        printf("ERROR: could not create the LDAC stream endpoint.\n");
        return 1;
    }
    ldac_local_seid = avdtp_local_seid(ep);

    static uint8_t sdp_a2dp_source_buffer[150];
    memset(sdp_a2dp_source_buffer, 0, sizeof(sdp_a2dp_source_buffer));
    a2dp_source_create_sdp_record(sdp_a2dp_source_buffer, sdp_create_service_record_handle(),
                                  AVDTP_SOURCE_FEATURE_MASK_PLAYER, NULL, NULL);
    sdp_register_service(sdp_a2dp_source_buffer);

    gap_set_local_name("LDAC Streamer");
    gap_set_class_of_device(0x20010C);            // computer + audio service
    gap_discoverable_control(0);
    gap_connectable_control(1);                   // lets a bonded headset reconnect on its own
    gap_set_bondable_mode(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_ssp_set_auto_accept(1);
    return 0;
}

static void usage(const char *exe) {
    printf("ldac-win - stream Windows audio to a Bluetooth headset with LDAC\n\n"
           "  %s --list-audio                 list Windows playback devices\n"
           "  %s --scan                       find Bluetooth devices (headset must be in pairing mode)\n"
           "  %s --connect AA:BB:CC:DD:EE:FF  connect and stream\n\n"
           "Options for --connect:\n"
           "  --device N       playback device whose sound is captured (default: Windows default)\n"
           "  --quality Q      hq (990 kbps), sq (660 kbps, default) or mq (330 kbps)\n"
           "  --no-retry       exit instead of reconnecting when the link drops\n\n"
           "Requires the USB Bluetooth dongle to use the WinUSB driver (see README, Zadig).\n",
           exe, exe, exe);
}

int main(int argc, const char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list-audio") == 0) {
            return cap_list_devices() < 0 ? 1 : 0;
        } else if (strcmp(argv[i], "--scan") == 0) {
            app_mode = MODE_SCAN;
        } else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            if (!sscanf_bd_addr(argv[++i], target_addr)) {
                printf("Invalid Bluetooth address: %s\n", argv[i]);
                return 1;
            }
            app_mode = MODE_CONNECT;
        } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            audio_device = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc) {
            const char *q = argv[++i];
            if (strcmp(q, "hq") == 0) ldac_eqmid = LDACBT_EQMID_HQ;
            else if (strcmp(q, "sq") == 0) ldac_eqmid = LDACBT_EQMID_SQ;
            else if (strcmp(q, "mq") == 0) ldac_eqmid = LDACBT_EQMID_MQ;
            else { usage(argv[0]); return 1; }
        } else if (strcmp(argv[i], "--no-retry") == 0) {
            auto_retry = false;
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (app_mode == MODE_NONE) {
        usage(argv[0]);
        return 1;
    }

    printf("ldac-win booting up\n");

    if (app_mode == MODE_CONNECT) {
        if (cap_start(audio_device, &capture_rate) != 0) {
            printf("Could not start audio capture.\n");
            return 1;
        }
        printf("Audio capture running at %u Hz.\n", (unsigned) capture_rate);
    }

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_windows_get_instance());

    hci_dump_windows_fs_open("ldac-win_hci.pklg", HCI_DUMP_PACKETLOGGER);
    hci_dump_init(hci_dump_windows_fs_get_instance());

    hci_init(hci_transport_usb_instance(), NULL);

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    btstack_stdin_windows_init();
    btstack_stdin_window_register_ctrl_c_callback(&trigger_shutdown);

    if (a2dp_init() != 0) return 1;

    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
    return 0;
}
