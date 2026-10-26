/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TOOL_SPEECH_HPP
#define TOOL_SPEECH_HPP

#include <functional>
#include <string>

#include "text_buffer.hpp"

struct ToolSpeechHooks {
    std::function<void(const std::string&)> speak;  // 合成一句并放进播放队列
    std::function<bool()> is_playing;
    std::function<bool()> interrupted;  // barge-in 或退出
};

// 调用会让设备动起来的工具前先把话说完：念完模型这一轮还没念的半句；这一轮一句都没说时念 fallback；
// 等播放结束再返回，让用户先听到“请注意安全”再动。返回 false 表示被打断，调用方不应再执行工具。
bool speakBeforeTool(TextBuffer* buffer, bool spoke_this_round, const std::string& fallback,
    const ToolSpeechHooks& hooks);

#endif  // TOOL_SPEECH_HPP
