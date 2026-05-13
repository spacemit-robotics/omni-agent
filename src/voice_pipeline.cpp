/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voice_pipeline.hpp"

#include <chrono>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "text_buffer.hpp"
#include "voice_common.hpp"

#ifdef USE_MCP
#include <nlohmann/json.hpp>

#include "mcp_helper.hpp"
using json = nlohmann::json;

namespace {

struct PendingToolCall {
    std::string id;
    std::string name;
    json args;
};

void trimConversationHistory(std::vector<spacemit_llm::ChatMessage>* messages) {
    constexpr size_t kMaxNonSystemMessages = 24;

    size_t non_system_count = 0;
    for (const auto& msg : *messages) {
        if (msg.role != spacemit_llm::ChatMessage::Role::SYSTEM) {
            non_system_count++;
        }
    }
    if (non_system_count <= kMaxNonSystemMessages) return;

    std::vector<spacemit_llm::ChatMessage> system_messages;
    std::vector<spacemit_llm::ChatMessage> non_system_messages;
    system_messages.reserve(messages->size());
    non_system_messages.reserve(non_system_count);

    for (const auto& msg : *messages) {
        if (msg.role == spacemit_llm::ChatMessage::Role::SYSTEM) {
            system_messages.push_back(msg);
        } else {
            non_system_messages.push_back(msg);
        }
    }

    size_t start = non_system_messages.size() - kMaxNonSystemMessages;
    while (start < non_system_messages.size() &&
            non_system_messages[start].role == spacemit_llm::ChatMessage::Role::TOOL) {
        start++;
    }

    std::vector<spacemit_llm::ChatMessage> trimmed = std::move(system_messages);
    trimmed.insert(trimmed.end(), non_system_messages.begin() + start, non_system_messages.end());
    messages->swap(trimmed);
}

bool collectValidToolCalls(const std::string& tool_calls_json,
        std::vector<PendingToolCall>* pending_calls,
        json* valid_tool_calls) {
    auto tool_calls = json::parse(tool_calls_json);
    *valid_tool_calls = json::array();

    for (const auto& tc : tool_calls) {
        std::string tc_id = tc.value("id", "");
        if (tc_id.empty()) {
            tc_id = "call_" + std::to_string(pending_calls->size());
        }

        if (!tc.contains("function") || !tc["function"].is_object()) {
            std::cerr << getTimestamp() << " [MCP] 跳过非法工具调用: 缺少 function\n";
            continue;
        }

        const auto& fn = tc["function"];
        if (!fn.contains("name") || !fn["name"].is_string() ||
                !fn.contains("arguments")) {
            std::cerr << getTimestamp() << " [MCP] 跳过非法工具调用: 字段不完整\n";
            continue;
        }

        std::string tool_name = fn["name"].get<std::string>();
        json tool_args;
        const json& raw_args = fn["arguments"];
        if (raw_args.is_string()) {
            try {
                tool_args = json::parse(raw_args.get<std::string>());
            } catch (const std::exception& e) {
                std::cerr << getTimestamp() << " [MCP] 跳过非法工具调用参数: "
                    << tool_name << " (" << e.what() << ")\n";
                continue;
            }
        } else {
            tool_args = raw_args;
        }

        if (!tool_args.is_object()) {
            std::cerr << getTimestamp() << " [MCP] 跳过非法工具调用参数: "
                << tool_name << " 参数不是 JSON object\n";
            continue;
        }

        valid_tool_calls->push_back({
            {"id", tc_id},
            {"type", tc.value("type", "function")},
            {"function", {
                {"name", tool_name},
                {"arguments", tool_args.dump()}
            }}
        });
        pending_calls->push_back({tc_id, tool_name, tool_args});
    }

    return !pending_calls->empty();
}

}  // namespace
#endif

void playStartupGreeting(VoicePipelineContext& ctx, const std::string& greeting) {
    if (greeting.empty() || !g_running) return;

    g_processing = true;
    g_barge_in = false;

    std::cout << getTimestamp() << " [启动问候]: " << greeting << "\n";
    auto result = ctx.tts->Call(greeting);
    if (result && result->IsSuccess() && g_running) {
        std::cout << getTimestamp() << " [TTS] 启动问候 "
            << result->GetDurationMs() << "ms, "
            << "RTF=" << std::fixed << std::setprecision(3)
            << result->GetRTF() << "\n";
        auto audio_bytes = result->GetAudioData();
        if (!audio_bytes.empty()) {
            if (ctx.save_tts_audio) {
                ctx.save_tts_audio(audio_bytes);
            }
            auto float_samples = pcm16BytesToFloat(audio_bytes);
            ctx.enqueue_playback(float_samples, ctx.tts_sample_rate);

            auto wait_start = std::chrono::steady_clock::now();
            while (g_running && !ctx.is_playing() &&
                    std::chrono::steady_clock::now() - wait_start <
                        std::chrono::milliseconds(500)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            while (g_running && ctx.is_playing()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    } else {
        std::cerr << getTimestamp() << " [TTS] 启动问候合成失败\n";
    }

    {
        std::lock_guard<std::mutex> lock(*ctx.buffer_mutex);
        ctx.audio_buffer->clear();
        ctx.pre_buffer->clear();
        *ctx.silence_frames = 0;
        *ctx.is_speaking = false;
    }
    *ctx.barge_in_recording = false;
    ctx.vad_frame_buffer->clear();
    ctx.vad->Reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    g_processing = false;
}

void processText(VoicePipelineContext& ctx, const std::string& text) {
    if (text.empty()) return;

    g_processing = true;
    g_barge_in = false;

    std::cout << "\n" << getTimestamp() << " [你]: " << text << "\n";

    TextBuffer text_buffer;
    std::string full_response;
    int sentence_count = 0;
    int total_duration_ms = 0;
    int total_processing_ms = 0;

    auto synthesizeSentence = [&](const std::string& sentence) {
        if (sentence.empty() || g_barge_in || !g_running) return;

        sentence_count++;
        auto result = ctx.tts->Call(sentence);
        if (result && result->IsSuccess() && !g_barge_in) {
            total_duration_ms += result->GetDurationMs();
            total_processing_ms += result->GetProcessingTimeMs();
            std::cout << getTimestamp() << " [TTS] 句" << sentence_count
                << ": \"" << sentence << "\" "
                << result->GetDurationMs() << "ms, "
                << "RTF=" << std::fixed << std::setprecision(3) << result->GetRTF() << "\n";
            auto audio_bytes = result->GetAudioData();
            if (!audio_bytes.empty()) {
                if (ctx.save_tts_audio) {
                    ctx.save_tts_audio(audio_bytes);
                }
                auto float_samples = pcm16BytesToFloat(audio_bytes);
                ctx.enqueue_playback(float_samples, ctx.tts_sample_rate);
            }
        }
    };

#ifdef USE_MCP
    if (ctx.mcp_enabled) {
        {
            std::lock_guard<std::mutex> lock(*ctx.conversation_mutex);
            ctx.conversation_messages->push_back(spacemit_llm::ChatMessage::User(text));
            trimConversationHistory(ctx.conversation_messages);
        }

        const int MAX_TOOL_ROUNDS = 10;
        int round = 0;

        while (round++ < MAX_TOOL_ROUNDS && g_running && !g_barge_in) {
            std::cout << getTimestamp() << " [LLM] 第 " << round << " 轮...\n";
            std::cout << getTimestamp() << " [AI]: " << std::flush;

            std::string current_tools;
            std::vector<spacemit_llm::ChatMessage> current_messages;
            {
                std::lock_guard<std::mutex> lock(*ctx.tools_mutex);
                current_tools = *ctx.llm_tools_json;
            }
            {
                std::lock_guard<std::mutex> lock(*ctx.conversation_mutex);
                current_messages = *ctx.conversation_messages;
            }

            auto result = ctx.llm->chat_stream(
                current_messages,
                [&](const std::string& chunk, bool is_done, const std::string& error) -> bool {
                    if (g_barge_in || !g_running) return false;
                    if (!error.empty()) {
                        std::cerr << "\n" << getTimestamp() << " [LLM错误] " << error << std::endl;
                        return false;
                    }
                    if (is_done) return true;

                    if (!chunk.empty()) {
                        std::cout << chunk << std::flush;
                        full_response += chunk;
                        text_buffer.addText(chunk);

                        while (text_buffer.hasSentence() && !g_barge_in) {
                            std::string sentence = text_buffer.getNextSentence();
                            synthesizeSentence(sentence);
                        }
                    }
                    return true;
                },
                current_tools);

            std::cout << std::endl;

            if (result.cancelled && g_barge_in) {
                std::cout << getTimestamp() << " [LLM] 已因 barge-in 中断生成\n";
                break;
            }
            if (!result.error.empty()) {
                g_processing = false;
                return;
            }

            if (result.HasToolCalls()) {
                std::cout << getTimestamp() << " [Tool Call] 检测到工具调用\n";

                try {
                    std::vector<PendingToolCall> pending_calls;
                    json valid_tool_calls;
                    if (!collectValidToolCalls(result.tool_calls_json,
                            &pending_calls, &valid_tool_calls)) {
                        std::cerr << getTimestamp() << " [MCP] 本轮工具调用全部无效，已忽略\n";
                        {
                            std::lock_guard<std::mutex> lock(*ctx.conversation_mutex);
                            ctx.conversation_messages->push_back(
                                spacemit_llm::ChatMessage::Assistant(
                                    "工具调用参数格式错误，请重新提问。"));
                            trimConversationHistory(ctx.conversation_messages);
                        }
                        synthesizeSentence("工具调用参数格式错误，请重新提问。");
                        break;
                    }

                    {
                        std::lock_guard<std::mutex> lock(*ctx.conversation_mutex);
                        ctx.conversation_messages->push_back(
                            spacemit_llm::ChatMessage::Assistant(
                                result.content,
                                valid_tool_calls.dump(),
                                result.reasoning_content));
                    }

                    for (const auto& call : pending_calls) {
                        std::string server = ctx.mcp_manager->findServerForTool(call.name);
                        std::cout << getTimestamp() << " [MCP] 调用: " << call.name
                            << " @ " << server << " 参数: "
                            << call.args.dump() << std::endl;

                        auto tool_result = ctx.mcp_manager->callTool(call.name, call.args);

                        std::string result_text;
                        if (tool_result.success && !tool_result.contents.empty()) {
                            result_text = tool_result.contents[0];
                        } else if (!tool_result.error.empty()) {
                            result_text = "错误: " + tool_result.error;
                        } else {
                            result_text = tool_result.rawResult.dump();
                        }

                        std::cout << getTimestamp() << " [MCP] 结果: " << result_text << std::endl;

                        {
                            std::lock_guard<std::mutex> lock(*ctx.conversation_mutex);
                            ctx.conversation_messages->push_back(
                                spacemit_llm::ChatMessage::Tool(result_text, call.id));
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(*ctx.conversation_mutex);
                        trimConversationHistory(ctx.conversation_messages);
                    }
                } catch (const std::exception& e) {
                    std::cerr << getTimestamp() << " [MCP] 工具调用解析错误: " << e.what() << std::endl;
                }

                full_response.clear();
                text_buffer.clear();
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(*ctx.conversation_mutex);
                ctx.conversation_messages->push_back(
                    spacemit_llm::ChatMessage::Assistant(result.content));
            }

            if (!g_barge_in) {
                text_buffer.stop();
                std::string remaining = text_buffer.getNextSentence();
                if (!remaining.empty()) {
                    synthesizeSentence(remaining);
                }
            }
            break;
        }
    } else {
#else
    {
#endif  // USE_MCP
        std::cout << getTimestamp() << " [LLM] 开始生成...\n";
        std::cout << getTimestamp() << " [AI]: " << std::flush;

        std::vector<spacemit_llm::ChatMessage> msgs;
        msgs.push_back(spacemit_llm::ChatMessage::System(ctx.system_prompt));
        msgs.push_back(spacemit_llm::ChatMessage::User(text));

        auto result = ctx.llm->chat_stream(msgs,
            [&](const std::string& chunk, bool is_done, const std::string& error) -> bool {
                if (g_barge_in || !g_running) return false;
                if (!error.empty()) {
                    std::cerr << "\n" << getTimestamp() << " [LLM错误] " << error << std::endl;
                    return false;
                }
                if (is_done) return true;

                if (!chunk.empty()) {
                    std::cout << chunk << std::flush;
                    full_response += chunk;
                    text_buffer.addText(chunk);

                    while (text_buffer.hasSentence() && !g_barge_in) {
                        std::string sentence = text_buffer.getNextSentence();
                        synthesizeSentence(sentence);
                    }
                }
                return true;
            });

        if (result.cancelled && g_barge_in) {
            std::cout << "\n" << getTimestamp() << " [LLM] 已因 barge-in 中断生成\n";
        } else if (!result.error.empty()) {
            std::cerr << "\n" << getTimestamp() << " [LLM错误] " << result.error << std::endl;
            g_processing = false;
            return;
        }

        std::cout << std::endl;

        if (!g_barge_in) {
            text_buffer.stop();
            std::string remaining = text_buffer.getNextSentence();
            if (!remaining.empty()) {
                synthesizeSentence(remaining);
            }
        }
    }

    if (sentence_count > 0) {
        float avg_rtf = total_duration_ms > 0
            ? static_cast<float>(total_processing_ms) / total_duration_ms : 0.0f;
        std::cout << getTimestamp() << " [TTS] 流式合成完成 ("
            << sentence_count << " 句, "
            << "音频=" << std::fixed << std::setprecision(1) << (total_duration_ms / 1000.0f) << "s, "
            << "耗时=" << (total_processing_ms / 1000.0f) << "s, "
            << "RTF=" << std::setprecision(3) << avg_rtf << ")\n";
    }

    // Wait for playback to finish
    while (ctx.is_playing() && g_running && !g_barge_in) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Clean up buffers
    if (!g_barge_in) {
        {
            std::lock_guard<std::mutex> lock(*ctx.buffer_mutex);
            ctx.audio_buffer->clear();
            ctx.pre_buffer->clear();
            *ctx.silence_frames = 0;
            *ctx.is_speaking = false;
        }
        *ctx.barge_in_recording = false;
        ctx.vad_frame_buffer->clear();
        ctx.vad->Reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << getTimestamp() << " [TTS] 播放完成，缓冲区已清理\n";
    } else {
        std::cout << getTimestamp() << " [TTS] Barge-in 打断，保留音频缓冲区\n";
        g_barge_in = false;
    }

    g_processing = false;
    std::cout << getTimestamp() << " [等待语音输入...]\n" << std::flush;
}
