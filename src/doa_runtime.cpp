/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "doa_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace omni_agent {

namespace {

std::string Trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::vector<std::string> Split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        out.push_back(Trim(item));
    }
    return out;
}

bool ParseFloat(const std::string& s, float* out) {
    if (!out) return false;
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') return false;
    *out = v;
    return true;
}

std::vector<int> DefaultPick(int capture_channels, int mic_count) {
    std::vector<int> pick;
    if (capture_channels == mic_count) {
        for (int i = 1; i <= mic_count; ++i) pick.push_back(i);
    } else if (capture_channels >= 4 && mic_count == 3) {
        pick = {2, 3, 4};
    }
    return pick;
}

}  // namespace

bool ParseIntList(const std::string& spec, std::vector<int>* out,
    std::string* error) {
    if (!out) return false;
    out->clear();
    if (Trim(spec).empty()) return true;
    for (const auto& item : Split(spec, ',')) {
        if (item.empty()) {
            if (error) *error = "empty item in integer list";
            return false;
        }
        char* end = nullptr;
        long v = std::strtol(item.c_str(), &end, 10);
        if (end == item.c_str() || *end != '\0') {
            if (error) *error = "invalid integer: " + item;
            return false;
        }
        out->push_back(static_cast<int>(v));
    }
    return true;
}

bool ParseMicPositions(const std::string& spec,
    std::vector<SpacemitAudio::MicrophonePosition>* out,
    std::string* error) {
    if (!out) return false;
    out->clear();
    if (Trim(spec).empty()) return true;

    for (const auto& mic : Split(spec, ';')) {
        const auto parts = Split(mic, ',');
        if (parts.size() != 2 && parts.size() != 3) {
            if (error) *error = "position must be x,y or x,y,z: " + mic;
            return false;
        }
        SpacemitAudio::MicrophonePosition p;
        if (!ParseFloat(parts[0], &p.x) || !ParseFloat(parts[1], &p.y)) {
            if (error) *error = "invalid position: " + mic;
            return false;
        }
        if (parts.size() == 3 && !ParseFloat(parts[2], &p.z)) {
            if (error) *error = "invalid position: " + mic;
            return false;
        }
        out->push_back(p);
    }
    return true;
}

bool DoaRuntime::Initialize(const DoaRuntimeConfig& config, std::ostream& err) {
    config_ = config;
    enabled_ = config.enabled;
    locator_.reset();
    selected_.clear();
    pick_zero_based_.clear();
    has_result_ = false;
    has_valid_result_ = false;
    latest_ = {};
    latest_valid_ = {};

    if (!enabled_) return true;

    if (config_.sample_rate <= 0) {
        err << "DOA sample rate must be > 0\n";
        return false;
    }
    if (config_.capture_channels <= 0) {
        err << "DOA capture channels must be > 0\n";
        return false;
    }
    if (config_.speech_channel < 1 ||
            config_.speech_channel > config_.capture_channels) {
        err << "speech channel " << config_.speech_channel
            << " out of range [1, " << config_.capture_channels << "]\n";
        return false;
    }

    SpacemitAudio::MultiSoundLocatorConfig loc_cfg;
    std::vector<SpacemitAudio::MicrophonePosition> positions;
    if (!config_.positions_spec.empty()) {
        std::string error;
        if (!ParseMicPositions(config_.positions_spec, &positions, &error)) {
            err << "invalid DOA positions: " << error << "\n";
            return false;
        }
        loc_cfg.microphones = positions;
    } else {
        if (config_.side_m <= 0.0f) {
            err << "DOA side length must be > 0\n";
            return false;
        }
        loc_cfg = SpacemitAudio::MultiSoundLocator::CreateEquilateralTriangleConfig(
            config_.side_m);
    }
    mic_count_ = static_cast<int>(loc_cfg.microphones.size());
    if (mic_count_ != 3) {
        err << "DOA expects exactly 3 microphones, got " << mic_count_ << "\n";
        return false;
    }

    std::vector<int> pick = config_.pick.empty()
        ? DefaultPick(config_.capture_channels, mic_count_)
        : config_.pick;
    if (static_cast<int>(pick.size()) != mic_count_) {
        err << "DOA pick must have exactly " << mic_count_ << " entries\n";
        return false;
    }
    std::vector<bool> seen(static_cast<size_t>(config_.capture_channels), false);
    for (int ch : pick) {
        if (ch < 1 || ch > config_.capture_channels) {
            err << "DOA pick channel " << ch << " out of range [1, "
                << config_.capture_channels << "]\n";
            return false;
        }
        if (seen[static_cast<size_t>(ch - 1)]) {
            err << "DOA pick repeats capture channel " << ch << "\n";
            return false;
        }
        seen[static_cast<size_t>(ch - 1)] = true;
        pick_zero_based_.push_back(ch - 1);
    }
    config_.pick = pick;

    loc_cfg.sample_rate = config_.sample_rate;
    loc_cfg.frame_size = 512;
    loc_cfg.avg_frames = 4;
    loc_cfg.max_avg_seconds = config_.max_avg_seconds;
    loc_cfg.confidence_threshold = config_.confidence_threshold;
    loc_cfg.margin_threshold = config_.margin_threshold;
    loc_cfg.quality_threshold = config_.quality_threshold;
    loc_cfg.min_signal_rms = config_.min_signal_rms;
    loc_cfg.closure_threshold_samples = config_.closure_threshold_samples;
    loc_cfg.closure_threshold_fraction = config_.closure_threshold_fraction;
    loc_cfg.azimuth_offset_deg = config_.azimuth_offset_deg;

    locator_ = std::make_unique<SpacemitAudio::MultiSoundLocator>(loc_cfg);
    if (!locator_->Initialize()) {
        err << "MultiSoundLocator Initialize failed\n";
        locator_.reset();
        return false;
    }
    selected_.resize(static_cast<size_t>(loc_cfg.frame_size) *
        static_cast<size_t>(mic_count_));
    return true;
}

void DoaRuntime::Reset() {
    if (locator_) locator_->Reset();
    std::lock_guard<std::mutex> lock(result_mutex_);
    has_result_ = false;
    has_valid_result_ = false;
    latest_ = {};
    latest_valid_ = {};
}

bool DoaRuntime::ProcessInterleaved(const float* interleaved, size_t frames,
                                    int capture_channels) {
    if (!enabled_ || !locator_ || !interleaved || frames == 0) return false;
    if (capture_channels != config_.capture_channels) return false;

    const size_t needed = frames * static_cast<size_t>(mic_count_);
    if (selected_.size() < needed) selected_.resize(needed);

    for (size_t f = 0; f < frames; ++f) {
        for (int m = 0; m < mic_count_; ++m) {
            selected_[f * mic_count_ + m] =
                interleaved[f * capture_channels + pick_zero_based_[m]];
        }
    }

    bool ready = false;
    try {
        ready = locator_->Process(selected_.data(), frames, mic_count_);
    } catch (const std::exception&) {
        return false;
    }
    if (ready) {
        std::lock_guard<std::mutex> lock(result_mutex_);
        latest_ = locator_->GetResult();
        has_result_ = true;
        if (latest_.valid) {
            latest_valid_ = latest_;
            has_valid_result_ = true;
        }
    }
    return ready;
}

bool DoaRuntime::GetLatest(SpacemitAudio::MultiSoundLocatorResult* out) const {
    std::lock_guard<std::mutex> lock(result_mutex_);
    if (!has_result_) return false;
    if (out) *out = latest_;
    return true;
}

bool DoaRuntime::GetLatestValid(
    SpacemitAudio::MultiSoundLocatorResult* out) const {
    std::lock_guard<std::mutex> lock(result_mutex_);
    if (!has_valid_result_) return false;
    if (out) *out = latest_valid_;
    return true;
}

float DoaRuntime::GetAverageAzimuth() const {
    return locator_ ? locator_->GetAverageAzimuth() : 0.0f;
}

float DoaRuntime::GetAverageResultantLength() const {
    return locator_ ? locator_->GetAverageResultantLength() : 0.0f;
}

int DoaRuntime::GetResultCount() const {
    return locator_ ? locator_->GetResultCount() : 0;
}

std::string DoaRuntime::ChannelMapString() const {
    std::ostringstream oss;
    oss << "speech=ch" << config_.speech_channel;
    if (enabled_) {
        oss << ", doa=";
        for (size_t i = 0; i < config_.pick.size(); ++i) {
            if (i) oss << ",";
            oss << "ch" << config_.pick[i] << "->mic" << i;
        }
    }
    return oss.str();
}

}  // namespace omni_agent
