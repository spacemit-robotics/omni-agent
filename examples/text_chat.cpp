/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * text_chat：文字输入调试入口。
 *
 * 和 voice_chat 走同一个 processText（LLM → 分句 TTS → MCP 工具调用），只把麦克风 + ASR 换成 stdin，
 * 把声卡换成按音频时长计时的假播放器，合成的语音按轮存成 wav。不占用音频设备，适合调 prompt 和工具交互。
 *
 *   text_chat --llm-url https://api.deepseek.com --model deepseek-flash --mcp-config mcp.json
 *   > 跟大家打个招呼
 *   > /barge 800 给我比个心      # 开始处理 800 ms 后模拟用户打断
 *   > /quit
 *
 * API key 从环境变量 OPENAI_API_KEY 读取。
 */

#include <atomic>
#include <csignal>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>  // NOLINT(build/c++17)
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine_init.hpp"
#include "fake_player.hpp"
#include "voice_common.hpp"
#include "voice_pipeline.hpp"

namespace {

struct TextChatConfig {
    std::string llm_url;
    std::string llm_model = "qwen2.5-0.5b";
    bool llm_model_set = false;
    std::string system_prompt = "You are a helpful assistant.";
    std::string mcp_config_path;
    std::string tts_type = "matcha:zh-en";
    std::string out_dir = "text_chat_out";
    int max_tokens = 150;
    int reasoning_budget = 0;
    std::string llm_extra_body;
};

void writeWav(const std::string& path, const std::vector<uint8_t>& pcm16, int rate) {
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(static_cast<uint32_t>(36 + pcm16.size()));
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(static_cast<uint32_t>(rate));
    u32(static_cast<uint32_t>(rate * 2));
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(static_cast<uint32_t>(pcm16.size()));
    out.write(reinterpret_cast<const char*>(pcm16.data()), static_cast<std::streamsize>(pcm16.size()));
}

void printUsage(const char* program) {
    std::cout << "用法: " << program << " --llm-url <url> [选项]\n"
        << "  --llm-url <url>            OpenAI 兼容地址 (必填)\n"
        << "  --model <name>             模型名\n"
        << "  --mcp-config <file>        MCP 配置 (和 voice_chat 相同的 mcp.json 格式)\n"
        << "  --system-prompt <text>     不用 MCP 时的 system prompt\n"
        << "  --tts <engine>             TTS 后端 (默认: matcha:zh-en)\n"
        << "  --max-tokens <n>           (默认: 150，与 daemon 默认一致)\n"
        << "  --reasoning-budget <n>     (默认: 0，不念推理内容)\n"
        << "  --llm-extra-body <json>    合并进每个 LLM 请求体的 JSON 对象，如 '{\"thinking\":{\"type\":\"disabled\"}}'\n"
        << "  --out-dir <dir>            每轮合成的语音存为 <dir>/turn_NN.wav (默认: text_chat_out)\n"
        << "输入: 一行一句；/barge <ms> <文字> 在开始处理 <ms> 毫秒后模拟打断；/quit 退出。\n";
}

bool parseArgs(int argc, char* argv[], TextChatConfig* cfg) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--llm-url" && has_value) {
            cfg->llm_url = argv[++i];
        } else if (arg == "--model" && has_value) {
            cfg->llm_model = argv[++i];
            cfg->llm_model_set = true;
        } else if (arg == "--mcp-config" && has_value) {
            cfg->mcp_config_path = argv[++i];
        } else if (arg == "--system-prompt" && has_value) {
            cfg->system_prompt = argv[++i];
        } else if (arg == "--tts" && has_value) {
            cfg->tts_type = argv[++i];
        } else if (arg == "--max-tokens" && has_value) {
            cfg->max_tokens = std::stoi(argv[++i]);
        } else if (arg == "--reasoning-budget" && has_value) {
            cfg->reasoning_budget = std::stoi(argv[++i]);
        } else if (arg == "--llm-extra-body" && has_value) {
            cfg->llm_extra_body = argv[++i];
        } else if (arg == "--out-dir" && has_value) {
            cfg->out_dir = argv[++i];
        } else {
            return false;
        }
    }
    return !cfg->llm_url.empty();
}

}  // namespace

int main(int argc, char* argv[]) {
    TextChatConfig cfg;
    if (!parseArgs(argc, argv, &cfg)) {
        printUsage(argv[0]);
        return 2;
    }
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    auto llm_result = initLLM(cfg.llm_model, cfg.llm_url, cfg.system_prompt, cfg.max_tokens);
    if (!llm_result.llm) return 1;
    auto llm = llm_result.llm;
    if (cfg.reasoning_budget >= 0) llm->update_reasoning_budget(cfg.reasoning_budget);
    if (!cfg.llm_extra_body.empty()) {
        try {
            llm->update_extra_body(cfg.llm_extra_body);
        } catch (const std::exception& e) {
            std::cerr << getTimestamp() << " 错误: --llm-extra-body 不是 JSON 对象: " << e.what() << "\n";
            return 1;
        }
    }
    auto system_prompt = llm_result.system_prompt;

    auto vad = initVAD(0.5f);  // processText 结束时会重置 VAD 状态
    if (!vad) return 1;
    auto tts_result = initTTS(cfg.tts_type);
    if (!tts_result.tts) return 1;

#ifdef USE_MCP
    MCPInitResult mcp;
    initMCP(cfg.mcp_config_path, llm, system_prompt, mcp, cfg.llm_url, cfg.llm_model_set ? cfg.llm_model : "");
#endif

    std::error_code dir_error;
    std::filesystem::create_directories(cfg.out_dir, dir_error);
    if (dir_error) {
        std::cerr << "无法创建输出目录 " << cfg.out_dir << ": " << dir_error.message() << "\n";
        return 1;
    }

    FakePlayer player;
    std::vector<uint8_t> turn_audio;
    std::vector<float> audio_buffer;
    std::mutex buffer_mutex;
    int silence_frames = 0;
    bool is_speaking = false;
    std::atomic<bool> barge_in_recording{false};
    std::vector<float> vad_frame_buffer;
    std::mutex vad_state_mutex;
    std::deque<std::vector<float>> pre_buffer;

    VoicePipelineContext ctx;
    ctx.llm = llm;
    ctx.tts = tts_result.tts;
    ctx.vad = vad;
    ctx.tts_sample_rate = tts_result.sample_rate;
    ctx.system_prompt = system_prompt;
    ctx.enqueue_playback = [&](const std::vector<float>& samples, int rate) { player.Enqueue(samples, rate); };
    ctx.is_playing = [&]() { return player.IsPlaying(); };
    ctx.clear_playback = [&]() { player.Clear(); };
    ctx.save_tts_audio = [&](const std::vector<uint8_t>& pcm16) {
        turn_audio.insert(turn_audio.end(), pcm16.begin(), pcm16.end());
    };
    ctx.audio_buffer = &audio_buffer;
    ctx.buffer_mutex = &buffer_mutex;
    ctx.silence_frames = &silence_frames;
    ctx.is_speaking = &is_speaking;
    ctx.barge_in_recording = &barge_in_recording;
    ctx.vad_frame_buffer = &vad_frame_buffer;
    ctx.vad_state_mutex = &vad_state_mutex;
    ctx.pre_buffer = &pre_buffer;
#ifdef USE_MCP
    ctx.mcp_manager = mcp.manager.get();
    ctx.llm_tools_json = &mcp.llm_tools_json;
    ctx.tools_mutex = &mcp.tools_mutex;
    ctx.conversation_messages = &mcp.conversation_messages;
    ctx.conversation_mutex = &mcp.conversation_mutex;
    ctx.mcp_enabled = mcp.enabled;
    ctx.pre_tool_speech = mcp.config.pre_tool_speech;
#endif

    std::cout << getTimestamp() << " [就绪] 输入一句话回车；/barge <ms> <文字> 模拟打断；/quit 退出" << std::endl;
    int turn = 0;
    std::string line;
    while (g_running && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (line == "/quit") break;
        int barge_ms = -1;
        std::string text = line;
        if (line.rfind("/barge ", 0) == 0) {
            const size_t space = line.find(' ', 7);
            if (space == std::string::npos) {
                std::cout << "用法: /barge <ms> <文字>" << std::endl;
                continue;
            }
            barge_ms = std::stoi(line.substr(7, space - 7));
            text = line.substr(space + 1);
        }

        ++turn;
        turn_audio.clear();
        std::mutex barge_mutex;
        std::condition_variable barge_cv;
        bool turn_done = false;
        std::thread barge;
        if (barge_ms >= 0) {
            barge = std::thread([&]() {
                std::unique_lock<std::mutex> lock(barge_mutex);
                if (barge_cv.wait_for(lock, std::chrono::milliseconds(barge_ms), [&]() { return turn_done; })) return;
                std::cout << getTimestamp() << " [模拟打断]" << std::endl;
                g_barge_in = true;  // 和真实唤醒/插话一样：置打断标志并清空播放
                player.Clear();
            });
        }
        processText(ctx, text);
        if (barge.joinable()) {
            {
                std::lock_guard<std::mutex> lock(barge_mutex);
                turn_done = true;
            }
            barge_cv.notify_all();
            barge.join();
        }
        g_barge_in = false;
        if (!turn_audio.empty()) {
            char name[32];
            std::snprintf(name, sizeof(name), "/turn_%02d.wav", turn);
            writeWav(cfg.out_dir + name, turn_audio, ctx.tts_sample_rate);
            std::cout << getTimestamp() << " [保存] " << cfg.out_dir << name << std::endl;
        }
    }

    g_running = false;
#ifdef USE_MCP
    if (mcp.registry_poll_thread.joinable()) mcp.registry_poll_thread.join();
    if (mcp.manager) mcp.manager->stopAll();
#endif
    return 0;
}
