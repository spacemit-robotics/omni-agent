/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * AecDuplexProcessor - AEC processor using AudioDuplex
 *
 * Integrates WebRTC APM with SpacemitAudio::AudioDuplex for echo cancellation.
 * Uses the full-duplex stream for automatic time alignment between
 * microphone input and speaker output.
 *
 * Usage:
 *   AecDuplexProcessor processor;
 *   processor.setAudioCallback([](const float* data, size_t frames) {
 *       // Process echo-cancelled audio
 *   });
 *   processor.initialize();
 *   processor.start();
 *
 *   // Enqueue TTS audio for playback
 *   processor.enqueuePlayback(tts_samples, tts_sample_rate);
 */

#ifndef AEC_DUPLEX_PROCESSOR_HPP
#define AEC_DUPLEX_PROCESSOR_HPP

#include <vector>
#include <algorithm>
#include <cmath>
#include <queue>
#include <mutex>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <condition_variable>
#include <utility>
#include <cstdint>
#include <cstdlib>

// Forward declarations
namespace webrtc {
class AudioProcessing;
}

namespace SpacemitAudio {
class AudioDuplex;
}

/**
 * AEC Full-Duplex Processor
 *
 * Combines SpacemitAudio::AudioDuplex with WebRTC APM for echo cancellation.
 * Barge-in detection should be done at the application layer using VAD.
 */
class EchoNullBeamformer;

class AecDuplexProcessor {
public:
    // Callback for processed (echo-cancelled) audio
    using AudioCallback = std::function<void(const float* data, size_t frames, int sample_rate)>;
    using RawAudioCallback = std::function<void(const float* interleaved, size_t frames,
                                                int channels, int sample_rate)>;

    struct Config {
        // Audio settings
        int sample_rate = 48000;           // 48kHz recommended for AEC
        int channels = 1;                  // Legacy: sets both capture/playback channels
        int capture_channels = 0;          // 0 = use channels
        int playback_channels = 0;         // 0 = use channels
        int speech_channel = 1;            // 1-based capture channel for AEC/VAD/ASR
        int frames_per_buffer = 480;       // 10ms @ 48kHz
        int input_device = -1;             // -1 for default
        int output_device = -1;            // -1 for default

        // WebRTC APM settings
        bool aec_enabled = true;           // Echo cancellation
        bool ns_enabled = true;            // Noise suppression
        // NS aggressiveness: 0=kLow 1=kModerate 2=kHigh 3=kVeryHigh.
        //
        // Measured on the dog (16 kHz, raw mic ch2, fan on, far-field playback of
        // 004_zh_selling_sausages so speech sits ~14 dB above the fan):
        //   level      noise floor   speech    SNR
        //   kLow        -40.5 dBFS   -20.4    20.2 dB
        //   kModerate   -46.4        -19.9    26.5 dB
        //   kHigh       -52.2        -20.0    32.1 dB
        //   kVeryHigh   -55.4        -20.1    35.2 dB
        // Loud speech survives every level (speech moves <1.2 dB) while the floor
        // drops 22 dB. Quiet speech does not: on a weaker stimulus (p90 -26.7
        // dBFS) kHigh cost 11.8 dB of speech for 18.5 dB of noise, and utterance
        // hit-rate fell from 9/28 at kModerate to 2/28 at kHigh (p=0.02).
        // kModerate is the default: most of the noise reduction, and it stays
        // safe when the talker is quiet or far away.
        int ns_level = 1;                  // kModerate
        bool agc_enabled = true;           // Automatic gain control (AGC1, fixed digital)
        bool highpass_enabled = true;      // High-pass filter
        // AGC2 fixed digital gain in dB, applied after NS/AGC1 and before the
        // APM limiter. 0 disables it. This is makeup gain only: it does not
        // track the noise floor, so it cannot pump up silence. Measured neutral
        // for recognition (1.4-5.0% CER either way), so it stays off.
        float fixed_gain_db = 0.0f;

        // AEC delay compensation
        int estimated_delay_ms = 50;       // Estimated system delay for AEC reference alignment

        // Debug: stream frame-aligned WAVs (aec_dump_{raw,in,ref,out,echo_only,null}.wav) into
        // this existing directory while running. Empty = off (AEC_DUMP_DIR is still honoured).
        std::string dump_dir;

        // Echo-only (KWS) path: once an online null on the device's own loudspeaker has learned
        // the echo (three consecutive raw mics from this 1-based capture channel), KWS takes the
        // null output instead of the speech channel's echo-only AEC3 output. The ASR path is
        // unchanged. 16 kHz only; adds 22 ms to the KWS path. See echo_null_beamformer.hpp.
        bool echo_null = false;
        int echo_null_first_channel = 2;

        // AEC3 reference. 0 = the samples written to the loudspeaker (software loopback, placed
        // by estimated_delay_ms and the delay tracker). n > 0 = 1-based capture channel carrying
        // the device's own loopback, scaled by reference_gain_db (SPV firmware 2026-09-28: ch1,
        // ~12 dB below the mics). The hardware loopback shares the mics' clock, so its lead is
        // fixed (2.4 ms) and AEC3's linear filter converges: offline, full AEC3 removed 21.5 dB
        // of echo with it vs 8.4 dB with the software loopback, whose delay wanders because the
        // USB endpoints are unsynchronised. The null gate, the delay tracker and
        // aec_dump_ref.wav keep the software loopback; the hardware one is in aec_dump_raw.wav.
        int reference_channel = 0;
        float reference_gain_db = 12.0f;
    };

    AecDuplexProcessor();
    explicit AecDuplexProcessor(const Config& config);
    ~AecDuplexProcessor();

    // Non-copyable
    AecDuplexProcessor(const AecDuplexProcessor&) = delete;
    AecDuplexProcessor& operator=(const AecDuplexProcessor&) = delete;

    // -------------------------------------------------------------------------
    // Initialization
    // -------------------------------------------------------------------------

    /**
     * Initialize the processor
     * @return true on success
     */
    bool initialize();

    /**
     * Clean up resources
     */
    void cleanup();

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    bool start();
    void stop();
    bool isRunning() const { return is_running_.load(); }

    // Warm-up controls (see voice_chat_aec startup): coarse delay tracking on/off with
    // its target margin, an explicit reference delay, and the current value.
    // until_s > 0: tracking switches itself off that many seconds after this call.
    void setDelayTracking(bool on, double margin_ms, double until_s = 0.0) {
        // Called from the control thread while the processing thread polls trk_enabled_:
        // publish the parameters first, then the flag (release pairs with the acquire load).
        trk_margin_ms_.store(margin_ms, std::memory_order_relaxed);
        trk_until_ms_.store((on && until_s > 0.0)
            ? steadyNowMs() + static_cast<long long>(until_s * 1000.0) : 0,
            std::memory_order_relaxed);
        trk_enabled_.store(on, std::memory_order_release);
    }
    void setDelayMs(double ms) {
        const long d = std::lround(ms * config_.sample_rate / 1000.0);
        const long max_d = static_cast<long>(playback_history_.size()) - static_cast<long>(config_.frames_per_buffer) - 1;
        delay_samples_.store(static_cast<size_t>(std::clamp(d, 0L, std::max(0L, max_d))));
    }
    double delayMs() const { return 1000.0 * static_cast<double>(delay_samples_.load()) / config_.sample_rate; }

    // -------------------------------------------------------------------------
    // Callbacks
    // -------------------------------------------------------------------------

    /**
     * Set callback for processed audio
     * Called from processing thread (not audio thread) with echo-cancelled samples
     */
    void setAudioCallback(AudioCallback callback) { audio_callback_ = std::move(callback); }
    void setRawAudioCallback(RawAudioCallback callback) { raw_audio_callback_ = std::move(callback); }
    /**
     * Optional second output: the speech channel through AEC only (no NS/AGC/HPF),
     * for detectors trained on unprocessed audio such as the KWS model.
     * Set after initialize() and before start(); runs a second AEC3 instance.
     */
    void setEchoOnlyAudioCallback(AudioCallback callback) { echo_only_callback_ = std::move(callback); }

    // -------------------------------------------------------------------------
    // Playback Queue
    // -------------------------------------------------------------------------

    /**
     * Enqueue audio for playback
     * Audio will be resampled to 48kHz if needed
     *
     * @param samples Audio samples (float, [-1.0, 1.0])
     * @param sample_rate Input sample rate
     */
    void enqueuePlayback(const std::vector<float>& samples, int sample_rate);

    /**
     * Enqueue audio from raw pointer
     */
    void enqueuePlayback(const float* samples, size_t count, int sample_rate);

    /**
     * Clear queued playback and fade out the currently playing buffer.
     */
    void clearPlayback();

    /**
     * Check if currently playing
     */
    bool isPlaying() const { return is_playing_.load(); }

    /**
     * Get playback queue size
     */
    size_t getPlaybackQueueSize() const;

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    const Config& getConfig() const { return config_; }

    // -------------------------------------------------------------------------
    // Utilities
    // -------------------------------------------------------------------------

    /**
     * Resample audio
     */
    static std::vector<float> resample(const std::vector<float>& input,
                                        int from_rate, int to_rate);

private:
    // Full-duplex callback (runs in real-time audio thread)
    void onDuplexAudio(const float* input, float* output, size_t frames,
        int input_channels, int output_channels);

    // Processing thread main loop (runs in non-real-time thread)
    void processingLoop();

    // Audio frame for async processing queue
    struct AudioFrame {
        std::vector<float> input;
        std::vector<float> reference;
        std::vector<float> hw_reference;   // scaled capture loopback; empty = AEC3 uses `reference`
        std::vector<float> raw_input;
        int raw_channels = 0;
    };

    // Playback buffer
    struct PlaybackBuffer {
        std::vector<float> samples;
        size_t position = 0;
    };

    // Fill output buffer from playback queue
    size_t fillOutputBuffer(float* output, size_t frames, int output_channels);

    // Process input through one APM instance into `processed`
    void processInput(webrtc::AudioProcessing* apm, const float* input, const float* output_ref,
        size_t frames, std::vector<float>& processed);
    bool createEchoOnlyApm();

    // Configuration
    Config config_;

    // SpacemitAudio duplex stream
    std::unique_ptr<SpacemitAudio::AudioDuplex> duplex_;

    // WebRTC APM
    webrtc::AudioProcessing* apm_;
    webrtc::AudioProcessing* apm_echo_only_ = nullptr;
    std::unique_ptr<EchoNullBeamformer> echo_null_;   // KWS path front end (config_.echo_null)

    // State
    std::atomic<bool> is_running_;
    std::atomic<bool> is_playing_;

    // Processing thread (for async AEC + user callback)
    std::thread processing_thread_;
    std::atomic<bool> processing_running_{false};

    // Audio frame queue (audio thread -> processing thread)
    std::queue<AudioFrame> audio_queue_;
    // Diagnostics: real-time callback frames, frames dropped because the async
    // queue was full, queue high-water mark. Printed by processingLoop().
    std::atomic<uint64_t> stat_cb_frames_{0};
    std::atomic<uint64_t> stat_dropped_frames_{0};
    std::atomic<uint64_t> stat_queue_hwm_{0};
    // Max queued frames before the RT callback drops (env AEC_QUEUE_MAX, default 100).
    size_t queue_max_ = 100;
    // Env AEC_DYNAMIC_DELAY=1: derive the reference delay per callback from PortAudio
    // timing (DAC time - ADC time) instead of the fixed estimated_delay_ms.
    // AEC_DYNAMIC_MARGIN_MS (default -20) keeps the reference ahead of the echo.
    bool dynamic_delay_ = false;
    // Env AEC_TRACK_DELAY=1: coarse external delay tracker. Every 0.5 s it cross-correlates
    // the reference handed to APM with the capture (both decimated to 4 kHz) and steps
    // delay_samples_ (<= 10 ms per update) so that the echo stays AEC_TRACK_MARGIN_MS
    // (default 30) behind the reference. Absorbs the USB device's onset delay ramp.
    std::atomic<bool> trk_enabled_{false};
    std::atomic<long long> trk_until_ms_{0};   // steady_clock ms; 0 = no deadline
    std::atomic<double> trk_margin_ms_{30.0};
    static long long steadyNowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::vector<float> trk_in_, trk_ref_;
    size_t trk_pos_ = 0;
    unsigned trk_frames_ = 0;
    std::atomic<double> trk_last_lag_ms_{0.0};
    std::atomic<double> trk_last_corr_{0.0};
    std::atomic<long> trk_updates_{0};
    void trackDelay();
    // Env AEC_IDLE_DITHER_DBFS (e.g. -50): replace digital silence on the output with
    // low-level noise so the USB device never idles its playback path (its internal
    // latency was observed to ramp ~45 ms over the first seconds of each playback).
    float idle_dither_amp_ = 0.00316228f;  // default -50 dBFS; env AEC_IDLE_DITHER_DBFS=0 disables
    uint32_t dither_seed_ = 12345u;
    double dynamic_margin_ms_ = -20.0;
    std::atomic<long> last_dynamic_delay_samples_{0};
    // Diagnostics: when env AEC_DUMP_DIR is set, the speech-channel input, the
    // delayed software reference and the processed output are recorded
    // frame-aligned and written as 16-bit wavs on stop().
    std::string dump_dir_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    // Playback queue
    std::queue<PlaybackBuffer> playback_queue_;
    PlaybackBuffer current_playback_;
    mutable std::mutex playback_mutex_;

    // Callbacks
    AudioCallback audio_callback_;
    RawAudioCallback raw_audio_callback_;
    AudioCallback echo_only_callback_;

    // Temporary buffers (pre-allocated to avoid allocation in callback)
    std::vector<int16_t> input_int16_;
    std::vector<int16_t> output_int16_;
    std::vector<float> processed_float_;
    std::vector<float> echo_only_float_;
    std::vector<float> null_out_, null_ref_;

    // Playback history ring buffer for AEC delay compensation
    static constexpr size_t kMaxDelayMs = 400;  // Max supported delay (raised 150->400 ms: real SPV loop delay ~330 ms)
    std::vector<float> playback_history_;
    size_t history_write_pos_ = 0;
    std::atomic<size_t> delay_samples_{0};  // written by the delay tracker, read by the RT callback
    float reference_gain_ = 1.0f;           // linear config_.reference_gain_db
    std::vector<float> delayed_ref_buffer_;  // Pre-allocated buffer for delayed reference

    // Fade-out for smooth playback stop (避免 AEC 参考信号突变)
    std::atomic<int> fade_out_frames_{0};
    static constexpr int kFadeOutFrames = 5;  // 淡出 5 帧 (~50ms @ 480 samples/frame)
};

#endif  // AEC_DUPLEX_PROCESSOR_HPP
