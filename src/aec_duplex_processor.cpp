/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * AecDuplexProcessor Implementation
 *
 * Integrates WebRTC APM with SpacemitAudio::AudioDuplex.
 * Provides echo cancellation for full-duplex audio.
 * Barge-in detection should be done at the application layer using VAD.
 */

#include "aec_duplex_processor.hpp"

// WebRTC includes
// Internal AEC3 headers need the library's own platform defines to parse.
#ifndef WEBRTC_POSIX
#define WEBRTC_POSIX 1
#endif
#ifndef WEBRTC_LINUX
#define WEBRTC_LINUX 1
#endif
#ifndef WEBRTC_APM_DEBUG_DUMP
#define WEBRTC_APM_DEBUG_DUMP 0
#endif
#include <webrtc/modules/audio_processing/include/audio_processing.h>
#include <webrtc/api/audio/echo_control.h>
#include <webrtc/api/audio/echo_canceller3_config.h>
#include <webrtc/modules/audio_processing/aec3/echo_canceller3.h>
#include <optional>
#include <sstream>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "audio_duplex.hpp"
#include "echo_null_beamformer.hpp"

// Frame size for WebRTC APM (10ms at 48kHz = 480 samples)
constexpr int kFrameSizeMs = 10;
// Custom AEC3 configuration, tunable at runtime through env AEC3_CFG, e.g.
//   AEC3_CFG=filter_len=20,hyst=4,smooth=0.5,drift=1,init=2.5,num_filters=5,default_delay=5
// (webrtc-audio-processing-2 ships no EchoCanceller3Factory, but EchoCanceller3 is exported.)
namespace {
class Aec3ConfigFactory : public webrtc::EchoControlFactory {
public:
    explicit Aec3ConfigFactory(const webrtc::EchoCanceller3Config& cfg) : cfg_(cfg) {}
    std::unique_ptr<webrtc::EchoControl> Create(int sample_rate_hz, int num_render_channels,
                                                int num_capture_channels) override {
        return std::make_unique<webrtc::EchoCanceller3>(cfg_, std::nullopt, sample_rate_hz,
            static_cast<size_t>(num_render_channels), static_cast<size_t>(num_capture_channels));
    }
private:
    webrtc::EchoCanceller3Config cfg_;
};

bool ParseAec3Env(webrtc::EchoCanceller3Config& cfg, std::string& summary) {
    const char* e = std::getenv("AEC3_CFG");
    if (!e || !*e) return false;
    std::stringstream ss(e); std::string item; std::ostringstream out;
    while (std::getline(ss, item, ',')) {
        const auto eq = item.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = item.substr(0, eq); const double v = std::atof(item.c_str() + eq + 1);
        if (k == "filter_len") { cfg.filter.refined.length_blocks = static_cast<size_t>(v); cfg.filter.coarse.length_blocks = static_cast<size_t>(v); }
        else if (k == "init_len") { cfg.filter.refined_initial.length_blocks = static_cast<size_t>(v); cfg.filter.coarse_initial.length_blocks = static_cast<size_t>(v); }
        else if (k == "hyst") cfg.delay.hysteresis_limit_blocks = static_cast<size_t>(v);
        else if (k == "smooth") { cfg.delay.delay_estimate_smoothing = static_cast<float>(v); cfg.delay.delay_estimate_smoothing_delay_found = static_cast<float>(v); }
        else if (k == "drift") cfg.echo_removal_control.has_clock_drift = (v != 0);
        else if (k == "stable") cfg.echo_removal_control.linear_and_stable_echo_path = (v != 0);
        else if (k == "init") cfg.filter.initial_state_seconds = static_cast<float>(v);
        else if (k == "num_filters") cfg.delay.num_filters = static_cast<size_t>(v);
        else if (k == "default_delay") cfg.delay.default_delay = static_cast<size_t>(v);
        else if (k == "headroom") cfg.delay.delay_headroom_samples = static_cast<size_t>(v);
        else if (k == "change_blocks") cfg.filter.config_change_duration_blocks = static_cast<size_t>(v);
        else continue;
        out << k << "=" << v << " ";
    }
    summary = out.str();
    return true;
}
}  // namespace

constexpr int kDefaultSampleRate = 48000;
constexpr int kFrameSize = kDefaultSampleRate * kFrameSizeMs / 1000;  // 480 samples

namespace {

int DefaultFramesPerBuffer(int sample_rate) {
    return std::max(1, sample_rate * kFrameSizeMs / 1000);
}

// 16-bit WAV (interleaved when multi-channel) written as the stream runs. The header is
// rewritten about once a second, so a session that is killed still leaves a readable file.
class DumpWav {
public:
    DumpWav() = default;
    DumpWav(const DumpWav&) = delete;
    DumpWav& operator=(const DumpWav&) = delete;
    ~DumpWav() { Close(); }

    bool Open(const std::string& path, int sample_rate, int channels = 1) {
        f_ = std::fopen(path.c_str(), "wb");
        if (!f_) return false;
        rate_ = static_cast<uint32_t>(sample_rate);
        channels_ = static_cast<uint16_t>(std::max(1, channels));
        WriteHeader();
        return true;
    }
    bool IsOpen() const { return f_ != nullptr; }
    uint64_t samples() const { return samples_; }

    // frames: samples per channel; data holds frames * channels interleaved values
    void Append(const float* data, size_t frames) {
        if (!f_) return;
        const size_t n = frames * channels_;
        buf_.resize(n);
        for (size_t i = 0; i < n; ++i) {
            buf_[i] = static_cast<int16_t>(std::clamp(data[i], -1.0f, 1.0f) * 32767.0f);
        }
        std::fwrite(buf_.data(), sizeof(int16_t), n, f_);
        samples_ += frames;
        unsynced_ += frames;
        if (unsynced_ >= rate_) {
            WriteHeader();
            unsynced_ = 0;
        }
    }

    void Close() {
        if (!f_) return;
        WriteHeader();
        std::fclose(f_);
        f_ = nullptr;
    }

private:
    void WriteHeader() {
        const long pos = std::ftell(f_);
        const uint32_t data_bytes = static_cast<uint32_t>(
            std::min<uint64_t>(samples_ * 2 * channels_, 0xFFFFFFFFull - 36));
        auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f_); };
        auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f_); };
        std::fseek(f_, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, f_); w32(36 + data_bytes); std::fwrite("WAVEfmt ", 1, 8, f_);
        w32(16); w16(1); w16(channels_); w32(rate_); w32(rate_ * 2 * channels_); w16(2 * channels_); w16(16);
        std::fwrite("data", 1, 4, f_); w32(data_bytes);
        if (pos > 44) std::fseek(f_, pos, SEEK_SET);
        std::fflush(f_);
    }

    FILE* f_ = nullptr;
    uint32_t rate_ = 0;
    uint16_t channels_ = 1;
    uint64_t samples_ = 0;
    uint64_t unsynced_ = 0;
    std::vector<int16_t> buf_;
};

}  // namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

AecDuplexProcessor::AecDuplexProcessor()
    : AecDuplexProcessor(Config()) {}

AecDuplexProcessor::AecDuplexProcessor(const Config& config)
    : config_(config)
    , duplex_(nullptr)
    , apm_(nullptr)
    , is_running_(false)
    , is_playing_(false)
    , audio_callback_(nullptr)
    , raw_audio_callback_(nullptr)
    , history_write_pos_(0)
    , delay_samples_(0) {
    if (config_.capture_channels <= 0) {
        config_.capture_channels = config_.channels;
    }
    if (config_.playback_channels <= 0) {
        config_.playback_channels = config_.channels;
    }
    if (config_.frames_per_buffer <= 0) {
        config_.frames_per_buffer = DefaultFramesPerBuffer(config_.sample_rate);
    }
    config_.speech_channel = std::max(1, config_.speech_channel);
    reference_gain_ = static_cast<float>(std::pow(10.0, config_.reference_gain_db / 20.0));

    // Pre-allocate buffers
    input_int16_.resize(config_.frames_per_buffer);
    output_int16_.resize(config_.frames_per_buffer);
    processed_float_.resize(config_.frames_per_buffer);
    delayed_ref_buffer_.resize(config_.frames_per_buffer);

    // Initialize playback history buffer (150ms @ 48kHz = 7200 samples)
    size_t history_samples = (config_.sample_rate * kMaxDelayMs) / 1000;
    playback_history_.resize(history_samples, 0.0f);

    // Calculate delay in samples
    delay_samples_ = (config_.sample_rate * config_.estimated_delay_ms) / 1000;
    dump_dir_ = config_.dump_dir;
    if (const char* dd = std::getenv("AEC_DUMP_DIR"); dd && dump_dir_.empty()) {
        dump_dir_ = dd;
    }
    if (const char* dd = std::getenv("AEC_DYNAMIC_DELAY")) {
        dynamic_delay_ = (dd[0] == '1' || dd[0] == 'y' || dd[0] == 't');
    }
    if (const char* dm = std::getenv("AEC_DYNAMIC_MARGIN_MS")) {
        dynamic_margin_ms_ = std::atof(dm);
    }
    if (const char* dz = std::getenv("AEC_IDLE_DITHER_DBFS")) {
        const double dbfs = std::atof(dz);
        if (dbfs >= 0.0 || dbfs <= -120.0) idle_dither_amp_ = 0.0f;
        else idle_dither_amp_ = static_cast<float>(std::pow(10.0, dbfs / 20.0));
    }
    if (const char* te = std::getenv("AEC_TRACK_DELAY")) {
        trk_enabled_ = (te[0] == '1' || te[0] == 'y' || te[0] == 't');
    }
    if (const char* tm = std::getenv("AEC_TRACK_MARGIN_MS")) {
        trk_margin_ms_ = std::atof(tm);
    }
    trk_in_.assign(static_cast<size_t>(config_.sample_rate / 4), 0.0f);  // 1 s at fs/4
    trk_ref_.assign(static_cast<size_t>(config_.sample_rate / 4), 0.0f);
    if (const char* qm = std::getenv("AEC_QUEUE_MAX")) {
        const long v = std::atol(qm);
        if (v >= 10 && v <= 100000) queue_max_ = static_cast<size_t>(v);
    }
    if (delay_samples_ >= history_samples) {
        delay_samples_ = history_samples - config_.frames_per_buffer;
    }
}

AecDuplexProcessor::~AecDuplexProcessor() {
    cleanup();
}

// ============================================================================
// Initialization
// ============================================================================

bool AecDuplexProcessor::initialize() {
    std::cout << "[AecDuplex] Initializing..." << std::endl;
    std::cout << "[AecDuplex] Sample rate: " << config_.sample_rate << " Hz" << std::endl;
    std::cout << "[AecDuplex] Frame size: " << config_.frames_per_buffer << " samples" << std::endl;

    if (config_.sample_rate != 16000 && config_.sample_rate != 32000 &&
            config_.sample_rate != 48000) {
        std::cerr << "[AecDuplex] Warning: WebRTC APM is best tested at "
            << "16000/32000/48000 Hz; requested " << config_.sample_rate
            << " Hz" << std::endl;
    }
    if (config_.capture_channels <= 0 || config_.playback_channels <= 0) {
        std::cerr << "[AecDuplex] Invalid channel config: capture="
            << config_.capture_channels << ", playback="
            << config_.playback_channels << std::endl;
        return false;
    }
    if (config_.speech_channel < 1 ||
            config_.speech_channel > config_.capture_channels) {
        std::cerr << "[AecDuplex] speech_channel " << config_.speech_channel
            << " out of range [1, " << config_.capture_channels << "]"
            << std::endl;
        return false;
    }
    if (config_.reference_channel < 0 ||
            config_.reference_channel > config_.capture_channels ||
            config_.reference_channel == config_.speech_channel) {
        std::cerr << "[AecDuplex] reference_channel " << config_.reference_channel
            << " must be 0 (software loopback) or a capture channel in [1, "
            << config_.capture_channels << "] other than speech_channel "
            << config_.speech_channel << std::endl;
        return false;
    }

    // Create WebRTC APM
    webrtc::AudioProcessingBuilder apm_builder;
    {
        webrtc::EchoCanceller3Config aec3_cfg;  // library defaults
        std::string summary;
        if (ParseAec3Env(aec3_cfg, summary)) {
            std::cout << "[AecDuplex] custom AEC3 config: " << summary << std::endl;
            apm_builder.SetEchoControlFactory(std::make_unique<Aec3ConfigFactory>(aec3_cfg));
        }
    }
    rtc::scoped_refptr<webrtc::AudioProcessing> apm_ref = apm_builder.Create();

    if (!apm_ref) {
        std::cerr << "[AecDuplex] Failed to create AudioProcessing instance" << std::endl;
        return false;
    }

    // Store the raw pointer and add a reference to prevent deletion
    apm_ = apm_ref.get();
    apm_->AddRef();

    // Configure APM
    webrtc::AudioProcessing::Config apm_config;

    // Echo cancellation
    apm_config.echo_canceller.enabled = config_.aec_enabled;
    apm_config.echo_canceller.mobile_mode = false;  // Desktop mode for better quality

    // Gain control. AGC1 in fixed-digital mode is a compressor with a limiter:
    // it lifts quiet speech but never above target_level_dbfs, so it cannot clip
    // and it does not chase the noise floor the way the adaptive modes do.
    if (config_.agc_enabled) {
        apm_config.gain_controller1.enabled = true;
        apm_config.gain_controller1.mode =
            webrtc::AudioProcessing::Config::GainController1::kFixedDigital;
        apm_config.gain_controller1.target_level_dbfs = 3;
        apm_config.gain_controller1.compression_gain_db = 12;
        apm_config.gain_controller1.enable_limiter = true;
    }

    // High-pass filter
    apm_config.high_pass_filter.enabled = config_.highpass_enabled;

    // Noise suppression. kLow (the previous hard-coded value) barely touches the
    // 500-2000 Hz fan noise that dominates this enclosure; see the ns_level
    // comment in the header for the measured levels.
    if (config_.ns_enabled) {
        apm_config.noise_suppression.enabled = true;
        const auto level = config_.ns_level;
        apm_config.noise_suppression.level =
            level <= 0 ? webrtc::AudioProcessing::Config::NoiseSuppression::kLow :
            level == 1 ? webrtc::AudioProcessing::Config::NoiseSuppression::kModerate :
            level == 2 ? webrtc::AudioProcessing::Config::NoiseSuppression::kHigh :
            webrtc::AudioProcessing::Config::NoiseSuppression::kVeryHigh;
    }

    // Makeup gain, applied last so it lifts speech without lifting the residual
    // floor that NS already knocked down.
    if (config_.fixed_gain_db != 0.0f) {
        apm_config.gain_controller2.enabled = true;
        apm_config.gain_controller2.fixed_digital.gain_db = config_.fixed_gain_db;
        apm_config.gain_controller2.adaptive_digital.enabled = false;
    }

    apm_->ApplyConfig(apm_config);

    static const char* kNsLevelNames[] = {"kLow", "kModerate", "kHigh", "kVeryHigh"};
    const int ns_idx = std::clamp(config_.ns_level, 0, 3);

    std::cout << "[AecDuplex] WebRTC APM configured:" << std::endl;
    std::cout << "[AecDuplex]   Capture Channels: " << config_.capture_channels << std::endl;
    std::cout << "[AecDuplex]   Playback Channels: " << config_.playback_channels << std::endl;
    std::cout << "[AecDuplex]   Speech Channel: ch" << config_.speech_channel << std::endl;
    std::cout << "[AecDuplex]   Echo Cancellation: " << (config_.aec_enabled ? "ON" : "OFF") << std::endl;
    std::cout << "[AecDuplex]   Noise Suppression: "
        << (config_.ns_enabled ? kNsLevelNames[ns_idx] : "OFF") << std::endl;
    std::cout << "[AecDuplex]   Gain Control: "
        << (config_.agc_enabled ? "AGC1 fixed-digital 3/-12" : "OFF")
        << " fixed_gain=" << config_.fixed_gain_db << " dB" << std::endl;
    std::cout << "[AecDuplex]   High-pass Filter: " << (config_.highpass_enabled ? "ON" : "OFF") << std::endl;
    std::cout << "[AecDuplex]   Delay Compensation: " << config_.estimated_delay_ms << " ms ("
        << delay_samples_ << " samples)" << std::endl;
    if (config_.reference_channel > 0) {
        std::cout << "[AecDuplex]   AEC Reference: capture ch" << config_.reference_channel
            << " (hardware loopback) " << std::showpos << config_.reference_gain_db
            << std::noshowpos << " dB; delay compensation only places the software loopback"
            << std::endl;
    } else {
        std::cout << "[AecDuplex]   AEC Reference: software loopback" << std::endl;
    }

    // Create AudioDuplex
    duplex_ = std::make_unique<SpacemitAudio::AudioDuplex>(
        config_.input_device,
        config_.output_device);

    // Set callback
    duplex_->SetCallbackEx([this](const float* input, float* output, size_t frames,
            int input_channels, int output_channels) {
        onDuplexAudio(input, output, frames, input_channels, output_channels);
    });

    std::cout << "[AecDuplex] Initialization complete" << std::endl;
    return true;
}

void AecDuplexProcessor::cleanup() {
    stop();

    duplex_.reset();

    if (apm_) {
        apm_->Release();
        apm_ = nullptr;
    }
    if (apm_echo_only_) {
        apm_echo_only_->Release();
        apm_echo_only_ = nullptr;
    }
}

bool AecDuplexProcessor::createEchoOnlyApm() {
    webrtc::AudioProcessingBuilder builder;
    webrtc::EchoCanceller3Config aec3_cfg;
    std::string summary;
    if (ParseAec3Env(aec3_cfg, summary)) {
        builder.SetEchoControlFactory(std::make_unique<Aec3ConfigFactory>(aec3_cfg));
    }
    rtc::scoped_refptr<webrtc::AudioProcessing> apm_ref = builder.Create();
    if (!apm_ref) {
        std::cerr << "[AecDuplex] Failed to create the echo-only AudioProcessing instance" << std::endl;
        return false;
    }
    apm_echo_only_ = apm_ref.get();
    apm_echo_only_->AddRef();

    webrtc::AudioProcessing::Config apm_config;
    apm_config.echo_canceller.enabled = true;
    apm_config.echo_canceller.mobile_mode = false;
    apm_echo_only_->ApplyConfig(apm_config);
    std::cout << "[AecDuplex] Echo-only output enabled (AEC3, no NS/AGC/HPF)" << std::endl;
    return true;
}

// ============================================================================
// Lifecycle
// ============================================================================

bool AecDuplexProcessor::start() {
    if (is_running_) return true;

    if (!duplex_) {
        std::cerr << "[AecDuplex] Not initialized" << std::endl;
        return false;
    }

    if (echo_only_callback_ && !apm_echo_only_ && !createEchoOnlyApm()) {
        return false;
    }
    if (config_.echo_null && echo_only_callback_ && !echo_null_) {
        EchoNullBeamformer::Options opt;
        opt.capture_channels = config_.capture_channels;
        opt.first_mic = config_.echo_null_first_channel - 1;
        const int first = config_.echo_null_first_channel;
        if (config_.speech_channel != first ||
                (config_.reference_channel >= first && config_.reference_channel <= first + 2)) {
            // Between playbacks the null passes its first mic through, so KWS must already be
            // listening to that mic; a loopback channel inside the trio is not a microphone.
            std::cerr << "[AecDuplex] echo null ch" << first << "-ch" << (first + 2)
                << " needs speech_channel == ch" << first << " (have ch" << config_.speech_channel
                << ") and the reference channel outside it (have ch" << config_.reference_channel
                << ")" << std::endl;
            return false;
        }
        auto null = std::make_unique<EchoNullBeamformer>();
        if (config_.sample_rate != EchoNullBeamformer::kSampleRate ||
                config_.frames_per_buffer % EchoNullBeamformer::kHop != 0 || !null->Init(opt)) {
            std::cerr << "[AecDuplex] echo null needs 16 kHz, 10 ms frames and capture channels ch"
                << config_.echo_null_first_channel << "-ch" << (config_.echo_null_first_channel + 2)
                << " (have " << config_.sample_rate << " Hz, " << config_.frames_per_buffer
                << " frames, " << config_.capture_channels << " channels)" << std::endl;
            return false;
        }
        echo_null_ = std::move(null);
        std::cout << "[AecDuplex] KWS path: loudspeaker null on ch" << config_.echo_null_first_channel
            << "-ch" << (config_.echo_null_first_channel + 2) << ", learned online during playback (+"
            << 1000 * EchoNullBeamformer::kLatency / EchoNullBeamformer::kSampleRate << " ms)" << std::endl;
    }

    // Start processing thread first
    processing_running_ = true;
    processing_thread_ = std::thread(&AecDuplexProcessor::processingLoop, this);

    if (!duplex_->Start(config_.sample_rate, config_.capture_channels,
                        config_.playback_channels, config_.frames_per_buffer)) {
        std::cerr << "[AecDuplex] Failed to start duplex stream" << std::endl;
        // Stop processing thread on failure
        processing_running_ = false;
        queue_cv_.notify_all();
        if (processing_thread_.joinable()) {
            processing_thread_.join();
        }
        return false;
    }

    is_running_ = true;
    std::cout << "[AecDuplex] Started (async processing enabled)" << std::endl;
    return true;
}

void AecDuplexProcessor::stop() {
    if (!is_running_) return;

    is_running_ = false;

    if (duplex_) {
        duplex_->Stop();
    }

    // Stop processing thread
    processing_running_ = false;
    queue_cv_.notify_all();
    if (processing_thread_.joinable()) {
        processing_thread_.join();
    }

    // Clear any remaining frames in queue
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!audio_queue_.empty()) {
            audio_queue_.pop();
        }
    }

    std::cout << "[AecDuplex] Stopped" << std::endl;
}

// ============================================================================
// Duplex Audio Callback (runs in real-time audio thread)
// ============================================================================

void AecDuplexProcessor::onDuplexAudio(const float* input, float* output,
    size_t frames, int input_channels, int output_channels) {
    // Step 1: Fill output buffer from playback queue (must be fast!)
    fillOutputBuffer(output, frames, output_channels);
    if (idle_dither_amp_ > 0.0f && output && output_channels > 0) {
        bool silent = true;
        const size_t total = frames * static_cast<size_t>(output_channels);
        for (size_t i = 0; i < total && silent; ++i) silent = (output[i] == 0.0f);
        if (silent) {
            for (size_t i = 0; i < frames; ++i) {
                dither_seed_ = dither_seed_ * 1664525u + 1013904223u;
                const float v = ((dither_seed_ >> 8) / 16777216.0f * 2.0f - 1.0f) * idle_dither_amp_;
                for (int ch = 0; ch < output_channels; ++ch) output[i * static_cast<size_t>(output_channels) + ch] = v;
            }
        }
    }

    // Step 2: Save output to playback history ring buffer
    for (size_t i = 0; i < frames; ++i) {
        const float ref = (output && output_channels > 0)
            ? output[i * static_cast<size_t>(output_channels)]
            : 0.0f;
        playback_history_[history_write_pos_] = ref;
        history_write_pos_ = (history_write_pos_ + 1) % playback_history_.size();
    }

    // Step 3: Get delayed reference signal from history
    size_t history_size = playback_history_.size();
    size_t delay_samples = delay_samples_;
    if (dynamic_delay_) {
        const double loop_s = duplex_->GetLoopDelaySeconds();
        if (loop_s > 0.0) {
            long d = std::lround((loop_s + dynamic_margin_ms_ / 1000.0) * config_.sample_rate);
            const long max_d = static_cast<long>(history_size) - static_cast<long>(frames) - 1;
            d = std::clamp<long>(d, 0L, max_d);
            delay_samples = static_cast<size_t>(d);
            last_dynamic_delay_samples_.store(d, std::memory_order_relaxed);
        }
    }
    size_t read_pos = (history_write_pos_ + history_size - delay_samples - frames) % history_size;

    // Step 4: Enqueue input + reference for async processing (no AEC here!)
    if (input && processing_running_) {
        AudioFrame frame;
        frame.input.resize(frames);
        const int speech_idx = std::clamp(config_.speech_channel - 1, 0,
            std::max(0, input_channels - 1));
        for (size_t i = 0; i < frames; ++i) {
            frame.input[i] = input[i * static_cast<size_t>(input_channels) + speech_idx];
        }
        if (raw_audio_callback_ || !dump_dir_.empty() || echo_null_) {
            frame.raw_channels = input_channels;
            frame.raw_input.assign(input, input + frames * static_cast<size_t>(input_channels));
        }
        frame.reference.resize(frames);
        for (size_t i = 0; i < frames; ++i) {
            frame.reference[i] = playback_history_[(read_pos + i) % history_size];
        }
        if (config_.reference_channel > 0 && config_.reference_channel <= input_channels) {
            const size_t ref_idx = static_cast<size_t>(config_.reference_channel - 1);
            frame.hw_reference.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                frame.hw_reference[i] = input[i * static_cast<size_t>(input_channels) + ref_idx] * reference_gain_;
            }
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            // Limit queue size to prevent memory growth. A dropped frame skips
            // both capture and reference, which shifts the echo path seen by
            // AEC3 by one frame; count it so the effect is visible.
            stat_cb_frames_.fetch_add(1, std::memory_order_relaxed);
            const size_t depth = audio_queue_.size();
            if (depth > stat_queue_hwm_.load(std::memory_order_relaxed)) {
                stat_queue_hwm_.store(depth, std::memory_order_relaxed);
            }
            if (depth < queue_max_) {
                audio_queue_.push(std::move(frame));
            } else {
                stat_dropped_frames_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        queue_cv_.notify_one();
    }
}

// ============================================================================
// Processing Thread (runs in non-real-time thread)
// ============================================================================

void AecDuplexProcessor::processingLoop() {
    std::cout << "[AecDuplex] Processing thread started (queue_max=" << queue_max_
        << " dynamic_delay=" << (dynamic_delay_ ? "on" : "off") << " margin_ms=" << dynamic_margin_ms_
        << " idle_dither_amp=" << idle_dither_amp_ << ")" << std::endl;
    size_t win_n = 0; double win_raw = 0, win_apm = 0, win_cb = 0, win_max = 0;
    const bool diag_ = std::getenv("AEC_DIAG") != nullptr;

    DumpWav dump_in, dump_ref, dump_out, dump_echo_only, dump_raw, dump_null;
    bool dumping = false;
    if (!dump_dir_.empty()) {
        const int rate = config_.sample_rate;
        // aec_dump_raw.wav keeps every capture channel (beamforming needs the other mics)
        dumping = dump_raw.Open(dump_dir_ + "/aec_dump_raw.wav", rate, config_.capture_channels) &&
            dump_in.Open(dump_dir_ + "/aec_dump_in.wav", rate) &&
            dump_ref.Open(dump_dir_ + "/aec_dump_ref.wav", rate) &&
            dump_out.Open(dump_dir_ + "/aec_dump_out.wav", rate) &&
            (!apm_echo_only_ || !echo_only_callback_ ||
                dump_echo_only.Open(dump_dir_ + "/aec_dump_echo_only.wav", rate)) &&
            (!echo_null_ || dump_null.Open(dump_dir_ + "/aec_dump_null.wav", rate));
        if (dumping) {
            std::cout << "[AecDuplex] streaming frame-aligned dumps to " << dump_dir_ << std::endl;
        } else {
            std::cerr << "[AecDuplex] cannot write dumps to " << dump_dir_ << ": "
                << std::strerror(errno) << std::endl;
        }
    }

    while (processing_running_) {
        AudioFrame frame;

        // Wait for audio frame
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !audio_queue_.empty() || !processing_running_;
            });

            if (!processing_running_) break;
            if (audio_queue_.empty()) continue;

            frame = std::move(audio_queue_.front());
            audio_queue_.pop();
        }
        const auto t_frame0 = std::chrono::steady_clock::now();

        if (raw_audio_callback_ && !frame.raw_input.empty()) {
            raw_audio_callback_(frame.raw_input.data(),
                                frame.raw_input.size() /
                                    static_cast<size_t>(frame.raw_channels),
                                frame.raw_channels,
                                config_.sample_rate);
        }

        // Process through AEC (now in non-real-time thread, can take longer)
        const auto t_apm0 = std::chrono::steady_clock::now();
        const float* aec_ref = frame.hw_reference.empty() ? frame.reference.data() : frame.hw_reference.data();
        if (apm_) {
            processInput(apm_, frame.input.data(), aec_ref, frame.input.size(), processed_float_);
        }
        bool nulled = false;
        if (apm_echo_only_ && echo_only_callback_) {
            const size_t n = frame.input.size();
            // The speech channel always goes through the echo-only AEC3; that output feeds KWS
            // until the loudspeaker null has learned the echo. From then on KWS takes the null
            // output directly: after the null there is little echo left, but AEC3 still derives
            // its suppression from the reference level and would duck the user's voice during
            // playback (live 2026-09-26: 11 vs 5 wakes while the robot talked, 0 self-triggers
            // in 30 min of TTS either way).
            processInput(apm_echo_only_, frame.input.data(), aec_ref, n, echo_only_float_);
            if (echo_null_ && frame.raw_channels == config_.capture_channels &&
                    frame.raw_input.size() == n * static_cast<size_t>(frame.raw_channels)) {
                null_out_.resize(n);
                null_ref_.resize(n);
                const auto before = echo_null_->stats();
                echo_null_->Process(frame.raw_input.data(), frame.reference.data(), n,
                    null_out_.data(), null_ref_.data());
                const auto& after = echo_null_->stats();
                if (after.engaged != before.engaged || after.relearns != before.relearns) {
                    std::cout << "[AecDuplex] echo null " << (after.relearns != before.relearns ? "relearned" : "engaged")
                        << " after " << after.playback_s << " s of playback" << std::endl;
                }
                nulled = true;
                if (after.engaged) {
                    std::copy(null_out_.begin(), null_out_.end(), echo_only_float_.begin());
                }
            }
        }

        const auto t_apm1 = std::chrono::steady_clock::now();
        if (trk_enabled_.load(std::memory_order_acquire)) {
            // decimate by 4 with a 4-sample average (both streams identically)
            const size_t n4 = frame.input.size() / 4;
            for (size_t i = 0; i < n4; ++i) {
                float si = 0.0f, sr = 0.0f;
                for (size_t k = 0; k < 4; ++k) { si += frame.input[4 * i + k]; sr += frame.reference[4 * i + k]; }
                trk_in_[trk_pos_] = si * 0.25f; trk_ref_[trk_pos_] = sr * 0.25f;
                trk_pos_ = (trk_pos_ + 1) % trk_in_.size();
            }
            if (++trk_frames_ >= 50) { trk_frames_ = 0; trackDelay(); }
        }
        // Call user callback with processed audio
        if (echo_only_callback_ && !echo_only_float_.empty()) {
            echo_only_callback_(echo_only_float_.data(), frame.input.size(), config_.sample_rate);
        }
        if (audio_callback_) {
            audio_callback_(processed_float_.data(), frame.input.size(), config_.sample_rate);
        }
        const auto t_end = std::chrono::steady_clock::now();
        if (dumping) {
            const size_t n = frame.input.size();
            if (!frame.raw_input.empty()) {
                dump_raw.Append(frame.raw_input.data(), n);
            }
            dump_in.Append(frame.input.data(), n);
            dump_ref.Append(frame.reference.data(), n);
            dump_out.Append(processed_float_.data(), n);
            if (dump_echo_only.IsOpen()) {
                dump_echo_only.Append(echo_only_float_.data(), n);
            }
            if (dump_null.IsOpen()) {
                if (!nulled) null_out_.assign(n, 0.0f);  // keep the tracks frame-aligned
                dump_null.Append(null_out_.data(), n);
            }
        }
        // Diagnostics: per-frame cost split into raw-callback (DOA) / APM / user callback (VAD).
        const double us_raw = std::chrono::duration<double, std::micro>(t_apm0 - t_frame0).count();
        const double us_apm = std::chrono::duration<double, std::micro>(t_apm1 - t_apm0).count();
        const double us_cb = std::chrono::duration<double, std::micro>(t_end - t_apm1).count();
        win_n++; win_raw += us_raw; win_apm += us_apm; win_cb += us_cb;
        win_max = std::max(win_max, us_raw + us_apm + us_cb);
        if (diag_ && win_n >= 200) {  // every 2 s of audio (env AEC_DIAG=1)
            size_t depth;
            { std::lock_guard<std::mutex> lock(queue_mutex_); depth = audio_queue_.size(); }
            const double frame_ms = 1000.0 * frame.input.size() / config_.sample_rate;
            std::cout << "[AecDuplex][stat] cb=" << stat_cb_frames_.load()
                << " dropped=" << stat_dropped_frames_.load()
                << " q_now=" << depth << " q_hwm=" << stat_queue_hwm_.load()
                << " per-frame(" << frame_ms << "ms): raw/doa=" << (win_raw / win_n)
                << "us apm=" << (win_apm / win_n) << "us cb/vad=" << (win_cb / win_n)
                << "us max=" << win_max << "us"
                << " dyn_delay=" << (dynamic_delay_ ? 1000.0 * last_dynamic_delay_samples_.load() / config_.sample_rate : -1.0)
                << "ms xruns=" << duplex_->GetXrunCount()
                << " delay=" << (1000.0 * delay_samples_.load() / config_.sample_rate)
                << "ms trk(lag=" << trk_last_lag_ms_.load() << "ms corr=" << trk_last_corr_.load()
                << " updates=" << trk_updates_.load() << ")";
            if (echo_null_) {
                const auto& ns = echo_null_->stats();
                std::cout << " null(engaged=" << ns.engaged << " reduction=" << ns.reduction_db
                    << "dB relearns=" << ns.relearns << " playback=" << ns.playback_s << "s)";
            }
            std::cout << std::endl;
            win_n = 0; win_raw = win_apm = win_cb = 0; win_max = 0;
        }
    }

    if (dumping) {
        std::cout << "[AecDuplex] dumped " << dump_in.samples() << " frame-aligned samples to "
            << dump_dir_ << std::endl;
    }
    std::cout << "[AecDuplex] Processing thread stopped: cb=" << stat_cb_frames_.load()
        << " dropped=" << stat_dropped_frames_.load() << " q_hwm=" << stat_queue_hwm_.load()
        << " queue_max=" << queue_max_ << std::endl;
}

size_t AecDuplexProcessor::fillOutputBuffer(float* output, size_t frames,
                                            int output_channels) {
    if (!output || output_channels <= 0) return 0;
    std::lock_guard<std::mutex> lock(playback_mutex_);

    // 检查是否处于淡出状态
    int fade_frames = fade_out_frames_.load();
    if (fade_frames > 0) {
        // 淡出处理：继续输出当前音频但逐渐降低音量
        size_t samples_written = 0;

        // 如果有音频数据，输出带淡出的音频
        if (!current_playback_.samples.empty() &&
            current_playback_.position < current_playback_.samples.size()) {
            size_t samples_available = current_playback_.samples.size() - current_playback_.position;
            size_t samples_to_copy = std::min(frames, samples_available);

            // 复制并应用淡出增益
            float gain = static_cast<float>(fade_frames) / kFadeOutFrames;
            for (size_t i = 0; i < samples_to_copy; ++i) {
                const float sample =
                    current_playback_.samples[current_playback_.position + i] * gain;
                for (int ch = 0; ch < output_channels; ++ch) {
                    output[i * static_cast<size_t>(output_channels) + ch] = sample;
                }
            }
            samples_written = samples_to_copy;
            current_playback_.position += samples_to_copy;
        }

        // 填充剩余为静音
        if (samples_written < frames) {
            std::memset(output + samples_written * static_cast<size_t>(output_channels),
                        0, (frames - samples_written) *
                            static_cast<size_t>(output_channels) * sizeof(float));
        }

        // 递减淡出计数
        fade_frames--;
        fade_out_frames_.store(fade_frames);

        // 淡出完成，只丢弃当前旧音频；clearPlayback() 之后新入队的音频
        // 需要保留，例如 HID 唤醒提示音。
        if (fade_frames == 0) {
            current_playback_.samples.clear();
            current_playback_.position = 0;
            is_playing_.store(!playback_queue_.empty());
        }

        return samples_written;
    }

    // If current buffer is exhausted, get next from queue
    if (current_playback_.samples.empty() ||
        current_playback_.position >= current_playback_.samples.size()) {
        if (!playback_queue_.empty()) {
            current_playback_ = std::move(playback_queue_.front());
            playback_queue_.pop();
            current_playback_.position = 0;
        } else {
            current_playback_.samples.clear();
            current_playback_.position = 0;
        }
    }

    // Check if we have playback samples
    if (current_playback_.samples.empty() ||
        current_playback_.position >= current_playback_.samples.size()) {
        // No playback, output silence
        std::memset(output, 0,
                    frames * static_cast<size_t>(output_channels) * sizeof(float));
        is_playing_.store(false);
        return 0;
    }

    // Copy samples to output
    size_t samples_available = current_playback_.samples.size() - current_playback_.position;
    size_t samples_to_copy = std::min(frames, samples_available);

    for (size_t i = 0; i < samples_to_copy; ++i) {
        const float sample = current_playback_.samples[current_playback_.position + i];
        for (int ch = 0; ch < output_channels; ++ch) {
            output[i * static_cast<size_t>(output_channels) + ch] = sample;
        }
    }

    // Pad with zeros if needed
    if (samples_to_copy < frames) {
        std::memset(output + samples_to_copy * static_cast<size_t>(output_channels),
                    0, (frames - samples_to_copy) *
                        static_cast<size_t>(output_channels) * sizeof(float));
    }

    current_playback_.position += samples_to_copy;

    // Update playing state: still playing if queue has data or current buffer has remaining data
    bool still_playing = !playback_queue_.empty() ||
        (current_playback_.position < current_playback_.samples.size());
    is_playing_.store(still_playing);

    return samples_to_copy;
}

void AecDuplexProcessor::processInput(webrtc::AudioProcessing* apm, const float* input,
        const float* output_ref, size_t frames, std::vector<float>& processed) {
    webrtc::StreamConfig stream_config(config_.sample_rate, config_.channels);

    // Ensure buffers are sized correctly
    if (input_int16_.size() < frames) {
        input_int16_.resize(frames);
        output_int16_.resize(frames);
    }
    if (processed.size() < frames) {
        processed.resize(frames);
    }

    // Convert output to int16 for reverse stream (playback reference)
    for (size_t i = 0; i < frames; ++i) {
        float sample = std::clamp(output_ref[i], -1.0f, 1.0f);
        output_int16_[i] = static_cast<int16_t>(
            std::clamp(sample * 32768.0f, -32768.0f, 32767.0f));
    }

    // Process reverse stream (playback reference for AEC)
    apm->ProcessReverseStream(output_int16_.data(), stream_config,
        stream_config, output_int16_.data());

    // Convert input to int16
    for (size_t i = 0; i < frames; ++i) {
        float sample = std::clamp(input[i], -1.0f, 1.0f);
        input_int16_[i] = static_cast<int16_t>(
            std::clamp(sample * 32768.0f, -32768.0f, 32767.0f));
    }

    // Process stream (applies AEC, NS, AGC)
    apm->ProcessStream(input_int16_.data(), stream_config,
                        stream_config, input_int16_.data());

    // Convert back to float
    for (size_t i = 0; i < frames; ++i) {
        processed[i] = static_cast<float>(input_int16_[i]) / 32768.0f;
    }
}

// ============================================================================
// Playback Queue
// ============================================================================

void AecDuplexProcessor::enqueuePlayback(const std::vector<float>& samples, int sample_rate) {
    enqueuePlayback(samples.data(), samples.size(), sample_rate);
}

void AecDuplexProcessor::enqueuePlayback(const float* samples, size_t count, int sample_rate) {
    if (!samples || count == 0) return;

    std::vector<float> resampled;

    // Resample to 48kHz if needed
    if (sample_rate != config_.sample_rate) {
        resampled = resample(std::vector<float>(samples, samples + count),
            sample_rate, config_.sample_rate);
    } else {
        resampled.assign(samples, samples + count);
    }

    PlaybackBuffer buffer;
    buffer.samples = std::move(resampled);
    buffer.position = 0;

    {
        std::lock_guard<std::mutex> lock(playback_mutex_);
        playback_queue_.push(std::move(buffer));
        is_playing_.store(true);  // Set playing state immediately when audio is queued
    }
}

void AecDuplexProcessor::clearPlayback() {
    std::lock_guard<std::mutex> lock(playback_mutex_);
    while (!playback_queue_.empty()) {
        playback_queue_.pop();
    }
    // 不立即清空当前正在写出的音频，设置淡出标志，避免 AEC 参考信号突变。
    // 淡出会在 fillOutputBuffer() 中执行。
    if (!current_playback_.samples.empty() &&
            current_playback_.position < current_playback_.samples.size()) {
        fade_out_frames_.store(kFadeOutFrames);
    } else {
        current_playback_.samples.clear();
        current_playback_.position = 0;
        fade_out_frames_.store(0);
        is_playing_.store(false);
    }
}

size_t AecDuplexProcessor::getPlaybackQueueSize() const {
    std::lock_guard<std::mutex> lock(playback_mutex_);
    return playback_queue_.size();
}

// ============================================================================
// Utilities
// ============================================================================

std::vector<float> AecDuplexProcessor::resample(const std::vector<float>& input,
        int from_rate, int to_rate) {
    if (from_rate == to_rate || input.empty()) {
        return input;
    }

    double ratio = static_cast<double>(to_rate) / from_rate;
    size_t output_size = static_cast<size_t>(input.size() * ratio);
    std::vector<float> output(output_size);

    // Simple linear interpolation resampling
    for (size_t i = 0; i < output_size; ++i) {
        double src_pos = i / ratio;
        size_t src_idx = static_cast<size_t>(src_pos);
        double frac = src_pos - src_idx;

        if (src_idx + 1 < input.size()) {
            output[i] = static_cast<float>(
                input[src_idx] * (1.0 - frac) + input[src_idx + 1] * frac);
        } else if (src_idx < input.size()) {
            output[i] = input[src_idx];
        } else {
            output[i] = 0.0f;
        }
    }

    return output;
}


// ============================================================================
// Coarse external delay tracker (processing thread)
// ============================================================================
void AecDuplexProcessor::trackDelay() {
    const long long until_ms = trk_until_ms_.load(std::memory_order_relaxed);
    if (until_ms != 0 && steadyNowMs() > until_ms) {
        trk_enabled_.store(false, std::memory_order_relaxed);
        std::cout << "[AecDuplex] delay tracking frozen at " << (1000.0 * delay_samples_.load() / config_.sample_rate)
            << " ms (last lag " << trk_last_lag_ms_.load() << " ms, corr " << trk_last_corr_.load() << ")" << std::endl;
        return;
    }
    const size_t N = trk_in_.size();          // 1 s at fs/4
    const size_t W = N * 3 / 4;                // 0.75 s analysis window
    const int fs4 = config_.sample_rate / 4;
    const int max_lag = fs4 * 3 / 20;          // +-150 ms search range
    // linearize the most recent W samples (oldest first)
    std::vector<float> in(W), ref(W);
    for (size_t i = 0; i < W; ++i) {
        const size_t idx = (trk_pos_ + N - W + i) % N;
        in[i] = trk_in_[idx]; ref[i] = trk_ref_[idx];
    }
    double e_ref = 0.0, e_in = 0.0, m_ref = 0.0, m_in = 0.0;
    for (size_t i = 0; i < W; ++i) { m_ref += ref[i]; m_in += in[i]; }
    m_ref /= W; m_in /= W;
    for (size_t i = 0; i < W; ++i) { ref[i] -= static_cast<float>(m_ref); in[i] -= static_cast<float>(m_in); e_ref += ref[i] * ref[i]; e_in += in[i] * in[i]; }
    if (e_ref / W < 1e-6 || e_in / W < 1e-8) return;   // far end quiet (< -60 dBFS rms) or no capture
    // normalized cross-correlation: c[L] = sum ref[t] * in[t + L], L in [-max_lag, +max_lag]
    int best_lag = 0; double best = 0.0;
    for (int L = -max_lag; L <= max_lag; ++L) {
        double acc = 0.0;
        const size_t t0 = L < 0 ? static_cast<size_t>(-L) : 0;
        const size_t t1 = L > 0 ? W - static_cast<size_t>(L) : W;
        for (size_t t = t0; t < t1; ++t) acc += ref[t] * in[t + L];
        if (acc > best) { best = acc; best_lag = L; }
    }
    const double corr = best / (std::sqrt(e_ref * e_in) + 1e-12);
    trk_last_lag_ms_.store(1000.0 * best_lag / fs4);
    trk_last_corr_.store(corr);
    if (corr < 0.12) return;                   // no reliable echo peak
    const double residual_ms = 1000.0 * best_lag / fs4;        // echo later than reference by this much
    double step_ms = residual_ms - trk_margin_ms_.load(std::memory_order_relaxed);  // want residual == margin
    if (std::fabs(step_ms) < 4.0) return;                      // dead band
    step_ms = std::clamp(step_ms, -20.0, 20.0);
    const long cur = static_cast<long>(delay_samples_.load());
    const long max_d = static_cast<long>(playback_history_.size()) - static_cast<long>(config_.frames_per_buffer) - 1;
    long next = cur + std::lround(step_ms * config_.sample_rate / 1000.0);
    next = std::clamp(next, 0L, max_d);
    delay_samples_.store(static_cast<size_t>(next));
    trk_updates_.fetch_add(1);
}
