/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "daemon_config.hpp"
#include "text_buffer.hpp"
#include "tool_speech.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "ASSERTION FAILED: " << message << std::endl;
        std::exit(1);
    }
}

long elapsedMs(Clock::time_point start) {
    return static_cast<long>(std::chrono::duration_cast<milliseconds>(Clock::now() - start).count());
}

// 假播放器：speak 记下句子，播放线程延迟 start_ms 开始、再过 play_ms 播完。
class FakePlayer {
public:
    FakePlayer(int start_ms, int play_ms) : start_ms_(start_ms), play_ms_(play_ms) {}
    ~FakePlayer() {
        for (auto& thread : threads_) thread.join();
    }

    ToolSpeechHooks hooks() {
        ToolSpeechHooks hooks;
        hooks.speak = [this](const std::string& sentence) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                spoken_.push_back(sentence);
            }
            ++pending_;
            threads_.emplace_back([this]() {
                std::this_thread::sleep_for(milliseconds(start_ms_));
                playing_ = true;
                std::this_thread::sleep_for(milliseconds(play_ms_));
                if (--pending_ == 0) playing_ = false;
            });
        };
        hooks.is_playing = [this]() { return playing_.load(); };
        hooks.interrupted = [this]() { return interrupted_.load(); };
        return hooks;
    }

    std::vector<std::string> spoken() {
        std::lock_guard<std::mutex> lock(mutex_);
        return spoken_;
    }

    std::atomic<bool> playing_{false};
    std::atomic<bool> interrupted_{false};

private:
    int start_ms_;
    int play_ms_;
    std::atomic<int> pending_{0};
    std::mutex mutex_;
    std::vector<std::string> spoken_;
    std::vector<std::thread> threads_;
};

void testFlush() {
    TextBuffer buffer;
    buffer.addText("好的，我准备挥手了，请注意安全");
    require(buffer.getNextSentence() == "好的，", "first sentence");
    require(buffer.getNextSentence() == "我准备挥手了，", "second sentence");
    require(!buffer.hasSentence(), "tail without punctuation stays buffered");
    buffer.flush();
    require(buffer.getNextSentence() == "请注意安全", "flush queues the tail");
    buffer.addText("还能继续。");
    require(buffer.getNextSentence() == "还能继续。", "flush does not stop the buffer");
    buffer.flush();
    require(!buffer.hasSentence(), "flush of an empty tail queues nothing");
}

void testSpeaksTailAndWaitsForPlayback() {
    FakePlayer player(0, 200);
    TextBuffer buffer;
    buffer.addText("请注意安全");
    const auto start = Clock::now();
    require(speakBeforeTool(&buffer, true, "兜底", player.hooks()), "not interrupted");
    require(player.spoken() == std::vector<std::string>{"请注意安全"}, "tail spoken, no fallback");
    require(!player.playing_, "returns after playback ends");
    require(elapsedMs(start) >= 190, "waited for playback");
}

void testFallbackWhenModelSaidNothing() {
    FakePlayer player(0, 100);
    TextBuffer buffer;
    require(speakBeforeTool(&buffer, false, "好的，请注意安全。", player.hooks()), "not interrupted");
    require(player.spoken() == std::vector<std::string>{"好的，请注意安全。"}, "fallback spoken");
    require(!player.playing_, "fallback finished before returning");
}

void testNoFallbackAfterEarlierSentence() {
    FakePlayer player(0, 100);
    TextBuffer buffer;
    player.playing_ = true;  // 流式阶段念的句子还在播
    std::thread finish([&]() {
        std::this_thread::sleep_for(milliseconds(150));
        player.playing_ = false;
    });
    const auto start = Clock::now();
    require(speakBeforeTool(&buffer, true, "兜底", player.hooks()), "not interrupted");
    finish.join();
    require(player.spoken().empty(), "model already spoke: no fallback");
    require(elapsedMs(start) >= 140, "still waits for the earlier sentence");
}

void testLatePlaybackStart() {
    // 非 AEC 播放线程每 20 ms 取一次队列：入队后 is_playing 可能还没变。
    FakePlayer player(80, 100);
    TextBuffer buffer;
    buffer.addText("好的。");
    const auto start = Clock::now();
    require(speakBeforeTool(&buffer, false, "兜底", player.hooks()), "not interrupted");
    require(elapsedMs(start) >= 170 && !player.playing_, "waits for a late start, then the end");
}

void testInterruptedWhilePlaying() {
    FakePlayer player(0, 1000);
    TextBuffer buffer;
    std::thread barge([&]() {
        std::this_thread::sleep_for(milliseconds(100));
        player.interrupted_ = true;
    });
    const auto start = Clock::now();
    const bool ok = speakBeforeTool(&buffer, false, "好的，请注意安全。", player.hooks());
    barge.join();
    require(!ok, "barge-in reports interruption");
    require(elapsedMs(start) < 500, "returns promptly on barge-in");
}

void testNothingToSay() {
    FakePlayer player(0, 100);
    TextBuffer buffer;
    const auto start = Clock::now();
    require(speakBeforeTool(&buffer, false, "", player.hooks()), "not interrupted");
    require(player.spoken().empty() && elapsedMs(start) < 50, "no text, no fallback: no wait");
}

void writeMcpConfig(const std::string& dir, const std::string& body) {
    std::ofstream output(dir + "/mcp.json", std::ios::trunc);
    require(output.good(), "failed to create mcp.json");
    output << body;
}

void writeLlmConfig(const std::string& dir, const std::string& body) {
    std::ofstream output(dir + "/llm.json", std::ios::trunc);
    require(output.good(), "failed to create llm.json");
    output << body;
}

void testLlmExtraBody(const std::string& dir) {
    writeLlmConfig(dir, R"({"extra_body": {"thinking": {"type": "disabled"}}})");
    auto config = omni_agent::LoadConfig(dir);
    require(config.llm_status.loaded, "llm.json loads: " + config.llm_status.error);
    require(config.llm.extra_body == nlohmann::json({{"thinking", {{"type", "disabled"}}}}), "extra_body parsed");
    const auto merged = nlohmann::json::parse(omni_agent::DumpMergedConfig(config));
    require(merged["llm"]["extra_body"]["thinking"]["type"] == "disabled", "merged config shows extra_body");

    writeLlmConfig(dir, R"({"max_tokens": 150})");
    config = omni_agent::LoadConfig(dir);
    require(config.llm_status.loaded && config.llm.extra_body.empty(), "absent extra_body adds nothing");
    writeLlmConfig(dir, R"({"extra_body": "thinking=off"})");
    config = omni_agent::LoadConfig(dir);
    require(!config.llm_status.loaded, "non-object extra_body is rejected");
}

void testDaemonConfig(const std::string& dir) {
    writeMcpConfig(dir, R"({"enabled": true, "pre_tool_speech": {"perform_action": "好的，请注意安全。"}})");
    auto config = omni_agent::LoadConfig(dir);
    require(config.mcp_status.loaded, "mcp.json loads: " + config.mcp_status.error);
    require(config.mcp.pre_tool_speech == nlohmann::json({{"perform_action", "好的，请注意安全。"}}),
        "pre_tool_speech parsed");
    const auto merged = nlohmann::json::parse(omni_agent::DumpMergedConfig(config));
    require(merged["mcp"]["pre_tool_speech"]["perform_action"] == "好的，请注意安全。", "merged config shows it");

    writeMcpConfig(dir, R"({"enabled": true})");
    config = omni_agent::LoadConfig(dir);
    require(config.mcp_status.loaded && config.mcp.pre_tool_speech.empty(), "absent means no gated tools");

    writeMcpConfig(dir, R"({"pre_tool_speech": ["perform_action"]})");
    config = omni_agent::LoadConfig(dir);
    require(!config.mcp_status.loaded, "array is rejected");
    writeMcpConfig(dir, R"({"pre_tool_speech": {"perform_action": 1}})");
    config = omni_agent::LoadConfig(dir);
    require(!config.mcp_status.loaded, "non-string line is rejected");
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "expected temporary config directory");
    testFlush();
    testSpeaksTailAndWaitsForPlayback();
    testFallbackWhenModelSaidNothing();
    testNoFallbackAfterEarlierSentence();
    testLatePlaybackStart();
    testInterruptedWhilePlaying();
    testNothingToSay();
    testDaemonConfig(argv[1]);
    testLlmExtraBody(argv[1]);
    std::cout << "tool_speech_test: all passed" << std::endl;
    return 0;
}
