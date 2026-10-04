// audio_capture.h - WASAPI loopback capture (captures what Windows is playing)
#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Prints all active playback devices with their index. Returns number of devices (or -1 on error).
int  cap_list_devices(void);

// Starts capturing the loopback of playback device `device_index` (-1 = Windows default).
// On success returns 0 and stores the device mix sample rate in *rate_hz.
int  cap_start(int device_index, uint32_t *rate_hz);

// Number of stereo frames waiting in the ring buffer.
uint32_t cap_available(void);

// Reads up to `frames` stereo frames (interleaved int16 L,R). Returns frames read.
uint32_t cap_read(int16_t *dst, uint32_t frames);

// Drops `frames` frames from the ring buffer.
void cap_discard(uint32_t frames);

// Milliseconds since the capture thread last received audio data from Windows.
// (Windows delivers nothing while no application is playing sound.)
uint32_t cap_ms_since_data(void);

void cap_stop(void);

#ifdef __cplusplus
}
#endif
#endif
