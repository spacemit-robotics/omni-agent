/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HID_WAKE_LISTENER_HPP
#define HID_WAKE_LISTENER_HPP

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace omni_agent {

class HidWakeListener {
public:
    using WakeCallback = std::function<void()>;

    HidWakeListener() = default;
    ~HidWakeListener();

    HidWakeListener(const HidWakeListener&) = delete;
    HidWakeListener& operator=(const HidWakeListener&) = delete;

    bool Start(const std::string& device, WakeCallback callback, std::string* error);
    void Stop();

    bool running() const { return running_.load(); }
    const std::string& device() const { return device_; }

private:
    void Run(int fd);

    std::string device_;
    WakeCallback callback_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

}  // namespace omni_agent

#endif  // HID_WAKE_LISTENER_HPP
