// audio_capture.cpp - WASAPI shared-mode loopback capture into a lock-free ring buffer.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <string.h>
#include <atomic>

#include "audio_capture.h"

// PKEY_Device_FriendlyName defined by hand to avoid linking issues across toolchains
static const PROPERTYKEY MY_PKEY_FriendlyName =
    {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

static const GUID MY_CLSID_MMDeviceEnumerator =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID MY_IID_IMMDeviceEnumerator =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID MY_IID_IAudioClient =
    {0x1CB9AD4C, 0xDBFA, 0x4c32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID MY_IID_IAudioCaptureClient =
    {0xC8ADBD64, 0xE71E, 0x48a0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};

// ---- ring buffer (single producer = capture thread, single consumer = BTstack thread) ----
#define RING_FRAMES (1u << 16)   // 65536 stereo frames (~1.3 s at 48 kHz)
static int16_t ring[RING_FRAMES * 2];
static std::atomic<uint64_t> ring_head(0);  // written by producer
static std::atomic<uint64_t> ring_tail(0);  // written by consumer

static std::atomic<bool>     running(false);
static std::atomic<uint64_t> last_data_ms(0);
static HANDLE                thread_handle = NULL;

struct CaptureCtx {
    int      device_index;
    HANDLE   ready_event;
    int      result;
    uint32_t rate;
};
static CaptureCtx ctx;

static void ring_write(const int16_t *src, uint32_t frames) {
    uint64_t head = ring_head.load(std::memory_order_relaxed);
    uint64_t tail = ring_tail.load(std::memory_order_acquire);
    uint32_t space = RING_FRAMES - (uint32_t)(head - tail);
    if (frames > space) frames = space;   // overflow: drop newest
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t idx = (uint32_t)((head + i) & (RING_FRAMES - 1));
        ring[idx * 2]     = src[i * 2];
        ring[idx * 2 + 1] = src[i * 2 + 1];
    }
    ring_head.store(head + frames, std::memory_order_release);
}

extern "C" uint32_t cap_available(void) {
    return (uint32_t)(ring_head.load(std::memory_order_acquire) - ring_tail.load(std::memory_order_relaxed));
}

extern "C" uint32_t cap_read(int16_t *dst, uint32_t frames) {
    uint32_t avail = cap_available();
    if (frames > avail) frames = avail;
    uint64_t tail = ring_tail.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t idx = (uint32_t)((tail + i) & (RING_FRAMES - 1));
        dst[i * 2]     = ring[idx * 2];
        dst[i * 2 + 1] = ring[idx * 2 + 1];
    }
    ring_tail.store(tail + frames, std::memory_order_release);
    return frames;
}

extern "C" void cap_discard(uint32_t frames) {
    uint32_t avail = cap_available();
    if (frames > avail) frames = avail;
    ring_tail.store(ring_tail.load(std::memory_order_relaxed) + frames, std::memory_order_release);
}

extern "C" uint32_t cap_ms_since_data(void) {
    uint64_t last = last_data_ms.load();
    if (last == 0) return 0xFFFFFFFFu;
    return (uint32_t)(GetTickCount64() - last);
}

// ---- helpers ----
static bool format_is_float(const WAVEFORMATEX *wf) {
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE *e = (const WAVEFORMATEXTENSIBLE *)wf;
        return e->SubFormat.Data1 == 3; // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    }
    return false;
}

static inline int16_t clip16(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 32767.0f);
}

// Converts one packet of arbitrary-format audio into stereo int16.
static uint32_t convert_to_s16(const BYTE *data, uint32_t frames, const WAVEFORMATEX *wf,
                               bool is_float, bool silent, int16_t *out) {
    uint32_t ch = wf->nChannels;
    uint32_t bits = wf->wBitsPerSample;
    uint32_t bytes_per_sample = bits / 8;
    if (silent || data == NULL) {
        memset(out, 0, frames * 2 * sizeof(int16_t));
        return frames;
    }
    for (uint32_t i = 0; i < frames; i++) {
        const BYTE *frame = data + (size_t)i * wf->nBlockAlign;
        float l = 0, r = 0;
        for (uint32_t c = 0; c < 2; c++) {
            uint32_t cc = (c < ch) ? c : 0;   // mono -> duplicate
            const BYTE *p = frame + cc * bytes_per_sample;
            float v = 0;
            if (is_float && bits == 32) {
                v = *(const float *)p;
            } else if (!is_float && bits == 16) {
                v = (*(const int16_t *)p) / 32768.0f;
            } else if (!is_float && bits == 24) {
                int32_t s = (p[0] << 8) | (p[1] << 16) | (p[2] << 24);
                v = (s >> 8) / 8388608.0f;
            } else if (!is_float && bits == 32) {
                v = (*(const int32_t *)p) / 2147483648.0f;
            }
            if (c == 0) l = v; else r = v;
        }
        out[i * 2]     = clip16(l);
        out[i * 2 + 1] = clip16(r);
    }
    return frames;
}

static IMMDevice *get_device(IMMDeviceEnumerator *en, int index) {
    IMMDevice *dev = NULL;
    if (index < 0) {
        en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        return dev;
    }
    IMMDeviceCollection *coll = NULL;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))) return NULL;
    coll->Item((UINT)index, &dev);
    coll->Release();
    return dev;
}

extern "C" int cap_list_devices(void) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool com_ok = SUCCEEDED(hr);
    IMMDeviceEnumerator *en = NULL;
    if (FAILED(CoCreateInstance(MY_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                MY_IID_IMMDeviceEnumerator, (void **)&en))) {
        if (com_ok) CoUninitialize();
        return -1;
    }
    IMMDevice *def = NULL;
    LPWSTR def_id = NULL;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def))) def->GetId(&def_id);

    IMMDeviceCollection *coll = NULL;
    UINT count = 0;
    en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
    if (coll) coll->GetCount(&count);
    printf("Playback devices (use --device N):\n");
    for (UINT i = 0; i < count; i++) {
        IMMDevice *d = NULL;
        coll->Item(i, &d);
        if (!d) continue;
        LPWSTR id = NULL;
        d->GetId(&id);
        IPropertyStore *props = NULL;
        d->OpenPropertyStore(STGM_READ, &props);
        PROPVARIANT name;
        PropVariantInit(&name);
        if (props) props->GetValue(MY_PKEY_FriendlyName, &name);
        bool is_def = (def_id && id && wcscmp(def_id, id) == 0);
        printf("  [%u] %ls%s\n", i, name.vt == VT_LPWSTR ? name.pwszVal : L"(unknown)",
               is_def ? "   <-- Windows default" : "");
        PropVariantClear(&name);
        if (props) props->Release();
        if (id) CoTaskMemFree(id);
        d->Release();
    }
    if (def_id) CoTaskMemFree(def_id);
    if (def) def->Release();
    if (coll) coll->Release();
    en->Release();
    if (com_ok) CoUninitialize();
    return (int)count;
}

static DWORD WINAPI capture_thread(LPVOID) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *client = NULL;
    IAudioCaptureClient *cap = NULL;
    WAVEFORMATEX *wf = NULL;
    int16_t *tmp = NULL;
    uint32_t tmp_frames = 0;

    ctx.result = -1;
    if (FAILED(hr)) goto done_signal;
    if (FAILED(CoCreateInstance(MY_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                MY_IID_IMMDeviceEnumerator, (void **)&en))) {
        printf("WASAPI: could not create device enumerator\n");
        goto done_signal;
    }
    dev = get_device(en, ctx.device_index);
    if (!dev) { printf("WASAPI: playback device not found (try --list-audio)\n"); goto done_signal; }
    if (FAILED(dev->Activate(MY_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client))) {
        printf("WASAPI: could not activate audio client\n");
        goto done_signal;
    }
    if (FAILED(client->GetMixFormat(&wf))) { printf("WASAPI: GetMixFormat failed\n"); goto done_signal; }

    {
        bool is_float = format_is_float(wf);
        bool fmt_ok = (is_float && wf->wBitsPerSample == 32) ||
                      (!is_float && (wf->wBitsPerSample == 16 || wf->wBitsPerSample == 24 || wf->wBitsPerSample == 32));
        if (!fmt_ok) { printf("WASAPI: unsupported mix format (%u bit)\n", wf->wBitsPerSample); goto done_signal; }
        uint32_t rate = wf->nSamplesPerSec;
        if (rate != 44100 && rate != 48000 && rate != 88200 && rate != 96000) {
            printf("The device runs at %u Hz. LDAC needs 44100, 48000, 88200 or 96000 Hz.\n"
                   "Set it in: Windows Settings > Sound > (device) > Properties > Advanced.\n", rate);
            goto done_signal;
        }

        // 200 ms buffer, loopback
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                      2000000 /* 200ms in 100ns units */, 0, wf, NULL))) {
            printf("WASAPI: loopback Initialize failed\n");
            goto done_signal;
        }
        if (FAILED(client->GetService(MY_IID_IAudioCaptureClient, (void **)&cap))) {
            printf("WASAPI: GetService failed\n");
            goto done_signal;
        }
        tmp_frames = 16384;
        tmp = (int16_t *)malloc(tmp_frames * 2 * sizeof(int16_t));
        if (FAILED(client->Start())) { printf("WASAPI: Start failed\n"); goto done_signal; }

        ctx.rate = rate;
        ctx.result = 0;
        SetEvent(ctx.ready_event);

        timeBeginPeriod(1);
        while (running.load()) {
            Sleep(5);
            UINT32 pkt = 0;
            while (SUCCEEDED(cap->GetNextPacketSize(&pkt)) && pkt > 0) {
                BYTE *data = NULL;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &flags, NULL, NULL))) break;
                uint32_t done = 0;
                while (done < frames) {
                    uint32_t n = frames - done;
                    if (n > tmp_frames) n = tmp_frames;
                    convert_to_s16(data ? data + (size_t)done * wf->nBlockAlign : NULL, n, wf, is_float,
                                   (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0, tmp);
                    ring_write(tmp, n);
                    done += n;
                }
                last_data_ms.store(GetTickCount64());
                cap->ReleaseBuffer(frames);
            }
        }
        timeEndPeriod(1);
        client->Stop();
    }
    goto cleanup;

done_signal:
    SetEvent(ctx.ready_event);
cleanup:
    if (tmp) free(tmp);
    if (wf) CoTaskMemFree(wf);
    if (cap) cap->Release();
    if (client) client->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    if (SUCCEEDED(hr)) CoUninitialize();
    return 0;
}

extern "C" int cap_start(int device_index, uint32_t *rate_hz) {
    if (running.load()) return -1;
    ring_head.store(0);
    ring_tail.store(0);
    last_data_ms.store(0);
    ctx.device_index = device_index;
    ctx.ready_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    ctx.result = -1;
    running.store(true);
    thread_handle = CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
    if (!thread_handle) { running.store(false); return -1; }
    WaitForSingleObject(ctx.ready_event, 5000);
    CloseHandle(ctx.ready_event);
    if (ctx.result != 0) {
        running.store(false);
        WaitForSingleObject(thread_handle, 2000);
        CloseHandle(thread_handle);
        thread_handle = NULL;
        return -1;
    }
    *rate_hz = ctx.rate;
    return 0;
}

extern "C" void cap_stop(void) {
    if (!running.load()) return;
    running.store(false);
    if (thread_handle) {
        WaitForSingleObject(thread_handle, 2000);
        CloseHandle(thread_handle);
        thread_handle = NULL;
    }
}
