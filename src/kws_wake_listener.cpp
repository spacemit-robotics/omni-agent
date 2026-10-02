/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kws_wake_listener.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <utility>

#include "kws_service.h"

namespace omni_agent {

class KwsWakeListener::EngineCallback : public SpacemiT::KwsEngineCallback {
public:
    explicit EngineCallback(WakeCallback callback) : callback_(std::move(callback)) {}

    void OnWakeWord(const std::string& keyword, float score, int64_t /*timestamp_ms*/) override {
        callback_(keyword, score);
    }
    // Overflows repeat every 10 ms while the host is starved: log the first one, then a count.
    void OnError(const std::string& message) override {
        const auto now = std::chrono::steady_clock::now();
        if (logged_ && now - last_log_ < std::chrono::seconds(5)) {
            ++suppressed_;
            return;
        }
        std::cerr << "[KWS] " << message;
        if (suppressed_ > 0) {
            std::cerr << " (+" << suppressed_ << " suppressed)";
        }
        std::cerr << "\n";
        logged_ = true;
        last_log_ = now;
        suppressed_ = 0;
    }

private:
    WakeCallback callback_;
    bool logged_ = false;
    std::chrono::steady_clock::time_point last_log_;
    uint64_t suppressed_ = 0;
};

KwsWakeListener::KwsWakeListener() = default;

KwsWakeListener::~KwsWakeListener() {
    Stop();
}

bool KwsWakeListener::Start(const Options& options, WakeCallback callback, std::string* error) {
    Stop();
    if (!callback) {
        if (error) *error = "wake callback is empty";
        return false;
    }

    // Keywords come from the model directory's keywords.txt.
    SpacemiT::KwsConfig config;
    config.model_dir = options.model_dir;
    config.num_channels = 1;
    config.threshold = options.threshold;
    config.holdoff_ms = options.holdoff_ms;
    config.partial_threshold = options.partial_threshold;

    auto engine = std::make_unique<SpacemiT::KwsEngine>(config);
    if (!engine->IsInitialized()) {
        if (error) *error = engine->GetLastError();
        return false;
    }
    engine->SetCallback(std::make_shared<EngineCallback>(std::move(callback)));
    if (!engine->Start()) {
        if (error) *error = engine->GetLastError();
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    engine_ = std::move(engine);
    running_ = true;
    return true;
}

void KwsWakeListener::Feed(const float* samples, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || samples == nullptr || count == 0) {
        return;
    }
    // Rejections and queue overflows are reported through OnError; listening continues.
    engine_->SendAudioFrame(samples, count);
}

void KwsWakeListener::Stop() {
    std::unique_ptr<SpacemiT::KwsEngine> engine;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        engine = std::move(engine_);
    }
    if (engine) {
        engine->Stop();
    }
}

bool KwsWakeListener::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

}  // namespace omni_agent
