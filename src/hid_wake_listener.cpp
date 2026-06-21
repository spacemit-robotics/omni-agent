/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hid_wake_listener.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>
#include <thread>

namespace omni_agent {
namespace {

constexpr unsigned char kWakeReport[5] = {0x4B, 0x01, 0xF0, 0x01, 0x01};

}  // namespace

HidWakeListener::~HidWakeListener() {
    Stop();
}

bool HidWakeListener::Start(const std::string& device, WakeCallback callback, std::string* error) {
    Stop();
    if (device.empty()) {
        if (error) *error = "wake HID device is empty";
        return false;
    }
    if (!callback) {
        if (error) *error = "wake callback is empty";
        return false;
    }

    int fd = open(device.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (error) {
            *error = device + ": " + std::strerror(errno);
        }
        return false;
    }

    device_ = device;
    callback_ = std::move(callback);
    running_ = true;
    worker_ = std::thread(&HidWakeListener::Run, this, fd);
    return true;
}

void HidWakeListener::Stop() {
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
}

void HidWakeListener::Run(int fd) {
    unsigned char buf[64];
    while (running_) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (n >= static_cast<ssize_t>(sizeof(kWakeReport)) &&
                std::memcmp(buf, kWakeReport, sizeof(kWakeReport)) == 0) {
            callback_();
        }
    }
    close(fd);
    running_ = false;
}

}  // namespace omni_agent
