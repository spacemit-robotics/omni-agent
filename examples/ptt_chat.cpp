/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * ptt_chat：按住说话入口。
 *
 * 录音由外部提供（例如手机网页按住说话后上传的 wav），本程序不开麦克风：ASR 识别录音，再走和 voice_chat
 * 相同的 processText（LLM → 分句 TTS → MCP 工具调用），用本机扬声器播放。
 *
 * stdin 命令，一行一条：
 *   wav <path>      识别并回答这段录音（16 位 PCM wav，任意采样率和声道数）
 *   text <文字>     跳过识别，直接回答
 *   interrupt       打断：停止播放，取消正在进行的回答，丢掉还没开始处理的录音
 *   reset           清空对话记忆（在当前回答结束后生效）
 *   quit
 *
 * 事件写到 --event-fd（每行一个 JSON），没给时写 stdout 并加 "@event " 前缀：
 *   {"type":"ready"}
 *   {"type":"state","id":N,"state":"recognizing|thinking|idle"}
 *   {"type":"asr","id":N,"text":"...","ms":268}        text 为空表示没听清
 *   {"type":"reply","id":N,"text":"...","tools":["已开始做「挥手」。"],"interrupted":false}
 *   {"type":"error","id":N,"message":"..."}
 *
 * API key 从环境变量 OPENAI_API_KEY 读取。
 */

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "audio_base.hpp"
#include "audio_resampler.hpp"
#include "engine_init.hpp"
#include "fake_player.hpp"
#include "voice_common.hpp"
#include "voice_pipeline.hpp"

namespace {

using json = nlohmann::json;

struct PttConfig {
    std::string llm_url;
    std::string llm_model = "qwen2.5-0.5b";
    bool llm_model_set = false;
    std::string system_prompt = "You are a helpful assistant.";
    std::string mcp_config_path;
    std::string asr_engine = "sensevoice";
    std::string tts_type = "matcha:zh-en";
    std::string output_device;  // 空 = 系统默认；数字 = 设备号；none = 不出声；其他 = 设备名片段
    int playback_rate = 16000;
    int playback_channels = 1;
    int max_tokens = 150;
    int reasoning_budget = 0;
    std::string llm_extra_body;
    int event_fd = -1;
};

std::mutex g_event_mutex;
int g_event_fd = -1;

void emitEvent(const json& event) {
    const std::string line = event.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    std::lock_guard<std::mutex> lock(g_event_mutex);
    if (g_event_fd < 0) {
        std::cout << "@event " << line << std::flush;
        return;
    }
    size_t written = 0;
    while (written < line.size()) {
        const ssize_t n = ::write(g_event_fd, line.data() + written, line.size() - written);
        if (n <= 0) return;  // 读端没了：前端已退出，事件丢掉即可
        written += static_cast<size_t>(n);
    }
}

std::vector<float> resampleMono(const std::vector<float>& samples, int from_rate, int to_rate) {
    if (samples.empty() || from_rate == to_rate) return samples;
    Resampler::Config rconf;
    rconf.input_sample_rate = from_rate;
    rconf.output_sample_rate = to_rate;
    rconf.channels = 1;
    rconf.method = from_rate < to_rate ? ResampleMethod::LINEAR_UPSAMPLE : ResampleMethod::LINEAR_DOWNSAMPLE;
    Resampler resampler(rconf);
    if (!resampler.initialize()) return {};
    return resampler.process(samples);
}

uint32_t readU32(const std::vector<char>& data, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, data.data() + offset, 4);
    return value;
}

uint16_t readU16(const std::vector<char>& data, size_t offset) {
    uint16_t value = 0;
    std::memcpy(&value, data.data() + offset, 2);
    return value;
}

// 读 16 位 PCM wav，混成单声道并重采样到 16 kHz。失败时返回 false 并给出原因。
bool loadWav16k(const std::string& path, std::vector<float>* out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        *error = "打不开录音文件";
        return false;
    }
    const std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0) {
        *error = "不是 wav 文件";
        return false;
    }
    int channels = 0;
    int rate = 0;
    int bits = 0;
    size_t pcm_offset = 0;
    size_t pcm_size = 0;
    for (size_t offset = 12; offset + 8 <= data.size();) {
        const uint32_t size = readU32(data, offset + 4);
        const size_t body = offset + 8;
        if (std::memcmp(data.data() + offset, "fmt ", 4) == 0 && size >= 16 && body + 16 <= data.size()) {
            if (readU16(data, body) != 1) {
                *error = "只支持 PCM 编码的 wav";
                return false;
            }
            channels = readU16(data, body + 2);
            rate = static_cast<int>(readU32(data, body + 4));
            bits = readU16(data, body + 14);
        } else if (std::memcmp(data.data() + offset, "data", 4) == 0) {
            pcm_offset = body;
            pcm_size = std::min<size_t>(size, data.size() - body);
            break;
        }
        offset = body + size + (size & 1);
    }
    if (bits != 16 || channels <= 0 || rate <= 0 || pcm_offset == 0) {
        *error = "只支持 16 位 PCM wav";
        return false;
    }
    const size_t frames = pcm_size / (2 * static_cast<size_t>(channels));
    std::vector<float> mono(frames);
    const char* pcm = data.data() + pcm_offset;
    for (size_t i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (int ch = 0; ch < channels; ++ch) {
            int16_t sample = 0;
            std::memcpy(&sample, pcm + (i * channels + ch) * 2, 2);
            sum += sample / 32768.0f;
        }
        mono[i] = sum / channels;
    }
    *out = resampleMono(mono, rate, 16000);
    if (out->empty() && !mono.empty()) {
        *error = "重采样失败";
        return false;
    }
    return true;
}

// 扬声器播放：和 voice_chat 的播放线程相同，阻塞写本身就是节拍器，队列空时写静音防止 ALSA XRUN。
class SpeakerPlayer {
public:
    SpeakerPlayer(int device, int rate, int channels) : player_(device), rate_(rate), channels_(channels) {}

    ~SpeakerPlayer() {
        stop_ = true;
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        player_.Stop();
        player_.Close();
    }

    bool Start() {
        if (!player_.Start(rate_, channels_, std::max(512, rate_ / 50))) return false;
        thread_ = std::thread([this]() { Loop(); });
        return true;
    }

    void Enqueue(const std::vector<float>& samples, int rate) {
        std::lock_guard<std::mutex> enqueue_lock(enqueue_mutex_);
        const std::vector<float> resampled = resampleMono(samples, rate, rate_);
        if (resampled.empty()) return;
        std::vector<uint8_t> pcm(resampled.size() * channels_ * sizeof(int16_t));
        int16_t* out = reinterpret_cast<int16_t*>(pcm.data());
        for (size_t i = 0; i < resampled.size(); ++i) {
            const float value = std::isfinite(resampled[i]) ? std::clamp(resampled[i], -1.0f, 1.0f) : 0.0f;
            const auto sample = static_cast<int16_t>(value * 32767.0f);
            for (int ch = 0; ch < channels_; ++ch) out[i * channels_ + ch] = sample;
        }
        const size_t chunk_bytes = static_cast<size_t>(rate_ / 50) * channels_ * sizeof(int16_t);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (size_t offset = 0; offset < pcm.size(); offset += chunk_bytes) {
                const size_t len = std::min(chunk_bytes, pcm.size() - offset);
                queue_.emplace(pcm.begin() + offset, pcm.begin() + offset + len);
            }
        }
        cv_.notify_one();
    }

    bool IsPlaying() const { return playing_.load(); }

    void Clear() {
        std::lock_guard<std::mutex> enqueue_lock(enqueue_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::queue<std::vector<uint8_t>> empty;
            queue_.swap(empty);
        }
        playing_ = false;
    }

private:
    void Loop() {
        const std::vector<uint8_t> silence(static_cast<size_t>(rate_ / 50) * channels_ * sizeof(int16_t), 0);
        while (!stop_) {
            std::vector<uint8_t> chunk;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(20), [this]() { return !queue_.empty() || stop_; });
                if (stop_) break;
                if (!queue_.empty()) {
                    chunk = std::move(queue_.front());
                    queue_.pop();
                }
            }
            if (chunk.empty()) {
                if (!player_.Write(silence)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                playing_ = false;
                continue;
            }
            playing_ = true;
            if (!player_.Write(chunk)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) playing_ = false;
        }
    }

    SpacemitAudio::AudioPlayer player_;
    const int rate_;
    const int channels_;
    std::mutex enqueue_mutex_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::vector<uint8_t>> queue_;
    std::atomic<bool> playing_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// 返回设备号；-1 = 系统默认，-2 = 找不到。
int resolveOutputDevice(const std::string& spec) {
    if (spec.empty()) return -1;
    if (std::all_of(spec.begin(), spec.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return std::stoi(spec);
    }
    for (const auto& device : SpacemitAudio::AudioPlayer::ListDevices()) {
        if (device.second.find(spec) != std::string::npos) return device.first;
    }
    return -2;
}

struct Job {
    enum class Kind { WAV, TEXT };
    Kind kind;
    std::string payload;
    uint64_t id;
    uint64_t generation;
};

// 命令队列：最多留一条待处理的录音，新的顶掉旧的；interrupt 清空并使正在处理的作废。
class JobQueue {
public:
    void Push(Job::Kind kind, const std::string& payload) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = Job{kind, payload, ++next_id_, generation_.load()};
        cv_.notify_all();
    }

    void Interrupt() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.reset();
        ++generation_;
    }

    void RequestReset() {
        std::lock_guard<std::mutex> lock(mutex_);
        reset_ = true;
        cv_.notify_all();
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        cv_.notify_all();
    }

    // 阻塞到有任务、有 reset 请求或关闭。返回 false 表示关闭或收到退出信号。
    bool Wait(std::optional<Job>* job, bool* reset) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!cv_.wait_for(lock, std::chrono::milliseconds(200),
                [this]() { return closed_ || reset_ || pending_.has_value(); })) {
            if (!g_running) return false;
        }
        if (closed_) return false;
        *reset = reset_;
        reset_ = false;
        *job = std::move(pending_);
        pending_.reset();
        return true;
    }

    bool Stale(const Job& job) const { return job.generation != generation_.load(); }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Job> pending_;
    uint64_t next_id_ = 0;
    std::atomic<uint64_t> generation_{0};
    bool reset_ = false;
    bool closed_ = false;
};

// 取这一轮写进对话记忆的回答和工具结果（只有 MCP 模式有对话记忆）。
void collectReply(const std::vector<spacemit_llm::ChatMessage>& messages, const std::string& user_text,
        std::string* reply, std::vector<std::string>* tools) {
    using Role = spacemit_llm::ChatMessage::Role;
    size_t start = messages.size();
    for (size_t i = messages.size(); i-- > 0;) {
        if (messages[i].role == Role::USER && messages[i].content == user_text) {
            start = i + 1;
            break;
        }
    }
    for (size_t i = start; i < messages.size(); ++i) {
        if (messages[i].role == Role::ASSISTANT) {
            std::string content = messages[i].content;
            // 去掉 processText 给被打断的一轮加的说明，访客页面只显示模型说的话
            for (const char* note : {"（被用户打断", "（出错中断"}) {
                const size_t pos = content.find(note);
                if (pos != std::string::npos) content.resize(pos);
            }
            *reply += content;
        } else if (messages[i].role == Role::TOOL) {
            tools->push_back(messages[i].content);
        }
    }
}

void printUsage(const char* program) {
    std::cout << "用法: " << program << " --llm-url <url> [选项]\n"
        << "  --llm-url <url>            OpenAI 兼容地址 (必填)\n"
        << "  --model <name>             模型名\n"
        << "  --mcp-config <file>        MCP 配置 (和 voice_chat 相同的 mcp.json 格式)\n"
        << "  --system-prompt <text>     不用 MCP 时的 system prompt\n"
        << "  --asr <engine>             ASR 后端 (默认: sensevoice)\n"
        << "  --tts <engine>             TTS 后端 (默认: matcha:zh-en)\n"
        << "  -o, --output-device <dev>  扬声器：设备号或设备名片段，none 不出声 (默认: 系统默认)\n"
        << "  --playback-rate <hz>       (默认: 16000)\n"
        << "  --playback-channels <n>    (默认: 1)\n"
        << "  --max-tokens <n>           (默认: 150)\n"
        << "  --reasoning-budget <n>     (默认: 0，不念推理内容)\n"
        << "  --llm-extra-body <json>    合并进每个 LLM 请求体的 JSON 对象，如 '{\"thinking\":{\"type\":\"disabled\"}}'\n"
        << "  --event-fd <fd>            事件写到这个 fd (默认: stdout，带 \"@event \" 前缀)\n"
        << "输入: wav <path> | text <文字> | interrupt | reset | quit，一行一条。\n";
}

bool parseArgs(int argc, char* argv[], PttConfig* cfg) {
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
        } else if (arg == "--asr" && has_value) {
            cfg->asr_engine = argv[++i];
        } else if (arg == "--tts" && has_value) {
            cfg->tts_type = argv[++i];
        } else if ((arg == "-o" || arg == "--output-device") && has_value) {
            cfg->output_device = argv[++i];
        } else if (arg == "--playback-rate" && has_value) {
            cfg->playback_rate = std::stoi(argv[++i]);
        } else if (arg == "--playback-channels" && has_value) {
            cfg->playback_channels = std::stoi(argv[++i]);
        } else if (arg == "--max-tokens" && has_value) {
            cfg->max_tokens = std::stoi(argv[++i]);
        } else if (arg == "--reasoning-budget" && has_value) {
            cfg->reasoning_budget = std::stoi(argv[++i]);
        } else if (arg == "--llm-extra-body" && has_value) {
            cfg->llm_extra_body = argv[++i];
        } else if (arg == "--event-fd" && has_value) {
            cfg->event_fd = std::stoi(argv[++i]);
        } else {
            return false;
        }
    }
    return !cfg->llm_url.empty() && cfg->playback_rate > 0 && cfg->playback_channels > 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    PttConfig cfg;
    if (!parseArgs(argc, argv, &cfg)) {
        printUsage(argv[0]);
        return 2;
    }
    g_event_fd = cfg.event_fd;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGPIPE, SIG_IGN);

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

    auto asr = initASR(cfg.asr_engine, "", "", 0);
    if (!asr) return 1;
    auto vad = initVAD(0.5f);  // processText 结束时会重置 VAD 状态
    if (!vad) return 1;
    auto tts_result = initTTS(cfg.tts_type);
    if (!tts_result.tts) return 1;

#ifdef USE_MCP
    MCPInitResult mcp;
    initMCP(cfg.mcp_config_path, llm, system_prompt, mcp, cfg.llm_url, cfg.llm_model_set ? cfg.llm_model : "");
#endif

    std::unique_ptr<FakePlayer> fake_player;
    std::unique_ptr<SpeakerPlayer> speaker;
    if (cfg.output_device == "none") {
        fake_player = std::make_unique<FakePlayer>();
    } else {
        const int device = resolveOutputDevice(cfg.output_device);
        if (device == -2) {
            std::cerr << getTimestamp() << " 错误: 找不到输出设备 \"" << cfg.output_device << "\"\n";
            return 1;
        }
        speaker = std::make_unique<SpeakerPlayer>(device, cfg.playback_rate, cfg.playback_channels);
        if (!speaker->Start()) {
            std::cerr << getTimestamp() << " 错误: 无法启动播放设备\n";
            return 1;
        }
    }

    JobQueue jobs;
    std::vector<float> audio_buffer;
    std::mutex buffer_mutex;
    int silence_frames = 0;
    bool is_speaking = false;
    std::atomic<bool> barge_in_recording{false};
    std::vector<float> vad_frame_buffer;
    std::mutex vad_state_mutex;
    std::deque<std::vector<float>> pre_buffer;

    // 正在处理的录音被 interrupt 作废后，processText 开头会把 g_barge_in 清掉；每次碰播放时再置一次，
    // 保证作废的回答不出声、不执行动作。
    std::optional<Job> current;
    std::mutex current_mutex;
    auto current_is_stale = [&]() {
        std::lock_guard<std::mutex> lock(current_mutex);
        return current.has_value() && jobs.Stale(*current);
    };

    VoicePipelineContext ctx;
    ctx.llm = llm;
    ctx.tts = tts_result.tts;
    ctx.vad = vad;
    ctx.tts_sample_rate = tts_result.sample_rate;
    ctx.system_prompt = system_prompt;
    ctx.enqueue_playback = [&](const std::vector<float>& samples, int rate) {
        if (current_is_stale()) {
            g_barge_in = true;
            return;
        }
        if (speaker) {
            speaker->Enqueue(samples, rate);
        } else {
            fake_player->Enqueue(samples, rate);
        }
    };
    ctx.is_playing = [&]() {
        if (current_is_stale()) g_barge_in = true;
        return speaker ? speaker->IsPlaying() : fake_player->IsPlaying();
    };
    ctx.clear_playback = [&]() {
        if (speaker) {
            speaker->Clear();
        } else {
            fake_player->Clear();
        }
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

    std::atomic<bool> reader_done{false};
    std::thread reader([&]() {
        std::string line;
        while (g_running && std::getline(std::cin, line)) {
            if (line == "quit") break;
            if (line == "interrupt") {
                jobs.Interrupt();
                g_barge_in = true;  // 和真实插话一样：置打断标志并清空播放
                ctx.clear_playback();
                std::cout << getTimestamp() << " [PTT] 打断" << std::endl;
            } else if (line == "reset") {
                jobs.RequestReset();
            } else if (line.rfind("wav ", 0) == 0) {
                jobs.Push(Job::Kind::WAV, line.substr(4));
            } else if (line.rfind("text ", 0) == 0) {
                jobs.Push(Job::Kind::TEXT, line.substr(5));
            } else if (!line.empty()) {
                std::cerr << getTimestamp() << " [PTT] 未知命令: " << line << std::endl;
            }
        }
        jobs.Close();
        reader_done = true;
    });

    emitEvent({{"type", "ready"}});
    std::cout << getTimestamp() << " [就绪] 等待 stdin 命令：wav <path> | text <文字> | interrupt | reset | quit"
        << std::endl;

    while (g_running) {
        {
            std::lock_guard<std::mutex> lock(current_mutex);
            current.reset();
        }
        std::optional<Job> job;
        bool reset = false;
        if (!jobs.Wait(&job, &reset)) break;
#ifdef USE_MCP
        if (reset) {
            std::lock_guard<std::mutex> lock(mcp.conversation_mutex);
            mcp.conversation_messages.clear();
            std::cout << getTimestamp() << " [PTT] 已清空对话记忆" << std::endl;
        }
#endif
        if (!job) continue;
        {
            std::lock_guard<std::mutex> lock(current_mutex);
            current = job;
        }
        const uint64_t id = job->id;
        std::string text = job->payload;
        if (job->kind == Job::Kind::WAV) {
            emitEvent({{"type", "state"}, {"id", id}, {"state", "recognizing"}});
            std::vector<float> samples;
            std::string error;
            if (!loadWav16k(job->payload, &samples, &error)) {
                std::cerr << getTimestamp() << " [PTT] " << job->payload << ": " << error << std::endl;
                emitEvent({{"type", "error"}, {"id", id}, {"message", error}});
                emitEvent({{"type", "state"}, {"id", id}, {"state", "idle"}});
                continue;
            }
            const auto started = std::chrono::steady_clock::now();
            text.clear();
            if (samples.size() >= 16000 * 3 / 10) {  // 不到 0.3 s 不送识别
                auto result = asr->Recognize(samples, 16000);
                if (result && !result->IsEmpty()) text = result->GetText();
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            std::cout << getTimestamp() << " [ASR] " << (samples.size() / 16) << " ms 音频，" << ms << " ms: \""
                << text << "\"" << std::endl;
            if (jobs.Stale(*job)) {
                std::cout << getTimestamp() << " [PTT] 识别期间被打断，丢弃" << std::endl;
                emitEvent({{"type", "state"}, {"id", id}, {"state", "idle"}});
                continue;
            }
            emitEvent({{"type", "asr"}, {"id", id}, {"text", text}, {"ms", ms}});
        }
        if (text.empty()) {
            emitEvent({{"type", "state"}, {"id", id}, {"state", "idle"}});
            continue;
        }

        emitEvent({{"type", "state"}, {"id", id}, {"state", "thinking"}});
        processText(ctx, text);
        g_barge_in = false;

        std::string reply;
        std::vector<std::string> tools;
#ifdef USE_MCP
        {
            std::lock_guard<std::mutex> lock(mcp.conversation_mutex);
            collectReply(mcp.conversation_messages, text, &reply, &tools);
        }
#endif
        emitEvent({{"type", "reply"}, {"id", id}, {"text", reply}, {"tools", tools},
            {"interrupted", jobs.Stale(*job)}});
        emitEvent({{"type", "state"}, {"id", id}, {"state", "idle"}});
    }

    g_running = false;
    // 收到信号退出时 reader 还卡在 getline 上，不等它。
    if (reader_done) {
        reader.join();
    } else {
        reader.detach();
    }
#ifdef USE_MCP
    if (mcp.registry_poll_thread.joinable()) mcp.registry_poll_thread.join();
    if (mcp.manager) mcp.manager->stopAll();
#endif
    return 0;
}
