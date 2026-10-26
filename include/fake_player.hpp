/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_PLAYER_HPP
#define FAKE_PLAYER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "voice_common.hpp"

// 假播放器：入队即算“在播放”，按音频时长计时，播完才算结束；和 AEC 播放线程的 isPlaying 语义一致。
class FakePlayer {
public:
    FakePlayer() : thread_([this]() { Loop(); }) {}
    ~FakePlayer() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    void Enqueue(const std::vector<float>& samples, int rate) {
        if (samples.empty() || rate <= 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(static_cast<int>(samples.size() * 1000 / rate));
        playing_ = true;
        cv_.notify_all();
    }

    bool IsPlaying() const { return playing_.load(); }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        ++generation_;
        playing_ = false;
        cv_.notify_all();
    }

private:
    void Loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
            cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
            if (stop_) break;
            const int duration_ms = queue_.front();
            queue_.pop_front();
            const uint64_t generation = generation_;
            std::cout << getTimestamp() << " [播放] 开始 " << duration_ms << " ms" << std::endl;
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
            cv_.wait_until(lock, end, [&]() { return stop_ || generation_ != generation; });
            if (generation_ != generation) {
                std::cout << getTimestamp() << " [播放] 被清空" << std::endl;
                continue;
            }
            if (queue_.empty()) {
                playing_ = false;
                std::cout << getTimestamp() << " [播放] 结束" << std::endl;
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<int> queue_;
    std::atomic<bool> playing_{false};
    uint64_t generation_ = 0;
    bool stop_ = false;
    std::thread thread_;
};

#endif  // FAKE_PLAYER_HPP
