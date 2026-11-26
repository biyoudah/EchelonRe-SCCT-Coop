#pragma once

#include <cstdint>

// Modernized voice capture/codec stack: miniaudio (48 kHz mono) + RNNoise + Opus (24 kbps, 20 ms, DTX)
// plus a 40-60 ms jitter buffer for remote Opus playback.

bool VoiceAudioStartCapture(int device_id, volatile long* out_peak_0_to_100);
void VoiceAudioStopCapture();
int VoiceAudioGetCaptureDeviceCount();
bool VoiceAudioGetCaptureDeviceName(int index, wchar_t* out_name, int out_cap);

// Encodes next queued 20 ms (960 samples @ 48 kHz) denoised frame to Opus (<= max_out bytes).
// Returns encoded byte length (>0), 0 if no full frame ready, or <0 on error.
int VoiceAudioPopEncodedOpus(uint8_t* out_opus, int max_out);

// Jitter-buffered remote Opus decode (40-60 ms target depth).
bool VoiceAudioPushRemoteOpus(uint16_t seq, const uint8_t* opus_data, int opus_len);
int VoiceAudioPopPlaybackPcm(int16_t* out_pcm_960, int max_samples);
