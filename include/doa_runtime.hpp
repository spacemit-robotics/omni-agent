/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DOA_RUNTIME_HPP
#define DOA_RUNTIME_HPP

#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "doa_service.h"

namespace omni_agent {

struct DoaRuntimeConfig {
    bool enabled = false;
    int sample_rate = 16000;
    int capture_channels = 1;
    int speech_channel = 1;  // 1-based capture channel for VAD/ASR/AEC.
    float side_m = 0.063f;
    std::string positions_spec;
    std::vector<int> pick;  // 1-based capture channels feeding DOA mics.
    float azimuth_offset_deg = 0.0f;
    float max_avg_seconds = 3.0f;
    float confidence_threshold = 0.1f;
    float margin_threshold = 0.6f;
    float quality_threshold = 0.0f;
    float closure_threshold_samples = 0.0f;
    float closure_threshold_fraction = 0.3f;
};

bool ParseIntList(const std::string& spec, std::vector<int>* out,
    std::string* error);

bool ParseMicPositions(const std::string& spec,
    std::vector<SpacemitAudio::MicrophonePosition>* out,
    std::string* error);

class DoaRuntime {
public:
    bool Initialize(const DoaRuntimeConfig& config, std::ostream& err);
    bool enabled() const { return enabled_; }
    void Reset();

    bool ProcessInterleaved(const float* interleaved, size_t frames,
                            int capture_channels);

    bool GetLatest(SpacemitAudio::MultiSoundLocatorResult* out) const;
    bool GetLatestValid(SpacemitAudio::MultiSoundLocatorResult* out) const;
    float GetAverageAzimuth() const;
    float GetAverageResultantLength() const;
    int GetResultCount() const;
    std::string ChannelMapString() const;

private:
    DoaRuntimeConfig config_;
    bool enabled_ = false;
    int mic_count_ = 0;
    std::vector<int> pick_zero_based_;
    std::vector<float> selected_;
    std::unique_ptr<SpacemitAudio::MultiSoundLocator> locator_;

    mutable std::mutex result_mutex_;
    bool has_result_ = false;
    bool has_valid_result_ = false;
    SpacemitAudio::MultiSoundLocatorResult latest_;
    SpacemitAudio::MultiSoundLocatorResult latest_valid_;
};

}  // namespace omni_agent

#endif  // DOA_RUNTIME_HPP
