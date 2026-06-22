/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WEBRTC_AUDIO_FRONTEND_HPP
#define WEBRTC_AUDIO_FRONTEND_HPP

#include <memory>
#include <string>
#include <vector>

namespace omni_agent {

struct WebRtcAudioFrontendConfig {
    int sample_rate = 16000;
    bool highpass_enabled = true;
    bool noise_suppression_enabled = false;
    bool agc_enabled = true;
    int agc_target_level_dbfs = 3;
    int agc_compression_gain_db = 12;
    bool agc_limiter_enabled = true;
};

class WebRtcAudioFrontend {
public:
    WebRtcAudioFrontend();
    ~WebRtcAudioFrontend();

    WebRtcAudioFrontend(const WebRtcAudioFrontend&) = delete;
    WebRtcAudioFrontend& operator=(const WebRtcAudioFrontend&) = delete;

    bool Initialize(const WebRtcAudioFrontendConfig& config, std::string* error = nullptr);
    void Reset();

    bool ProcessChunk(const std::vector<float>& input, std::vector<float>* output);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace omni_agent

#endif  // WEBRTC_AUDIO_FRONTEND_HPP
