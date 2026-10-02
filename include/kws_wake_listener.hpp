/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KWS_WAKE_LISTENER_HPP
#define KWS_WAKE_LISTENER_HPP

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace SpacemiT {
class KwsEngine;
}  // namespace SpacemiT

namespace omni_agent {

// Wake source backed by the on-device KWS model (components/model_zoo/kws).
// The caller feeds 16 kHz mono audio; the callback runs on the engine's event
// thread once per detected keyword.
class KwsWakeListener {
public:
    struct Options {
        std::string model_dir;    // empty = KWS_MODEL_DIR or ~/.cache/models/kws/xiaojin-v1
        float threshold = 0.3f;
        int holdoff_ms = 1000;
        float partial_threshold = 0.0f;  // > 0: also accept half the keyword at this score
    };
    using WakeCallback = std::function<void(const std::string& keyword, float score)>;

    KwsWakeListener();
    ~KwsWakeListener();

    KwsWakeListener(const KwsWakeListener&) = delete;
    KwsWakeListener& operator=(const KwsWakeListener&) = delete;

    bool Start(const Options& options, WakeCallback callback, std::string* error);
    // Audio is ignored until Start succeeds and after Stop.
    void Feed(const float* samples, size_t count);
    void Stop();

    bool running() const;

private:
    class EngineCallback;

    mutable std::mutex mutex_;
    std::unique_ptr<SpacemiT::KwsEngine> engine_;
    bool running_ = false;
};

}  // namespace omni_agent

#endif  // KWS_WAKE_LISTENER_HPP
