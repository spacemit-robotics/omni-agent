/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOICE_PIPELINE_HPP
#define VOICE_PIPELINE_HPP

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

#include "llm_service.h"
#include "tts_service.h"
#include "vad_service.h"

#ifdef USE_MCP
#include <mcp_service.hpp>
#endif

using PlaybackCallback = std::function<void(const std::vector<float>& samples, int sample_rate)>;
using IsPlayingCallback = std::function<bool()>;
using ClearPlaybackCallback = std::function<void()>;
using SaveTtsAudioCallback = std::function<void(const std::vector<uint8_t>& pcm16_bytes)>;
using FlushPendingCaptureCallback = std::function<void()>;
using LlmErrorCallback = std::function<void(const std::string& error)>;
using LlmTextCallback = std::function<void(const std::string& chunk)>;
using ToolEventCallback = std::function<void(const std::string& name, const std::string& detail)>;

struct VoicePipelineContext {
    std::shared_ptr<spacemit_llm::LLMService> llm;
    std::shared_ptr<SpacemiT::TtsEngine> tts;
    std::shared_ptr<SpacemiT::VadEngine> vad;
    int tts_sample_rate;
    std::string system_prompt;
    PlaybackCallback enqueue_playback;
    IsPlayingCallback is_playing;
    ClearPlaybackCallback clear_playback;
    SaveTtsAudioCallback save_tts_audio;
    FlushPendingCaptureCallback flush_pending_capture;
    // LLM 出错时调用；这一轮还没开口、也没调工具时再播 llm_error_speech（空 = 不播）
    LlmErrorCallback on_llm_error;
    std::string llm_error_speech;
    // 给界面实时显示：LLM 每段输出的文字（念之前就回调）
    LlmTextCallback on_llm_text;

    // State variables (references from main)
    std::vector<float>* audio_buffer;
    std::mutex* buffer_mutex;
    int* silence_frames;
    bool* is_speaking;
    std::atomic<bool>* barge_in_recording;
    std::vector<float>* vad_frame_buffer;
    std::mutex* vad_state_mutex;
    std::deque<std::vector<float>>* pre_buffer;

#ifdef USE_MCP
    mcp::MCPManager* mcp_manager = nullptr;
    std::string* llm_tools_json = nullptr;
    std::mutex* tools_mutex = nullptr;
    std::vector<spacemit_llm::ChatMessage>* conversation_messages = nullptr;
    std::mutex* conversation_mutex = nullptr;
    bool mcp_enabled = false;
    // 工具名 → 兜底播报。列出的工具先把这一轮的话播完（模型没说话就播兜底句）再调用。
    std::map<std::string, std::string> pre_tool_speech;
    // 给界面实时显示：模型决定调用工具时（念播报之前，detail = 参数 JSON）、工具返回后（detail = 结果文字）
    ToolEventCallback on_tool_call;
    ToolEventCallback on_tool_result;
#endif
};

void playStartupGreeting(VoicePipelineContext& ctx, const std::string& greeting);
void processText(VoicePipelineContext& ctx, const std::string& text);

#endif  // VOICE_PIPELINE_HPP
