/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tool_speech.hpp"

#include <chrono>
#include <string>
#include <thread>

bool speakBeforeTool(TextBuffer* buffer, bool spoke_this_round, const std::string& fallback,
    const ToolSpeechHooks& hooks) {
    bool spoke = spoke_this_round;
    buffer->flush();
    while (buffer->hasSentence() && !hooks.interrupted()) {
        const std::string sentence = buffer->getNextSentence();
        if (sentence.empty()) continue;
        hooks.speak(sentence);
        spoke = true;
    }
    if (!spoke && !fallback.empty() && !hooks.interrupted()) {
        hooks.speak(fallback);
        spoke = true;
    }

    // 刚入队的音频可能还没被播放线程取走：先等它开始（最多 300 ms），再等它播完。
    using Clock = std::chrono::steady_clock;
    const auto start_deadline = Clock::now() + std::chrono::milliseconds(300);
    while (spoke && !hooks.is_playing() && !hooks.interrupted() && Clock::now() < start_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    while (hooks.is_playing() && !hooks.interrupted()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return !hooks.interrupted();
}
