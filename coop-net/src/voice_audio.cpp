#include "voice_audio.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#include "miniaudio.h"

#include "opus.h"
#include "rnnoise.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kRnnFrame = 480;     // 10 ms @ 48 kHz
constexpr int kOpusFrame = 960;    // 20 ms @ 48 kHz (2x RNNoise frames)
constexpr int kOpusBitrate = 24000;
constexpr int kMaxOpusBytes = 256;
constexpr int kCaptureRingCap = kOpusFrame * 8;
constexpr int kJitterSlots = 6;    // 120 ms max, 2-3 frames (40-60 ms) prebuffer

struct JitterSlot {
    bool valid = false;
    uint16_t seq = 0;
    int len = 0;
    uint8_t data[kMaxOpusBytes]{};
};

CRITICAL_SECTION g_va_lock{};
bool g_va_lock_init = false;

ma_context g_ma_ctx{};
bool g_ma_ctx_init = false;
ma_device g_ma_dev{};
bool g_ma_dev_init = false;

DenoiseState* g_rnn = nullptr;
OpusEncoder* g_enc = nullptr;
OpusDecoder* g_dec = nullptr;
volatile long* g_peak_ptr = nullptr;

float g_cap_ring[kCaptureRingCap]{};
int g_cap_read = 0;
int g_cap_write = 0;
int g_cap_count = 0;

JitterSlot g_jitter[kJitterSlots]{};
uint16_t g_next_play_seq = 0;
bool g_jitter_primed = false;

void EnsureLock() {
    if (!g_va_lock_init) {
        InitializeCriticalSection(&g_va_lock);
        g_va_lock_init = true;
    }
}

void EnsureCodecsLocked() {
    if (!g_rnn) {
        g_rnn = rnnoise_create(nullptr);
    }
    if (!g_enc) {
        int err = 0;
        g_enc = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &err);
        if (g_enc && err == OPUS_OK) {
            opus_encoder_ctl(g_enc, OPUS_SET_BITRATE(kOpusBitrate));
            opus_encoder_ctl(g_enc, OPUS_SET_DTX(1));
            opus_encoder_ctl(g_enc, OPUS_SET_INBAND_FEC(1));
            opus_encoder_ctl(g_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        }
    }
    if (!g_dec) {
        int err = 0;
        g_dec = opus_decoder_create(kSampleRate, 1, &err);
    }
}

void MaCaptureCallback(ma_device*, void*, const void* input, ma_uint32 frame_count) {
    if (!input || frame_count == 0) {
        return;
    }
    const auto* f = static_cast<const float*>(input);
    float peak = 0.0f;
    for (ma_uint32 i = 0; i < frame_count; ++i) {
        const float a = std::fabs(f[i]);
        if (a > peak) {
            peak = a;
        }
    }
    int scaled = static_cast<int>(peak * 200.0f);
    if (scaled > 100) {
        scaled = 100;
    }
    if (g_peak_ptr) {
        InterlockedExchange(g_peak_ptr, scaled);
    }

    EnterCriticalSection(&g_va_lock);
    for (ma_uint32 i = 0; i < frame_count; ++i) {
        if (g_cap_count >= kCaptureRingCap) {
            g_cap_read = (g_cap_read + 1) % kCaptureRingCap;
            --g_cap_count;
        }
        g_cap_ring[g_cap_write] = f[i];
        g_cap_write = (g_cap_write + 1) % kCaptureRingCap;
        ++g_cap_count;
    }
    LeaveCriticalSection(&g_va_lock);
}

}  // namespace

bool VoiceAudioStartCapture(int device_id, volatile long* out_peak_0_to_100) {
    EnsureLock();
    EnterCriticalSection(&g_va_lock);
    if (g_ma_dev_init) {
        g_peak_ptr = out_peak_0_to_100;
        LeaveCriticalSection(&g_va_lock);
        return true;
    }
    EnsureCodecsLocked();
    g_peak_ptr = out_peak_0_to_100;
    g_cap_read = 0;
    g_cap_write = 0;
    g_cap_count = 0;

    if (!g_ma_ctx_init) {
        if (ma_context_init(nullptr, 0, nullptr, &g_ma_ctx) != MA_SUCCESS) {
            LeaveCriticalSection(&g_va_lock);
            return false;
        }
        g_ma_ctx_init = true;
    }

    ma_device_info* cap_infos = nullptr;
    ma_uint32 cap_count = 0;
    ma_context_get_devices(&g_ma_ctx, nullptr, nullptr, &cap_infos, &cap_count);
    if (cap_count == 0) {
        LeaveCriticalSection(&g_va_lock);
        return false;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = 1;
    cfg.sampleRate = kSampleRate;
    cfg.periodSizeInFrames = kRnnFrame;
    cfg.dataCallback = MaCaptureCallback;
    if (device_id >= 0 && static_cast<ma_uint32>(device_id) < cap_count && cap_infos) {
        cfg.capture.pDeviceID = &cap_infos[device_id].id;
    }

    if (ma_device_init(&g_ma_ctx, &cfg, &g_ma_dev) != MA_SUCCESS) {
        LeaveCriticalSection(&g_va_lock);
        return false;
    }
    if (ma_device_start(&g_ma_dev) != MA_SUCCESS) {
        ma_device_uninit(&g_ma_dev);
        LeaveCriticalSection(&g_va_lock);
        return false;
    }
    g_ma_dev_init = true;
    LeaveCriticalSection(&g_va_lock);
    return true;
}

void VoiceAudioStopCapture() {
    EnsureLock();
    EnterCriticalSection(&g_va_lock);
    if (g_ma_dev_init) {
        ma_device_uninit(&g_ma_dev);
        g_ma_dev_init = false;
    }
    if (g_peak_ptr) {
        InterlockedExchange(g_peak_ptr, 0);
    }
    g_cap_read = 0;
    g_cap_write = 0;
    g_cap_count = 0;
    LeaveCriticalSection(&g_va_lock);
}

int VoiceAudioGetCaptureDeviceCount() {
    EnsureLock();
    EnterCriticalSection(&g_va_lock);
    if (!g_ma_ctx_init) {
        if (ma_context_init(nullptr, 0, nullptr, &g_ma_ctx) != MA_SUCCESS) {
            LeaveCriticalSection(&g_va_lock);
            return 0;
        }
        g_ma_ctx_init = true;
    }
    ma_device_info* cap_infos = nullptr;
    ma_uint32 cap_count = 0;
    ma_context_get_devices(&g_ma_ctx, nullptr, nullptr, &cap_infos, &cap_count);
    LeaveCriticalSection(&g_va_lock);
    return static_cast<int>(cap_count);
}

bool VoiceAudioGetCaptureDeviceName(int index, wchar_t* out_name, int out_cap) {
    if (!out_name || out_cap <= 0 || index < 0) {
        return false;
    }
    out_name[0] = L'\0';
    EnsureLock();
    EnterCriticalSection(&g_va_lock);
    if (!g_ma_ctx_init) {
        if (ma_context_init(nullptr, 0, nullptr, &g_ma_ctx) != MA_SUCCESS) {
            LeaveCriticalSection(&g_va_lock);
            return false;
        }
        g_ma_ctx_init = true;
    }
    ma_device_info* cap_infos = nullptr;
    ma_uint32 cap_count = 0;
    ma_context_get_devices(&g_ma_ctx, nullptr, nullptr, &cap_infos, &cap_count);
    bool ok = false;
    if (static_cast<ma_uint32>(index) < cap_count && cap_infos) {
        ok = MultiByteToWideChar(CP_UTF8, 0, cap_infos[index].name, -1, out_name, out_cap) > 0;
    }
    LeaveCriticalSection(&g_va_lock);
    return ok;
}

int VoiceAudioPopEncodedOpus(uint8_t* out_opus, int max_out) {
    if (!out_opus || max_out <= 0) {
        return -1;
    }
    EnsureLock();
    EnterCriticalSection(&g_va_lock);
    EnsureCodecsLocked();
    if (!g_enc || g_cap_count < kOpusFrame) {
        LeaveCriticalSection(&g_va_lock);
        return 0;
    }
    float pcm[kOpusFrame]{};
    for (int i = 0; i < kOpusFrame; ++i) {
        pcm[i] = g_cap_ring[g_cap_read];
        g_cap_read = (g_cap_read + 1) % kCaptureRingCap;
        --g_cap_count;
    }

    if (g_rnn) {
        for (int half = 0; half < 2; ++half) {
            float rnn_buf[kRnnFrame]{};
            for (int i = 0; i < kRnnFrame; ++i) {
                rnn_buf[i] = pcm[half * kRnnFrame + i] * 32767.0f;
            }
            rnnoise_process_frame(g_rnn, rnn_buf, rnn_buf);
            for (int i = 0; i < kRnnFrame; ++i) {
                pcm[half * kRnnFrame + i] = std::max(-1.0f, std::min(1.0f, rnn_buf[i] / 32767.0f));
            }
        }
    }

    const int nbytes = opus_encode_float(g_enc, pcm, kOpusFrame, out_opus, max_out);
    LeaveCriticalSection(&g_va_lock);
    return nbytes;
}

bool VoiceAudioPushRemoteOpus(uint16_t seq, const uint8_t* opus_data, int opus_len) {
    if (!opus_data || opus_len <= 0 || opus_len > kMaxOpusBytes) {
        return false;
    }
    EnsureLock();
    EnterCriticalSection(&g_va_lock);
    if (!g_jitter_primed) {
        g_next_play_seq = seq;
    }
    JitterSlot& slot = g_jitter[seq % kJitterSlots];
    slot.valid = true;
    slot.seq = seq;
    slot.len = opus_len;
    std::memcpy(slot.data, opus_data, static_cast<size_t>(opus_len));

    int buffered = 0;
    for (const auto& s : g_jitter) {
        if (s.valid) {
            ++buffered;
        }
    }
    if (!g_jitter_primed && buffered >= 2) {
        g_jitter_primed = true;  // 40 ms pre-buffer target (2 x 20 ms frames)
    }
    LeaveCriticalSection(&g_va_lock);
    return true;
}

int VoiceAudioPopPlaybackPcm(int16_t* out_pcm_960, int max_samples) {
    if (!out_pcm_960 || max_samples < kOpusFrame) {
        return 0;
    }
    EnsureLock();
    EnterCriticalSection(&g_va_lock);
    EnsureCodecsLocked();
    if (!g_dec || !g_jitter_primed) {
        LeaveCriticalSection(&g_va_lock);
        return 0;
    }
    JitterSlot& slot = g_jitter[g_next_play_seq % kJitterSlots];
    int decoded = 0;
    if (slot.valid && slot.seq == g_next_play_seq) {
        decoded = opus_decode(g_dec, slot.data, slot.len, out_pcm_960, kOpusFrame, 0);
        slot.valid = false;
    } else {
        // PLC (packet loss concealment) for missing frame
        decoded = opus_decode(g_dec, nullptr, 0, out_pcm_960, kOpusFrame, 0);
    }
    ++g_next_play_seq;
    LeaveCriticalSection(&g_va_lock);
    return decoded > 0 ? decoded : 0;
}
