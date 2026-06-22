/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "webrtc_audio_frontend.hpp"

#include <api/scoped_refptr.h>
#include <webrtc/modules/audio_processing/include/audio_processing.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace omni_agent {

namespace {

constexpr int kFrameMs = 10;

int ClampInt(int value, int min_value, int max_value) {
    return std::max(min_value, std::min(max_value, value));
}

int FrameSamples(int sample_rate) {
    return std::max(1, sample_rate * kFrameMs / 1000);
}

int16_t FloatToInt16(float sample) {
    sample = std::clamp(sample, -1.0f, 1.0f);
    return static_cast<int16_t>(
        std::clamp(sample * 32768.0f, -32768.0f, 32767.0f));
}

float Int16ToFloat(int16_t sample) {
    return static_cast<float>(sample) / 32768.0f;
}

}  // namespace

struct WebRtcAudioFrontend::Impl {
    WebRtcAudioFrontendConfig config;
    rtc::scoped_refptr<webrtc::AudioProcessing> apm;
    std::vector<float> pending;
    std::vector<int16_t> frame_i16;
    std::vector<int16_t> processed_i16;
    int frame_samples = 160;

    bool ProcessFrame(const float* input, std::vector<float>* output) {
        if (!apm || !input || !output) {
            return false;
        }

        for (int i = 0; i < frame_samples; ++i) {
            frame_i16[static_cast<size_t>(i)] = FloatToInt16(input[i]);
        }

        webrtc::StreamConfig stream_config(config.sample_rate, 1);
        const int rc = apm->ProcessStream(frame_i16.data(), stream_config,
            stream_config, processed_i16.data());
        if (rc != 0) {
            return false;
        }

        const size_t old_size = output->size();
        output->resize(old_size + static_cast<size_t>(frame_samples));
        for (int i = 0; i < frame_samples; ++i) {
            (*output)[old_size + static_cast<size_t>(i)] =
                Int16ToFloat(processed_i16[static_cast<size_t>(i)]);
        }
        return true;
    }
};

WebRtcAudioFrontend::WebRtcAudioFrontend()
    : impl_(std::make_unique<Impl>()) {}

WebRtcAudioFrontend::~WebRtcAudioFrontend() = default;

bool WebRtcAudioFrontend::Initialize(
        const WebRtcAudioFrontendConfig& config, std::string* error) {
    if (config.sample_rate <= 0) {
        if (error) {
            *error = "sample_rate must be > 0";
        }
        return false;
    }

    impl_->config = config;
    impl_->frame_samples = FrameSamples(config.sample_rate);
    impl_->frame_i16.assign(static_cast<size_t>(impl_->frame_samples), 0);
    impl_->processed_i16.assign(static_cast<size_t>(impl_->frame_samples), 0);
    impl_->pending.clear();

    impl_->apm = webrtc::AudioProcessingBuilder().Create();
    if (!impl_->apm) {
        if (error) {
            *error = "failed to create WebRTC AudioProcessing";
        }
        return false;
    }

    webrtc::AudioProcessing::Config apm_config;
    apm_config.high_pass_filter.enabled = config.highpass_enabled;
    apm_config.noise_suppression.enabled = config.noise_suppression_enabled;
    apm_config.noise_suppression.level =
        webrtc::AudioProcessing::Config::NoiseSuppression::kLow;

    apm_config.gain_controller1.enabled = config.agc_enabled;
    apm_config.gain_controller1.mode =
        webrtc::AudioProcessing::Config::GainController1::kAdaptiveDigital;
    apm_config.gain_controller1.target_level_dbfs =
        ClampInt(config.agc_target_level_dbfs, 0, 31);
    apm_config.gain_controller1.compression_gain_db =
        ClampInt(config.agc_compression_gain_db, 0, 90);
    apm_config.gain_controller1.enable_limiter = config.agc_limiter_enabled;
    apm_config.gain_controller1.analog_gain_controller.enabled = false;
    apm_config.gain_controller2.enabled = false;

    impl_->apm->ApplyConfig(apm_config);
    return true;
}

void WebRtcAudioFrontend::Reset() {
    if (!impl_) {
        return;
    }
    WebRtcAudioFrontendConfig config = impl_->config;
    std::string error;
    Initialize(config, &error);
}

bool WebRtcAudioFrontend::ProcessChunk(
        const std::vector<float>& input, std::vector<float>* output) {
    if (!output) {
        return false;
    }
    output->clear();
    if (!impl_ || !impl_->apm) {
        *output = input;
        return true;
    }
    if (input.empty()) {
        return true;
    }

    impl_->pending.insert(impl_->pending.end(), input.begin(), input.end());

    const size_t frame = static_cast<size_t>(impl_->frame_samples);
    size_t offset = 0;
    while (impl_->pending.size() - offset >= frame) {
        if (!impl_->ProcessFrame(impl_->pending.data() + offset, output)) {
            return false;
        }
        offset += frame;
    }

    if (offset > 0) {
        impl_->pending.erase(impl_->pending.begin(),
            impl_->pending.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    return true;
}

}  // namespace omni_agent
