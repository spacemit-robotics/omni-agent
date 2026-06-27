/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * 语音对话系统 Demo (非 AEC 模式)
 *
 * 使用独立的 AudioCapture 和 AudioPlayer，适用于硬件自带 AEC 的场景
 * 录音默认 16kHz/1ch，直连 VAD/STT，无需重采样
 * 播放使用独立采样率（默认 48kHz）
 * 支持 barge-in（用户打断 TTS 播放）
 *
 * 用法:
 *   ./voice_chat [--tts matcha:zh|matcha:en|matcha:zh-en|kokoro|kokoro:<voice>] [--model qwen2.5:0.5b] [--input-device 0] [--output-device 0]
 */

#include <iostream>
#include <string>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <deque>
#include <functional>
#include <memory>
#include <queue>
#include <condition_variable>
#include <sstream>
#include <utility>
#include <vector>
#include <cmath>

// Audio capture/playback
#include "audio_base.hpp"

// Resampler (capture rate <-> 16kHz, TTS rate -> playback rate)
#include "audio_resampler.hpp"

// Shared modules
#include "voice_common.hpp"
#include "engine_init.hpp"
#include "hid_wake_listener.hpp"
#include "voice_pipeline.hpp"
#ifdef USE_DOA
#include "doa_runtime.hpp"
#endif
#ifdef USE_AUDIO_FRONTEND
#include "webrtc_audio_frontend.hpp"
#endif

// ============================================================================
// 参数配置
// ============================================================================

struct Config {
    std::string tts_type = "matcha:zh-en";
    bool list_voices = false;
    std::string llm_model = "qwen2.5:0.5b";
    std::string llm_url = "";
    int input_device = -1;
    int output_device = -1;
    float vad_threshold = 0.8f;
    float silence_duration = 0.5f;
    std::string asr_engine = "qwen3-asr";
    std::string asr_endpoint = "http://127.0.0.1:8063/v1/chat/completions";
    std::string asr_model = "qwen3-asr";
    int asr_timeout = 60;
    bool wake_enabled = false;
    std::string wake_device = "/dev/hidraw0";
    bool wake_interrupt_mode = true;
    std::string wake_ack_audio = "~/.cache/models/assets/audio/006_im_here.wav";
    bool wake_drop_asr = true;
    int wake_drop_audio_ms = 500;
    int wake_post_ack_tail_ms = 0;
    int max_tokens = 150;
    int reasoning_budget = -1;
    std::string system_prompt = "You are a helpful assistant.";
    std::string startup_greeting;
    bool list_devices = false;

    // Audio config (independent capture/playback)
    int capture_rate = 16000;
    int capture_channels = 1;
    int speech_channel = 1;
    int playback_rate = 48000;
    int playback_channels = 1;
    bool capture_channels_set = false;

    // ch1 -> VAD/ASR 前的单路音频增强。
    bool audio_frontend_enabled = true;
    bool audio_frontend_highpass = true;
    bool audio_frontend_ns = false;
    bool audio_frontend_agc = true;
    int audio_frontend_agc_target = 3;
    int audio_frontend_agc_gain = 12;
    bool audio_frontend_agc_limiter = true;

    // 调试：音频录制
    bool save_audio = false;
    std::string audio_file = "voice_debug.wav";
    bool save_asr_audio = false;
    std::string asr_audio_file = "voice_asr_debug.wav";
    bool save_tts_audio = false;
    std::string tts_audio_file = "tts_debug.wav";

    // MCP 配置
    std::string mcp_config_path = "";

#ifdef USE_DOA
    omni_agent::DoaRuntimeConfig doa;
#endif

#ifdef USE_VP
    // Voiceprint 配置
    bool vp_enabled = false;
    std::string vp_database = "";
    int vp_threads = 1;
    float vp_threshold = 0.6f;
    int vp_top = 3;
    std::string vp_verify = "";
    bool vp_list = false;
    bool vp_verbose = false;
#endif

    // 跟踪 --model 是否被显式指定
    bool llm_model_set = false;
};

Config parseArgs(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tts") == 0 && i + 1 < argc) {
            cfg.tts_type = argv[++i];
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            cfg.llm_model = argv[++i];
            cfg.llm_model_set = true;
        } else if ((strcmp(argv[i], "--llm-url") == 0 || strcmp(argv[i], "--llm_url") == 0) && i + 1 < argc) {
            cfg.llm_url = argv[++i];
        } else if (strcmp(argv[i], "--asr-engine") == 0 && i + 1 < argc) {
            cfg.asr_engine = argv[++i];
        } else if (strcmp(argv[i], "--asr-endpoint") == 0 && i + 1 < argc) {
            cfg.asr_endpoint = argv[++i];
        } else if (strcmp(argv[i], "--asr-model") == 0 && i + 1 < argc) {
            cfg.asr_model = argv[++i];
        } else if (strcmp(argv[i], "--asr-timeout") == 0 && i + 1 < argc) {
            cfg.asr_timeout = std::stoi(argv[++i]);
        } else if ((strcmp(argv[i], "--input-device") == 0 || strcmp(argv[i], "-i") == 0) && i + 1 < argc) {
            cfg.input_device = std::stoi(argv[++i]);
        } else if ((strcmp(argv[i], "--output-device") == 0 || strcmp(argv[i], "-o") == 0) && i + 1 < argc) {
            cfg.output_device = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--list-devices") == 0 || strcmp(argv[i], "-l") == 0) {
            cfg.list_devices = true;
        } else if (strcmp(argv[i], "--capture-rate") == 0 && i + 1 < argc) {
            cfg.capture_rate = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--capture-channels") == 0 && i + 1 < argc) {
            cfg.capture_channels = std::stoi(argv[++i]);
            cfg.capture_channels_set = true;
        } else if (strcmp(argv[i], "--speech-channel") == 0 && i + 1 < argc) {
            cfg.speech_channel = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--playback-rate") == 0 && i + 1 < argc) {
            cfg.playback_rate = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--playback-channels") == 0 && i + 1 < argc) {
            cfg.playback_channels = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--audio-frontend") == 0) {
            cfg.audio_frontend_enabled = true;
        } else if (strcmp(argv[i], "--no-audio-frontend") == 0) {
            cfg.audio_frontend_enabled = false;
        } else if (strcmp(argv[i], "--audio-frontend-hpf") == 0) {
            cfg.audio_frontend_highpass = true;
        } else if (strcmp(argv[i], "--no-audio-frontend-hpf") == 0) {
            cfg.audio_frontend_highpass = false;
        } else if (strcmp(argv[i], "--audio-frontend-ns") == 0) {
            cfg.audio_frontend_ns = true;
        } else if (strcmp(argv[i], "--no-audio-frontend-ns") == 0) {
            cfg.audio_frontend_ns = false;
        } else if (strcmp(argv[i], "--audio-frontend-agc") == 0) {
            cfg.audio_frontend_agc = true;
        } else if (strcmp(argv[i], "--no-audio-frontend-agc") == 0) {
            cfg.audio_frontend_agc = false;
        } else if (strcmp(argv[i], "--audio-frontend-agc-target") == 0 && i + 1 < argc) {
            cfg.audio_frontend_agc_target = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--audio-frontend-agc-gain") == 0 && i + 1 < argc) {
            cfg.audio_frontend_agc_gain = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--audio-frontend-agc-limiter") == 0) {
            cfg.audio_frontend_agc_limiter = true;
        } else if (strcmp(argv[i], "--no-audio-frontend-agc-limiter") == 0) {
            cfg.audio_frontend_agc_limiter = false;
#ifdef USE_DOA
        } else if (strcmp(argv[i], "--doa") == 0) {
            cfg.doa.enabled = true;
        } else if (strcmp(argv[i], "--no-doa") == 0) {
            cfg.doa.enabled = false;
        } else if (strcmp(argv[i], "--doa-pick") == 0 && i + 1 < argc) {
            std::string error;
            if (!omni_agent::ParseIntList(argv[++i], &cfg.doa.pick, &error)) {
                std::cerr << "错误: --doa-pick: " << error << "\n";
                exit(1);
            }
        } else if (strcmp(argv[i], "--doa-side") == 0 && i + 1 < argc) {
            cfg.doa.side_m = std::stof(argv[++i]);
        } else if (strcmp(argv[i], "--doa-positions") == 0 && i + 1 < argc) {
            cfg.doa.positions_spec = argv[++i];
        } else if (strcmp(argv[i], "--doa-azimuth-offset") == 0 && i + 1 < argc) {
            cfg.doa.azimuth_offset_deg = std::stof(argv[++i]);
        } else if (strcmp(argv[i], "--doa-max-avg-seconds") == 0 && i + 1 < argc) {
            cfg.doa.max_avg_seconds = std::stof(argv[++i]);
        } else if (strcmp(argv[i], "--doa-confidence-threshold") == 0 && i + 1 < argc) {
            cfg.doa.confidence_threshold = std::stof(argv[++i]);
        } else if (strcmp(argv[i], "--doa-margin-threshold") == 0 && i + 1 < argc) {
            cfg.doa.margin_threshold = std::stof(argv[++i]);
        } else if (strcmp(argv[i], "--doa-quality-threshold") == 0 && i + 1 < argc) {
            cfg.doa.quality_threshold = std::stof(argv[++i]);
        } else if (strcmp(argv[i], "--doa-min-signal-rms") == 0 && i + 1 < argc) {
            cfg.doa.min_signal_rms = std::stof(argv[++i]);
        } else if (strcmp(argv[i], "--doa-closure-threshold-samples") == 0 && i + 1 < argc) {
            cfg.doa.closure_threshold_samples = std::stof(argv[++i]);
        } else if (strcmp(argv[i], "--doa-closure-threshold-fraction") == 0 && i + 1 < argc) {
            cfg.doa.closure_threshold_fraction = std::stof(argv[++i]);
#else
        } else if (strcmp(argv[i], "--doa") == 0 || strcmp(argv[i], "--no-doa") == 0 ||
                strncmp(argv[i], "--doa-", 6) == 0) {
            std::cerr << "错误: 当前构建未启用 DOA (USE_DOA=OFF)\n";
            exit(1);
#endif
        } else if (strcmp(argv[i], "--save-audio") == 0) {
            cfg.save_audio = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                cfg.audio_file = argv[++i];
            }
        } else if (strcmp(argv[i], "--save-asr-audio") == 0) {
            cfg.save_asr_audio = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                cfg.asr_audio_file = argv[++i];
            }
        } else if (strcmp(argv[i], "--save-tts-audio") == 0) {
            cfg.save_tts_audio = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                cfg.tts_audio_file = argv[++i];
            }
        } else if (strcmp(argv[i], "--mcp-config") == 0 && i + 1 < argc) {
            cfg.mcp_config_path = argv[++i];
        } else if (strcmp(argv[i], "--vad-threshold") == 0 && i + 1 < argc) {
            cfg.vad_threshold = std::stof(argv[++i]);
        } else if (strcmp(argv[i], "--silence-duration") == 0 && i + 1 < argc) {
            cfg.silence_duration = std::stof(argv[++i]);
        } else if (strcmp(argv[i], "--wake-enabled") == 0 || strcmp(argv[i], "--wake") == 0) {
            cfg.wake_enabled = true;
        } else if (strcmp(argv[i], "--no-wake") == 0) {
            cfg.wake_enabled = false;
        } else if (strcmp(argv[i], "--wake-device") == 0 && i + 1 < argc) {
            cfg.wake_device = argv[++i];
        } else if (strcmp(argv[i], "--wake-interrupt-mode") == 0) {
            cfg.wake_interrupt_mode = true;
        } else if (strcmp(argv[i], "--no-wake-interrupt-mode") == 0) {
            cfg.wake_interrupt_mode = false;
        } else if (strcmp(argv[i], "--wake-ack-audio") == 0 && i + 1 < argc) {
            cfg.wake_ack_audio = argv[++i];
        } else if (strcmp(argv[i], "--wake-drop-asr") == 0) {
            cfg.wake_drop_asr = true;
        } else if (strcmp(argv[i], "--no-wake-drop-asr") == 0) {
            cfg.wake_drop_asr = false;
        } else if (strcmp(argv[i], "--wake-drop-audio-ms") == 0 && i + 1 < argc) {
            cfg.wake_drop_audio_ms = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--wake-post-ack-tail-ms") == 0 && i + 1 < argc) {
            cfg.wake_post_ack_tail_ms = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-tokens") == 0 && i + 1 < argc) {
            cfg.max_tokens = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--reasoning-budget") == 0 && i + 1 < argc) {
            cfg.reasoning_budget = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--system-prompt") == 0 && i + 1 < argc) {
            cfg.system_prompt = argv[++i];
        } else if (strcmp(argv[i], "--startup-greeting") == 0 && i + 1 < argc) {
            cfg.startup_greeting = argv[++i];
#ifdef USE_VP
        } else if (strcmp(argv[i], "--voiceprint") == 0 || strcmp(argv[i], "-vp") == 0) {
            cfg.vp_enabled = true;
        } else if (strcmp(argv[i], "--vp-database") == 0 && i + 1 < argc) {
            cfg.vp_database = argv[++i];
        } else if (strcmp(argv[i], "--vp-threads") == 0 && i + 1 < argc) {
            cfg.vp_threads = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--vp-threshold") == 0 && i + 1 < argc) {
            cfg.vp_threshold = std::stof(argv[++i]);
        } else if (strcmp(argv[i], "--vp-top") == 0 && i + 1 < argc) {
            cfg.vp_top = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--vp-verify") == 0 && i + 1 < argc) {
            cfg.vp_verify = argv[++i];
        } else if (strcmp(argv[i], "--vp-list") == 0) {
            cfg.vp_list = true;
        } else if (strcmp(argv[i], "--vp-verbose") == 0) {
            cfg.vp_verbose = true;
#endif
        } else if (strcmp(argv[i], "--list-voices") == 0) {
            cfg.list_voices = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            std::cout << "用法: " << argv[0] << " [选项]\n"
                << "\n音频设备:\n"
                << "  -i, --input-device <id>       输入设备索引 (默认: 系统默认)\n"
                << "  -o, --output-device <id>      输出设备索引 (默认: 系统默认)\n"
                << "  -l, --list-devices            列出可用音频设备\n"
                << "\n音频参数:\n"
                << "  --capture-rate <hz>           录音采样率 (默认: 16000)\n"
                << "  --capture-channels <n>        录音声道数 (默认: 1)\n"
                << "  --speech-channel <n>          送入VAD/ASR的录音声道 (默认: 1)\n"
                << "  --playback-rate <hz>          播放采样率 (默认: 48000)\n"
                << "  --playback-channels <n>       播放声道数 (默认: 1)\n"
                << "  --audio-frontend              开启 ch1 WebRTC AGC/HPF/NS 前端\n"
                << "  --no-audio-frontend           关闭 ch1 WebRTC 前端\n"
                << "  --audio-frontend-agc          开启 AGC\n"
                << "  --no-audio-frontend-agc       关闭 AGC\n"
                << "  --audio-frontend-agc-target <dbfs>  AGC 目标峰值余量 (默认: 3)\n"
                << "  --audio-frontend-agc-gain <db>      AGC 最大数字增益 (默认: 12)\n"
                << "  --audio-frontend-hpf          开启高通滤波\n"
                << "  --no-audio-frontend-hpf       关闭高通滤波\n"
                << "  --audio-frontend-ns           开启低档降噪\n"
                << "  --no-audio-frontend-ns        关闭降噪\n"
#ifdef USE_DOA
                << "\nDOA:\n"
                << "  --doa                         开启三麦0-360度定位\n"
                << "  --no-doa                      关闭定位\n"
                << "  --doa-pick <a,b,c>            1-based定位声道映射 (4ch默认: 2,3,4)\n"
                << "  --doa-side <m>                等边三角形边长 (默认: 0.063)\n"
                << "  --doa-positions <spec>        麦克风坐标: x,y[,z];x,y[,z];x,y[,z]\n"
                << "  --doa-azimuth-offset <deg>    阵列到机器人坐标角度偏移\n"
                << "  --doa-min-signal-rms <rms>   低能量帧过滤阈值 (默认: 0.003)\n"
#endif
                << "\nLLM:\n"
                << "  --model <name>                LLM模型 (默认: qwen2.5:0.5b)\n"
                << "  --llm-url <url>               LLM API地址 (必填)\n"
                << "  --max-tokens <n>              最大生成 token 数 (默认: 150)\n"
                << "  --reasoning-budget <n>        reasoning budget；0 表示隐藏思考输出\n"
                << "  --system-prompt <text>        系统提示词\n"
                << "\nVAD:\n"
                << "  --vad-threshold <0-1>         VAD触发阈值 (默认: 0.8)\n"
                << "  --silence-duration <sec>      静音结束判定时长 (默认: 0.5)\n"
                << "\nASR:\n"
                << "  --asr-engine <name>           ASR后端: sensevoice | qwen3-asr | zipformer (默认: qwen3-asr)\n"
                << "  --asr-endpoint <url>          qwen3-asr llama-server endpoint\n"
                << "  --asr-model <name>            qwen3-asr model tag (默认: qwen3-asr)\n"
                << "  --asr-timeout <sec>           qwen3-asr HTTP超时 (默认: 60)\n"
                << "\n唤醒:\n"
                << "  --wake-enabled, --wake        开启 HID 唤醒打断\n"
                << "  --no-wake                     关闭 HID 唤醒打断\n"
                << "  --wake-device <path>          hidraw 设备 (默认: /dev/hidraw0)\n"
                << "  --wake-interrupt-mode         唤醒只中断TTS并播放提示音\n"
                << "  --no-wake-interrupt-mode      使用旧的唤醒后ASR插话模式\n"
                << "  --wake-ack-audio <wav>        唤醒提示音\n"
                << "  --wake-drop-asr               丢弃唤醒词对应的ASR输入\n"
                << "  --no-wake-drop-asr            不启用唤醒后录音丢弃窗口\n"
                << "  --wake-drop-audio-ms <ms>     唤醒后最大丢弃保护窗口 (默认: 500)\n"
                << "  --wake-post-ack-tail-ms <ms>  保守模式: 提示音后继续丢弃录音时长 (默认: 0)\n"
                << "\nTTS:\n"
                << "  --tts <engine>                TTS后端 (默认: matcha:zh-en)\n"
                << "                                matcha:zh / matcha:en / matcha:zh-en\n"
                << "                                kokoro / kokoro:<voice>\n"
                << "  --list-voices                 列出 Kokoro 可用音色\n"
                << "  --startup-greeting <text>     启动完成后播放的问候语\n"
                << "\n调试:\n"
                << "  --save-audio [file]           保存录音 (默认: voice_debug.wav)\n"
                << "  --save-asr-audio [file]       保存ASR增益后音频 (默认: voice_asr_debug.wav)\n"
                << "  --save-tts-audio [file]       保存TTS输出 (默认: tts_debug.wav)\n"
                << "\nMCP:\n"
                << "  --mcp-config <path>           MCP配置文件 (启用工具调用)\n"
                << "\n声纹识别 (Voiceprint):\n"
                << "  -vp, --voiceprint             开启声纹识别\n"
                << "  --vp-database <file>          声纹数据库文件 (开启VP时必填)\n"
                << "  --vp-threads <n>              推理线程数 (默认: 1)\n"
                << "  --vp-threshold <0-1>          相似度阈值 (默认: 0.6)\n"
                << "  --vp-top <n>                  显示前N个匹配 (默认: 3)\n"
                << "  --vp-verify <name>            验证特定说话人\n"
                << "  --vp-list                     列出所有已注册说话人\n"
                << "  --vp-verbose                  显示所有匹配分数\n"
                << "\n其他:\n"
                << "  -h, --help                    显示帮助\n";
            exit(0);
        }
    }
#ifdef USE_DOA
    if (cfg.doa.enabled && !cfg.capture_channels_set) {
        cfg.capture_channels = 4;
    }
    cfg.doa.sample_rate = cfg.capture_rate;
    cfg.doa.capture_channels = cfg.capture_channels;
    cfg.doa.speech_channel = cfg.speech_channel;
#endif
    return cfg;
}

#ifdef USE_VP
std::string formatVpScore(float score) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << score;
    return oss.str();
}
#endif

std::string formatFloat(float value, int precision) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    return oss.str();
}

class VadProgressPrinter {
public:
    VadProgressPrinter() = default;
    ~VadProgressPrinter() {
        Stop();
    }

    VadProgressPrinter(const VadProgressPrinter&) = delete;
    VadProgressPrinter& operator=(const VadProgressPrinter&) = delete;

    void Start() {
        running_ = true;
        worker_ = std::thread([this]() {
            while (running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                float prob = 0.0f;
                float threshold = 0.0f;
                size_t buffer_samples = 0;
                bool active = false;
                bool has_recent_sample = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto now = std::chrono::steady_clock::now();
                    has_recent_sample = has_sample_ &&
                        now - updated_at_ <= std::chrono::milliseconds(300);
                    if (has_recent_sample) {
                        prob = prob_;
                        threshold = threshold_;
                        buffer_samples = buffer_samples_;
                        active = active_;
                    }
                }

                if (!has_recent_sample) {
                    continue;
                }

                std::cout << "\r" << getTimestamp()
                    << " [VAD] prob=" << formatFloat(prob, 2);
                if (active) {
                    std::cout << " threshold=" << formatFloat(threshold, 2)
                        << " buffer=" << formatFloat(buffer_samples / 16000.0f, 1) << "s";
                }
                std::cout << "                    " << std::flush;
            }
        });
    }

    void Stop() {
        running_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void Publish(float prob, float threshold, size_t buffer_samples, bool active,
            bool force = false) {
        auto now = std::chrono::steady_clock::now();
        if (!force && now - last_publish_ < std::chrono::milliseconds(30)) {
            return;
        }
        last_publish_ = now;

        std::lock_guard<std::mutex> lock(mutex_);
        prob_ = prob;
        threshold_ = threshold;
        buffer_samples_ = buffer_samples;
        active_ = active;
        has_sample_ = true;
        updated_at_ = now;
    }

private:
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mutex_;
    bool has_sample_ = false;
    float prob_ = 0.0f;
    float threshold_ = 0.0f;
    size_t buffer_samples_ = 0;
    bool active_ = false;
    std::chrono::steady_clock::time_point last_publish_{};
    std::chrono::steady_clock::time_point updated_at_{};
};

// ============================================================================
// 列出音频设备
// ============================================================================

void listAudioDevices() {
    std::cout << getTimestamp() << " ========================================\n";
    std::cout << getTimestamp() << "            可用音频设备\n";
    std::cout << getTimestamp() << " ========================================\n\n";

    std::cout << getTimestamp() << " 输入设备 (麦克风):\n";
    auto input_devices = SpacemitAudio::AudioCapture::ListDevices();
    if (input_devices.empty()) {
        std::cout << getTimestamp() << "   (无可用设备)\n";
    } else {
        for (const auto& dev : input_devices) {
            std::cout << getTimestamp() << "   [" << dev.first << "] " << dev.second << "\n";
        }
    }

    std::cout << getTimestamp() << " \n输出设备 (扬声器):\n";
    auto output_devices = SpacemitAudio::AudioPlayer::ListDevices();
    if (output_devices.empty()) {
        std::cout << getTimestamp() << "   (无可用设备)\n";
    } else {
        for (const auto& dev : output_devices) {
            std::cout << getTimestamp() << "   [" << dev.first << "] " << dev.second << "\n";
        }
    }

    std::cout << getTimestamp() << " \n使用方法:\n";
    std::cout << getTimestamp() << "   voice_chat -i <输入设备ID> -o <输出设备ID>\n";
    std::cout << getTimestamp() << " ========================================\n";
}

// ============================================================================
// 主程序
// ============================================================================

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    Config cfg = parseArgs(argc, argv);
    cfg.wake_ack_audio = expandUserPath(cfg.wake_ack_audio);

    if (cfg.list_devices) {
        listAudioDevices();
        return 0;
    }

    if (cfg.list_voices) {
        printVoiceList();
        return 0;
    }

    if (cfg.llm_url.empty()) {
        std::cerr << "错误: 必须通过 --llm-url 指定 LLM API 地址\n";
        return 1;
    }

#ifdef USE_VP
    if (cfg.vp_enabled && cfg.vp_database.empty()) {
        std::cerr << "错误: 开启声纹识别 (--voiceprint) 时必须通过 --vp-database 指定数据库文件\n";
        return 1;
    }
#endif

    std::cout << getTimestamp() << " ========================================\n";
    std::cout << getTimestamp() << "    语音对话系统 (非 AEC 模式)\n";
    std::cout << getTimestamp() << " ========================================\n";
    std::cout << getTimestamp() << " TTS后端: " << cfg.tts_type << "\n";
    std::cout << getTimestamp() << " LLM模型: " << cfg.llm_model << "\n";
    std::cout << getTimestamp() << " LLM URL: " << cfg.llm_url << "\n";
    std::cout << getTimestamp() << " ASR后端: " << cfg.asr_engine;
    if (cfg.asr_engine == "qwen3-asr") {
        std::cout << " (" << cfg.asr_endpoint << ", model=" << cfg.asr_model << ")";
    }
    std::cout << "\n";
    std::cout << getTimestamp() << " HID唤醒: "
        << (cfg.wake_enabled ? ("ON (" + cfg.wake_device + ")") : "OFF") << "\n";
    if (cfg.wake_enabled) {
        std::cout << getTimestamp() << " 唤醒打断模式: "
            << (cfg.wake_interrupt_mode ? "ON" : "OFF")
            << " ack=" << cfg.wake_ack_audio
            << " drop_ms=" << cfg.wake_drop_audio_ms
            << " tail_ms=" << cfg.wake_post_ack_tail_ms << "\n";
    }
    std::cout << getTimestamp() << " 录音: " << cfg.capture_rate << " Hz / "
        << cfg.capture_channels << " ch\n";
    std::cout << getTimestamp() << " 音频前端: "
        << (cfg.audio_frontend_enabled ? "ON" : "OFF")
        << " hpf=" << (cfg.audio_frontend_highpass ? "on" : "off")
        << " ns=" << (cfg.audio_frontend_ns ? "on" : "off")
        << " agc=" << (cfg.audio_frontend_agc ? "on" : "off")
        << " target=" << cfg.audio_frontend_agc_target
        << "dBFS gain=" << cfg.audio_frontend_agc_gain << "dB\n";
    std::cout << getTimestamp() << " 播放: " << cfg.playback_rate << " Hz / "
        << cfg.playback_channels << " ch\n";
    std::cout << getTimestamp() << " 按 Ctrl+C 退出\n";
    std::cout << getTimestamp() << " ========================================\n\n";
    // -------------------------------------------------------------------------
    auto llm_result = initLLM(cfg.llm_model, cfg.llm_url, cfg.system_prompt, cfg.max_tokens);
    if (!llm_result.llm) return 1;
    auto llm = llm_result.llm;
    if (cfg.reasoning_budget >= 0) {
        llm->update_reasoning_budget(cfg.reasoning_budget);
    }
    auto system_prompt = llm_result.system_prompt;

    auto vad = initVAD(cfg.vad_threshold);
    if (!vad) return 1;

    auto asr = initASR(
        cfg.asr_engine, cfg.asr_endpoint, cfg.asr_model, cfg.asr_timeout);
    if (!asr) return 1;

    auto tts_result = initTTS(cfg.tts_type);
    if (!tts_result.tts) return 1;
    auto tts = tts_result.tts;
    int tts_sample_rate = tts_result.sample_rate;

    AudioClip wake_ack_clip;
    if (cfg.wake_enabled && cfg.wake_interrupt_mode && !cfg.wake_ack_audio.empty()) {
        std::string error;
        if (!loadWavMonoFloat(cfg.wake_ack_audio, &wake_ack_clip, &error)) {
            std::cerr << getTimestamp() << " 错误: 无法加载唤醒提示音 "
                << cfg.wake_ack_audio << ": " << error << "\n";
            return 1;
        }
        std::cout << getTimestamp() << " 唤醒提示音: " << cfg.wake_ack_audio
            << " (" << wake_ack_clip.sample_rate << " Hz, "
            << formatFloat(wake_ack_clip.samples.size() /
                static_cast<float>(wake_ack_clip.sample_rate), 2) << "s)\n";
    }

    // -------------------------------------------------------------------------
    // 5. 初始化音频设备
    // -------------------------------------------------------------------------
    std::cout << getTimestamp() << " [5/5] 初始化音频设备..." << std::flush;

    SpacemitAudio::AudioCapture capture(cfg.input_device);
    SpacemitAudio::AudioPlayer player(cfg.output_device);

    // 初始化录音重采样器（capture_rate != 16kHz 时使用）
    std::unique_ptr<Resampler> capture_resampler;
    if (cfg.capture_rate != 16000) {
        Resampler::Config rconf;
        rconf.input_sample_rate = cfg.capture_rate;
        rconf.output_sample_rate = 16000;
        rconf.channels = 1;
        rconf.method = (cfg.capture_rate > 16000)
            ? ResampleMethod::LINEAR_DOWNSAMPLE
            : ResampleMethod::LINEAR_UPSAMPLE;
        capture_resampler = std::make_unique<Resampler>(rconf);
        if (!capture_resampler->initialize()) {
            std::cerr << "\n" << getTimestamp() << " 错误: 录音重采样器初始化失败\n";
            return 1;
        }
    }

#ifdef USE_AUDIO_FRONTEND
    std::unique_ptr<omni_agent::WebRtcAudioFrontend> audio_frontend;
    if (cfg.audio_frontend_enabled) {
        omni_agent::WebRtcAudioFrontendConfig af_cfg;
        af_cfg.sample_rate = 16000;
        af_cfg.highpass_enabled = cfg.audio_frontend_highpass;
        af_cfg.noise_suppression_enabled = cfg.audio_frontend_ns;
        af_cfg.agc_enabled = cfg.audio_frontend_agc;
        af_cfg.agc_target_level_dbfs = cfg.audio_frontend_agc_target;
        af_cfg.agc_compression_gain_db = cfg.audio_frontend_agc_gain;
        af_cfg.agc_limiter_enabled = cfg.audio_frontend_agc_limiter;
        audio_frontend = std::make_unique<omni_agent::WebRtcAudioFrontend>();
        std::string error;
        if (!audio_frontend->Initialize(af_cfg, &error)) {
            std::cerr << "\n" << getTimestamp()
                << " 错误: WebRTC 音频前端初始化失败: " << error << "\n";
            return 1;
        }
    }
#else
    if (cfg.audio_frontend_enabled) {
        std::cerr << "\n" << getTimestamp()
            << " [warn] 当前构建未启用 WebRTC 音频前端，已跳过\n";
        cfg.audio_frontend_enabled = false;
    }
#endif

    // 初始化播放重采样器（TTS 采样率 != 播放采样率时使用）
    std::unique_ptr<Resampler> playback_resampler;
    if (tts_sample_rate != cfg.playback_rate) {
        Resampler::Config rconf;
        rconf.input_sample_rate = tts_sample_rate;
        rconf.output_sample_rate = cfg.playback_rate;
        rconf.channels = 1;
        rconf.method = (tts_sample_rate < cfg.playback_rate)
            ? ResampleMethod::LINEAR_UPSAMPLE
            : ResampleMethod::LINEAR_DOWNSAMPLE;
        playback_resampler = std::make_unique<Resampler>(rconf);
        if (!playback_resampler->initialize()) {
            std::cerr << "\n" << getTimestamp() << " 错误: 播放重采样器初始化失败\n";
            return 1;
        }
    }

#ifdef USE_DOA
    omni_agent::DoaRuntime doa_runtime;
    if (cfg.doa.enabled) {
        if (!doa_runtime.Initialize(cfg.doa, std::cerr)) {
            std::cerr << "\n" << getTimestamp() << " 错误: DOA 初始化失败\n";
            return 1;
        }
    }
#endif

    std::cout << " OK\n";
    if (cfg.capture_rate == 16000 && cfg.capture_channels == 1) {
        std::cout << getTimestamp() << " 录音管道: 16kHz/1ch -> ";
        if (cfg.audio_frontend_enabled) std::cout << "WebRTC前端 -> ";
        std::cout << "VAD/STT (零重采样)\n";
    } else {
        std::cout << getTimestamp() << " 录音管道: " << cfg.capture_rate << "Hz/"
            << cfg.capture_channels << "ch -> ";
        if (cfg.capture_channels > 1) {
            std::cout << "取ch" << cfg.speech_channel << "->mono -> ";
        }
        if (cfg.capture_rate != 16000) std::cout << "重采样->16kHz -> ";
        if (cfg.audio_frontend_enabled) std::cout << "WebRTC前端 -> ";
        std::cout << "VAD/STT\n";
    }
#ifdef USE_DOA
    if (doa_runtime.enabled()) {
        std::cout << getTimestamp() << " DOA: ON (" << doa_runtime.ChannelMapString()
            << ", sample_rate=" << cfg.capture_rate << ")\n";
    }
#endif
    if (tts_sample_rate == cfg.playback_rate) {
        std::cout << getTimestamp() << " 播放管道: TTS(" << tts_sample_rate << "Hz) -> 直连播放\n";
    } else {
        std::cout << getTimestamp() << " 播放管道: TTS(" << tts_sample_rate << "Hz) -> 重采样->"
            << cfg.playback_rate << "Hz -> 播放\n";
    }
    std::cout << "\n";

    // -------------------------------------------------------------------------
    // 6. 初始化 MCP (可选)
    // -------------------------------------------------------------------------
#ifdef USE_MCP
    MCPInitResult mcp;
    initMCP(cfg.mcp_config_path, llm, system_prompt, mcp,
            cfg.llm_url,
            cfg.llm_model_set ? cfg.llm_model : "");
#endif

    // -------------------------------------------------------------------------
    // 7. 初始化 VP (可选)
    // -------------------------------------------------------------------------
#ifdef USE_VP
    std::shared_ptr<SpacemiT::VpEngine> vp_engine;
    if (cfg.vp_enabled) {
        if (cfg.vp_list) {
            auto vp_result = initVP(cfg.vp_database, cfg.vp_threads, cfg.vp_threshold);
            if (!vp_result.engine) return 1;
            auto speakers = vp_result.engine->GetAllSpeakers();
            std::cout << "已注册说话人 (" << speakers.size() << "):\n";
            for (size_t i = 0; i < speakers.size(); i++) {
                std::cout << "  " << (i + 1) << ". " << speakers[i] << "\n";
            }
            return 0;
        }

        auto vp_result = initVP(cfg.vp_database, cfg.vp_threads, cfg.vp_threshold);
        if (!vp_result.engine) return 1;
        vp_engine = vp_result.engine;
        std::cout << getTimestamp() << " 声纹识别: ON (db: " << cfg.vp_database
            << ", threshold=" << formatVpScore(vp_engine->GetThreshold()) << ")\n";
    }
#endif

    // -------------------------------------------------------------------------
    // 播放队列和播放线程
    // -------------------------------------------------------------------------
    std::queue<std::vector<uint8_t>> playback_queue;
    std::mutex playback_mutex;
    std::condition_variable playback_cv;
    std::atomic<bool> is_playing{false};

    const int playback_frames_per_buffer = std::max(512, cfg.playback_rate / 50);
    if (!player.Start(cfg.playback_rate, cfg.playback_channels,
            playback_frames_per_buffer)) {
        std::cerr << getTimestamp() << " 错误: 无法启动播放设备\n";
        return 1;
    }

    // 播放线程：阻塞写本身就是节拍器；队列空时连续写静音防止 ALSA XRUN。
    std::thread playback_thread([&]() {
        const size_t silence_frames = cfg.playback_rate / 50;  // 20ms
        const size_t silence_bytes =
            silence_frames * cfg.playback_channels * sizeof(int16_t);
        const std::vector<uint8_t> silence(silence_bytes, 0);

        while (g_running) {
            std::vector<uint8_t> chunk;
            {
                std::unique_lock<std::mutex> lock(playback_mutex);
                playback_cv.wait_for(
                    lock, std::chrono::milliseconds(20),
                    [&] { return !playback_queue.empty() || !g_running; });
                if (!g_running) break;
                if (!playback_queue.empty()) {
                    chunk = std::move(playback_queue.front());
                    playback_queue.pop();
                }
            }
            if (chunk.empty()) {
                if (!player.Write(silence)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                is_playing = false;
            } else {
                is_playing = true;
                if (!player.Write(chunk)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                {
                    std::lock_guard<std::mutex> lock(playback_mutex);
                    if (playback_queue.empty()) {
                        is_playing = false;
                    }
                }
            }
        }
    });

    std::mutex playback_enqueue_mutex;

    auto resampleForPlayback = [&](const std::vector<float>& float_samples, int src_rate) {
        if (float_samples.empty()) {
            return std::vector<float>{};
        }
        if (src_rate <= 0 || src_rate == cfg.playback_rate) {
            return float_samples;
        }
        if (src_rate == tts_sample_rate && playback_resampler) {
            return playback_resampler->process(float_samples);
        }

        Resampler::Config rconf;
        rconf.input_sample_rate = src_rate;
        rconf.output_sample_rate = cfg.playback_rate;
        rconf.channels = 1;
        rconf.method = (src_rate < cfg.playback_rate)
            ? ResampleMethod::LINEAR_UPSAMPLE
            : ResampleMethod::LINEAR_DOWNSAMPLE;
        Resampler resampler(rconf);
        if (!resampler.initialize()) {
            std::cerr << getTimestamp()
                << " [Audio] 播放重采样器初始化失败: "
                << src_rate << "Hz -> " << cfg.playback_rate << "Hz\n";
            return std::vector<float>{};
        }
        return resampler.process(float_samples);
    };

    // 入队播放数据：float mono -> resample -> expand channels -> PCM16 bytes -> enqueue
    auto enqueuePlayback = [&](const std::vector<float>& float_samples, int src_rate) {
        std::lock_guard<std::mutex> enqueue_lock(playback_enqueue_mutex);
        std::vector<float> resampled = resampleForPlayback(float_samples, src_rate);
        if (resampled.empty()) {
            return;
        }

        size_t invalid_samples = 0;
        for (float& sample : resampled) {
            if (!std::isfinite(sample)) {
                sample = 0.0f;
                invalid_samples++;
            }
        }
        if (invalid_samples > 0) {
            std::cerr << getTimestamp() << " [TTS音频警告] 检测到 "
                << invalid_samples << " 个非法 sample，已替换为静音\n";
        }

        size_t total_samples;
        if (cfg.playback_channels > 1) {
            total_samples = resampled.size() * cfg.playback_channels;
        } else {
            total_samples = resampled.size();
        }

        std::vector<uint8_t> pcm_bytes(total_samples * 2);
        int16_t* out = reinterpret_cast<int16_t*>(pcm_bytes.data());

        if (cfg.playback_channels > 1) {
            for (size_t i = 0; i < resampled.size(); ++i) {
                int16_t sample = static_cast<int16_t>(
                    std::clamp(resampled[i], -1.0f, 1.0f) * 32767.0f);
                for (int ch = 0; ch < cfg.playback_channels; ++ch) {
                    out[i * cfg.playback_channels + ch] = sample;
                }
            }
        } else {
            for (size_t i = 0; i < resampled.size(); ++i) {
                out[i] = static_cast<int16_t>(
                    std::clamp(resampled[i], -1.0f, 1.0f) * 32767.0f);
            }
        }

        {
            const size_t chunk_bytes =
                (cfg.playback_rate / 50) * cfg.playback_channels * sizeof(int16_t);
            std::lock_guard<std::mutex> lock(playback_mutex);
            for (size_t offset = 0; offset < pcm_bytes.size(); offset += chunk_bytes) {
                size_t len = std::min(chunk_bytes, pcm_bytes.size() - offset);
                playback_queue.push(std::vector<uint8_t>(
                    pcm_bytes.data() + offset, pcm_bytes.data() + offset + len));
            }
        }
        playback_cv.notify_one();
    };

    auto clearPlayback = [&]() {
        std::lock_guard<std::mutex> enqueue_lock(playback_enqueue_mutex);
        {
            std::lock_guard<std::mutex> lock(playback_mutex);
            std::queue<std::vector<uint8_t>> empty;
            playback_queue.swap(empty);
        }
        is_playing = false;
    };

    // -------------------------------------------------------------------------
    // 状态变量
    // -------------------------------------------------------------------------
    std::vector<float> audio_buffer;
    std::mutex buffer_mutex;
    int silence_frames_count = 0;
    const int silence_frames_threshold =
        static_cast<int>(cfg.silence_duration * 16000 / 512);
    bool is_speaking = false;
    float vad_segment_max_prob = 0.0f;

    const size_t PRE_BUFFER_FRAMES = 30;
    const size_t POST_WAKE_PRE_BUFFER_FRAMES = 6;
    const long long WAKE_HARD_DROP_MS = 120;
    const long long WAKE_EVENT_TTL_MS = 1500;
    std::deque<std::vector<float>> pre_buffer;

    int barge_in_confirm_frames = 0;
    const int BARGE_IN_CONFIRM_THRESHOLD = 5;

    std::atomic<bool> barge_in_recording{false};
    std::atomic<long long> wake_barge_in_request_ms{0};
    std::atomic<long long> wake_drop_until_ms{0};
    std::atomic<long long> wake_drop_start_ms{0};
    std::atomic<bool> wake_ready_pending{false};
    std::atomic<bool> wake_command_pending{false};
    std::atomic<bool> wake_asr_filter_pending{false};

    std::vector<int16_t> recorded_audio;
    std::mutex record_mutex;
    std::vector<int16_t> asr_recorded_audio;
    std::vector<int16_t> tts_recorded_audio;
    std::mutex tts_record_mutex;

    const size_t VAD_FRAME_SIZE = 512;
    std::vector<float> vad_frame_buffer;
    std::mutex vad_state_mutex;
    VadProgressPrinter vad_progress;
    vad_progress.Start();
    omni_agent::HidWakeListener wake_listener;
    std::queue<std::vector<float>> recognition_queue;
    std::mutex recognition_mutex;
    std::condition_variable recognition_cv;

    // -------------------------------------------------------------------------
    // 构造 VoicePipelineContext
    // -------------------------------------------------------------------------
    VoicePipelineContext pipeline_ctx;
    pipeline_ctx.llm = llm;
    pipeline_ctx.tts = tts;
    pipeline_ctx.vad = vad;
    pipeline_ctx.tts_sample_rate = tts_sample_rate;
    pipeline_ctx.system_prompt = system_prompt;
    if (cfg.save_tts_audio) {
        pipeline_ctx.save_tts_audio = [&](const std::vector<uint8_t>& pcm16_bytes) {
            std::lock_guard<std::mutex> lock(tts_record_mutex);
            size_t samples = pcm16_bytes.size() / sizeof(int16_t);
            tts_recorded_audio.reserve(tts_recorded_audio.size() + samples);
            for (size_t i = 0; i < samples; ++i) {
                uint16_t sample = static_cast<uint16_t>(pcm16_bytes[i * 2]) |
                    (static_cast<uint16_t>(pcm16_bytes[i * 2 + 1]) << 8);
                tts_recorded_audio.push_back(static_cast<int16_t>(sample));
            }
        };
    }
    pipeline_ctx.enqueue_playback = enqueuePlayback;
    pipeline_ctx.is_playing = [&]() { return is_playing.load(); };
    pipeline_ctx.clear_playback = clearPlayback;
    pipeline_ctx.audio_buffer = &audio_buffer;
    pipeline_ctx.buffer_mutex = &buffer_mutex;
    pipeline_ctx.silence_frames = &silence_frames_count;
    pipeline_ctx.is_speaking = &is_speaking;
    pipeline_ctx.barge_in_recording = &barge_in_recording;
    pipeline_ctx.vad_frame_buffer = &vad_frame_buffer;
    pipeline_ctx.vad_state_mutex = &vad_state_mutex;
    pipeline_ctx.pre_buffer = &pre_buffer;
#ifdef USE_MCP
    pipeline_ctx.mcp_manager = mcp.manager.get();
    pipeline_ctx.llm_tools_json = &mcp.llm_tools_json;
    pipeline_ctx.tools_mutex = &mcp.tools_mutex;
    pipeline_ctx.conversation_messages = &mcp.conversation_messages;
    pipeline_ctx.conversation_mutex = &mcp.conversation_mutex;
    pipeline_ctx.mcp_enabled = mcp.enabled;
#endif

    auto monotonicMs = []() -> long long {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };

    auto consumeWakeRequest = [&]() -> bool {
        long long wake_ms = wake_barge_in_request_ms.exchange(0);
        if (wake_ms <= 0) {
            return false;
        }
        long long age_ms = monotonicMs() - wake_ms;
        return age_ms >= 0 && age_ms <= WAKE_EVENT_TTL_MS;
    };

    auto wakeDropDurationMs = [&]() -> int {
        int duration_ms = std::max(0, cfg.wake_drop_audio_ms);
        if (cfg.wake_post_ack_tail_ms > 0 &&
                !wake_ack_clip.samples.empty() && wake_ack_clip.sample_rate > 0) {
            int ack_ms = static_cast<int>(
                wake_ack_clip.samples.size() * 1000 / wake_ack_clip.sample_rate);
            duration_ms = std::max(duration_ms,
                ack_ms + cfg.wake_post_ack_tail_ms);
        }
        return duration_ms;
    };

    auto isLoudWakeAudio = [](const std::vector<float>& samples) -> bool {
        if (samples.empty()) return false;
        double sum_sq = 0.0;
        float peak = 0.0f;
        for (float sample : samples) {
            float abs_sample = std::abs(sample);
            peak = std::max(peak, abs_sample);
            sum_sq += static_cast<double>(sample) * sample;
        }
        float rms = static_cast<float>(std::sqrt(sum_sq / samples.size()));
        return peak >= 0.40f || rms >= 0.14f;
    };

    auto appendPreBuffer = [&](size_t max_frames) {
        size_t start = 0;
        if (pre_buffer.size() > max_frames) {
            start = pre_buffer.size() - max_frames;
        }
        for (size_t i = start; i < pre_buffer.size(); ++i) {
            audio_buffer.insert(audio_buffer.end(),
                pre_buffer[i].begin(), pre_buffer[i].end());
        }
        pre_buffer.clear();
    };

    auto resetWakeInputState = [&]() {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex);
            audio_buffer.clear();
            pre_buffer.clear();
            silence_frames_count = 0;
            is_speaking = false;
            vad_segment_max_prob = 0.0f;
        }
        barge_in_recording = false;
        {
            std::lock_guard<std::mutex> lock(vad_state_mutex);
            vad_frame_buffer.clear();
            vad->Reset();
        }
    };

    auto playWakeAck = [&]() {
        if (!wake_ack_clip.samples.empty() && wake_ack_clip.sample_rate > 0) {
            enqueuePlayback(wake_ack_clip.samples, wake_ack_clip.sample_rate);
        }
    };

    auto enqueueRecognition = [&](std::vector<float> utterance) {
        if (utterance.empty()) return;
        {
            std::lock_guard<std::mutex> lock(recognition_mutex);
            recognition_queue.push(std::move(utterance));
        }
        recognition_cv.notify_one();
    };

    std::thread recognition_thread([&]() {
        while (true) {
            std::vector<float> utterance;
            {
                std::unique_lock<std::mutex> lock(recognition_mutex);
                recognition_cv.wait(lock, [&]() {
                    return !g_running || !recognition_queue.empty();
                });
                if (recognition_queue.empty()) {
                    if (!g_running) {
                        break;
                    }
                    continue;
                }
                utterance = std::move(recognition_queue.front());
                recognition_queue.pop();
            }

#ifdef USE_VP
            std::string speaker_tag;
            bool vp_passed = true;
            if (vp_engine) {
                std::shared_ptr<SpacemiT::VpResult> vp_res;
                if (!cfg.vp_verify.empty()) {
                    vp_res = vp_engine->Verify(cfg.vp_verify, utterance, 16000);
                    if (vp_res && vp_res->IsSuccess()) {
                        std::cout << getTimestamp() << " [VP] 验证 \""
                            << cfg.vp_verify << "\": "
                            << (vp_res->IsVerified() ? "通过" : "不通过")
                            << " (score: " << formatVpScore(vp_res->GetScore())
                            << ", threshold: " << formatVpScore(vp_engine->GetThreshold())
                            << ")" << std::endl;
                        if (vp_res->IsVerified()) {
                            speaker_tag = "[" + cfg.vp_verify + "] ";
                        } else {
                            vp_passed = false;
                        }
                    } else {
                        vp_passed = false;
                    }
                } else {
                    vp_res = vp_engine->Identify(utterance, 16000);
                    if (vp_res && vp_res->IsSuccess()) {
                        if (vp_res->IsIdentified()) {
                            std::cout << getTimestamp() << " [VP] 说话人: "
                                << vp_res->GetName()
                                << " (score: " << formatVpScore(vp_res->GetScore())
                                << ", threshold: " << formatVpScore(vp_engine->GetThreshold())
                                << ")" << std::endl;
                            speaker_tag = "[" + vp_res->GetName() + "] ";
                        } else {
                            vp_passed = false;
                        }
                        auto matches = vp_res->GetMatches();
                        int show_n = cfg.vp_verbose
                            ? static_cast<int>(matches.size())
                            : std::min(cfg.vp_top, static_cast<int>(matches.size()));
                        if (show_n > 1 || cfg.vp_verbose) {
                            for (int k = 0; k < show_n; k++) {
                                std::cout << getTimestamp() << " [VP]   "
                                    << (k + 1) << ". " << matches[k].name
                                    << " (score: " << formatVpScore(matches[k].score) << ")"
                                    << (matches[k].score >= vp_engine->GetThreshold() ? " *" : "")
                                    << std::endl;
                            }
                        }
                    } else {
                        vp_passed = false;
                    }
                }
                if (!vp_passed) {
                    std::cout << getTimestamp() << " [VP] 未识别说话人，丢弃音频";
                    if (!cfg.vp_verify.empty()) {
                        if (vp_res && vp_res->IsSuccess()) {
                            std::cout << " (score: " << formatVpScore(vp_res->GetScore())
                                << ", threshold: " << formatVpScore(vp_engine->GetThreshold())
                                << ")";
                        } else {
                            std::cout << " (threshold: " << formatVpScore(vp_engine->GetThreshold())
                                << ")";
                        }
                    } else {
                        auto matches = vp_res ? vp_res->GetMatches() : std::vector<SpacemiT::SpeakerMatch>{};
                        if (!matches.empty()) {
                            std::cout << " (best: " << matches[0].name
                                << ", score: " << formatVpScore(matches[0].score)
                                << ", threshold: " << formatVpScore(vp_engine->GetThreshold())
                                << ")";
                        } else {
                            std::cout << " (threshold: " << formatVpScore(vp_engine->GetThreshold())
                                << ")";
                        }
                    }
                    std::cout << std::endl;
                    g_processing = false;
                    continue;
                }
            }
#endif

            AsrAudioPreprocessStats asr_audio_stats = preprocessAsrAudio(&utterance);
            std::cout << getTimestamp() << " [ASR] 音频增益: rms="
                << formatFloat(asr_audio_stats.input_rms, 4)
                << " active=" << formatFloat(asr_audio_stats.active_rms, 4)
                << " peak=" << formatFloat(asr_audio_stats.input_peak, 3)
                << " gain=" << formatFloat(asr_audio_stats.gain, 2)
                << " out_peak=" << formatFloat(asr_audio_stats.output_peak, 3);
            if (asr_audio_stats.clipped_samples > 0) {
                std::cout << " clipped=" << asr_audio_stats.clipped_samples;
            }
            std::cout << std::endl;
            if (cfg.save_asr_audio) {
                std::vector<int16_t> asr_pcm = floatToPcm16(utterance);
                asr_recorded_audio.insert(
                    asr_recorded_audio.end(), asr_pcm.begin(), asr_pcm.end());
            }

            std::cout << getTimestamp() << " [ASR] 开始识别..." << std::endl;
            auto result = asr->Recognize(utterance, 16000);
            if (result && !result->IsEmpty()) {
                std::string text = result->GetText();
                std::cout << getTimestamp() << " [ASR] 识别完成: \""
                    << text << "\"" << std::endl;
                bool apply_wake_text_filter =
                    cfg.wake_enabled && cfg.wake_drop_asr;
                if (wake_asr_filter_pending.exchange(false)) {
                    apply_wake_text_filter = true;
                }
                if (apply_wake_text_filter) {
                    WakeAsrTextFilterResult filtered = filterWakeAsrText(text);
                    if (filtered.drop) {
                        std::cout << getTimestamp()
                            << " [Wake] 丢弃唤醒词ASR: \"" << text << "\"\n";
                        g_processing = false;
                        std::cout << getTimestamp()
                            << " [等待语音输入...]\n" << std::flush;
                        continue;
                    }
                    if (filtered.changed) {
                        std::cout << getTimestamp()
                            << " [Wake] 过滤唤醒词ASR: \"" << text
                            << "\" -> \"" << filtered.text << "\"\n";
                        text = filtered.text;
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(g_process_thread_mutex);
                    if (g_process_thread && g_process_thread->joinable()) {
                        g_process_thread->join();
                    }
#ifdef USE_VP
                    std::string final_text = speaker_tag + text;
#else
                    const std::string& final_text = text;
#endif
                    g_process_thread = std::make_unique<std::thread>(
                        [&pipeline_ctx, final_text]() {
                            processText(pipeline_ctx, final_text);
                        });
                }
            } else {
                wake_asr_filter_pending = false;
                std::cout << getTimestamp() << " [ASR] 识别完成: (无结果)" << std::endl;
                g_processing = false;
                std::cout << getTimestamp() << " [等待语音输入...]\n" << std::flush;
            }
        }
    });

    // -------------------------------------------------------------------------
    // 录音回调只入队，避免在 PortAudio 回调线程里跑 VAD/DOA/重采样。
    // -------------------------------------------------------------------------
    struct CaptureChunk {
        std::vector<uint8_t> bytes;
        uint64_t generation = 0;
    };

    std::deque<CaptureChunk> capture_queue;
    std::mutex capture_queue_mutex;
    std::condition_variable capture_queue_cv;
    constexpr size_t kMaxCaptureQueueChunks = 80;
    constexpr long long kPostFlushCaptureIgnoreMs = 250;
    std::atomic<size_t> capture_queue_drops{0};
    std::atomic<uint64_t> capture_queue_generation{0};
    std::atomic<long long> capture_ignore_until_ms{0};
    std::atomic<bool> audio_frontend_error_logged{false};

    auto isCurrentCaptureGeneration = [&](uint64_t generation) {
        return generation == capture_queue_generation.load(std::memory_order_acquire);
    };

    auto flushPendingCapture = [&]() {
        capture_queue_generation.fetch_add(1, std::memory_order_acq_rel);
        capture_ignore_until_ms.store(
            monotonicMs() + kPostFlushCaptureIgnoreMs, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(capture_queue_mutex);
            capture_queue.clear();
        }
        capture_queue_drops.store(0);
        capture_queue_cv.notify_all();
    };
    pipeline_ctx.flush_pending_capture = flushPendingCapture;

    auto processCaptureChunk = [&](const uint8_t* data, size_t size, uint64_t generation) {
        if (!g_running || data == nullptr || size == 0) return;
        if (!isCurrentCaptureGeneration(generation)) return;

        // PCM16 little-endian -> float
        size_t num_samples = size / 2;
        const int16_t* pcm = reinterpret_cast<const int16_t*>(data);
        std::vector<float> float_samples(num_samples);
        for (size_t i = 0; i < num_samples; ++i) {
            float_samples[i] = pcm[i] / 32768.0f;
        }

        // 多声道取 speech channel 到 mono；DOA 使用原始多声道。
        if (cfg.capture_channels > 1) {
            size_t frames = num_samples / cfg.capture_channels;
#ifdef USE_DOA
            if (doa_runtime.enabled()) {
                doa_runtime.ProcessInterleaved(float_samples.data(), frames,
                    cfg.capture_channels);
            }
#endif
            std::vector<float> mono(frames);
            const int speech_idx = std::clamp(cfg.speech_channel - 1, 0,
                cfg.capture_channels - 1);
            for (size_t i = 0; i < frames; ++i) {
                mono[i] = float_samples[i * cfg.capture_channels + speech_idx];
            }

            float_samples = std::move(mono);
        }

        // 重采样到 16kHz（如果需要）
        std::vector<float> samples_16k;
        if (capture_resampler) {
            samples_16k = capture_resampler->process(float_samples);
        } else {
            samples_16k = std::move(float_samples);
        }

        if (samples_16k.empty()) return;
        if (!isCurrentCaptureGeneration(generation)) return;

        if (cfg.wake_enabled && cfg.wake_interrupt_mode && cfg.wake_drop_asr) {
            long long drop_until = wake_drop_until_ms.load();
            if (drop_until > 0) {
                long long now_ms = monotonicMs();
                long long drop_start = wake_drop_start_ms.load();
                long long elapsed_ms = drop_start > 0 ? now_ms - drop_start : 0;
                bool loud_wake_audio = isLoudWakeAudio(samples_16k);
                if ((drop_start > 0 && elapsed_ms < WAKE_HARD_DROP_MS) ||
                        (loud_wake_audio && (now_ms < drop_until ||
                            (drop_start > 0 && elapsed_ms < 1600)))) {
                    resetWakeInputState();
                    return;
                }
                wake_drop_until_ms.store(0);
                wake_drop_start_ms.store(0);
                resetWakeInputState();
                wake_command_pending = true;
                if (wake_ready_pending.exchange(false)) {
                    std::cout << getTimestamp()
                        << " [Wake] ready for command after "
                        << elapsed_ms << "ms\n" << std::flush;
                }
            }
        }

#ifdef USE_AUDIO_FRONTEND
        if (audio_frontend) {
            std::vector<float> enhanced;
            if (audio_frontend->ProcessChunk(samples_16k, &enhanced)) {
                samples_16k = std::move(enhanced);
                if (samples_16k.empty()) {
                    return;
                }
            } else if (!audio_frontend_error_logged.exchange(true)) {
                std::cerr << getTimestamp()
                    << " [AudioFrontend] WebRTC处理失败，后续音频旁路\n";
            }
        }
#endif

        if (!isCurrentCaptureGeneration(generation)) return;

        // 录制音频（用于调试）
        if (cfg.save_audio) {
            std::lock_guard<std::mutex> lock(record_mutex);
            for (float s : samples_16k) {
                recorded_audio.push_back(static_cast<int16_t>(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
            }
        }

        // 累积音频到 VAD 帧缓冲区
        {
            std::lock_guard<std::mutex> lock(vad_state_mutex);
            vad_frame_buffer.insert(vad_frame_buffer.end(),
                samples_16k.begin(), samples_16k.end());
        }

        while (g_running) {
            if (!isCurrentCaptureGeneration(generation)) return;

            std::vector<float> vad_frame;
            float vad_prob = 0.0f;

            {
                std::lock_guard<std::mutex> lock(vad_state_mutex);
                if (vad_frame_buffer.size() < VAD_FRAME_SIZE) {
                    break;
                }
                vad_frame.assign(vad_frame_buffer.begin(),
                    vad_frame_buffer.begin() + VAD_FRAME_SIZE);
                vad_frame_buffer.erase(vad_frame_buffer.begin(),
                    vad_frame_buffer.begin() + VAD_FRAME_SIZE);

                auto vad_result = vad->Detect(vad_frame);
                vad_prob = vad_result ? vad_result->GetProbability() : 0.0f;
            }

            // TTS 播放期间：检测 barge-in
            if (g_processing) {
                if (cfg.wake_enabled && consumeWakeRequest()) {
                    if (cfg.wake_interrupt_mode) {
                        std::cout << "\n" << getTimestamp()
                            << " [Wake] HID唤醒，中断TTS并播放提示音\n";
                        g_barge_in = true;
                        clearPlayback();
                        playWakeAck();
                        if (cfg.wake_drop_asr) {
                            wake_ready_pending = true;
                            wake_asr_filter_pending = true;
                            long long now_ms = monotonicMs();
                            wake_drop_start_ms.store(now_ms);
                            wake_drop_until_ms.store(now_ms + wakeDropDurationMs());
                        }
#ifdef USE_DOA
                        if (doa_runtime.enabled()) {
                            doa_runtime.Reset();
                        }
#endif
                        barge_in_confirm_frames = 0;
                        resetWakeInputState();
                        continue;
                    }

                    std::cout << "\n" << getTimestamp()
                        << " [Wake] HID唤醒，停止播放\n";
                    g_barge_in = true;
                    clearPlayback();
#ifdef USE_DOA
                    if (doa_runtime.enabled()) {
                        doa_runtime.Reset();
                    }
#endif
                    barge_in_recording = true;
                    barge_in_confirm_frames = 0;

                    std::lock_guard<std::mutex> lock(buffer_mutex);
                    is_speaking = true;
                    vad_segment_max_prob = vad_prob;
                    audio_buffer.clear();
                    for (const auto& frame : pre_buffer) {
                        audio_buffer.insert(audio_buffer.end(), frame.begin(), frame.end());
                    }
                    audio_buffer.insert(audio_buffer.end(), vad_frame.begin(), vad_frame.end());
                    pre_buffer.clear();
                    silence_frames_count = 0;
                    vad_progress.Publish(vad_prob, cfg.vad_threshold,
                        audio_buffer.size(), true, true);
                    continue;
                }

                if (barge_in_recording && is_speaking) {
                    std::lock_guard<std::mutex> lock(buffer_mutex);
                    audio_buffer.insert(audio_buffer.end(), vad_frame.begin(), vad_frame.end());
                    vad_progress.Publish(vad_prob, cfg.vad_threshold, audio_buffer.size(), true);

                    if (vad_prob <= cfg.vad_threshold) {
                        silence_frames_count++;
                    } else {
                        silence_frames_count = 0;
                    }
                    continue;
                }

                if (!cfg.wake_enabled && is_playing.load() && vad_prob > cfg.vad_threshold) {
                    barge_in_confirm_frames++;
                    pre_buffer.push_back(vad_frame);
                    if (pre_buffer.size() > PRE_BUFFER_FRAMES + BARGE_IN_CONFIRM_THRESHOLD) {
                        pre_buffer.pop_front();
                    }

                    if (barge_in_confirm_frames >= BARGE_IN_CONFIRM_THRESHOLD) {
                        std::cout << "\n" << getTimestamp() << " [Barge-in] 用户打断 (连续"
                            << barge_in_confirm_frames << "帧, prob=" << vad_prob
                            << ")，停止播放\n";
                        g_barge_in = true;
                        clearPlayback();
#ifdef USE_DOA
                        if (doa_runtime.enabled()) {
                            doa_runtime.Reset();
                        }
#endif
                        barge_in_recording = true;
                        barge_in_confirm_frames = 0;

                        std::lock_guard<std::mutex> lock(buffer_mutex);
                        is_speaking = true;
                        vad_segment_max_prob = vad_prob;
                        audio_buffer.clear();
                        for (const auto& frame : pre_buffer) {
                            audio_buffer.insert(audio_buffer.end(), frame.begin(), frame.end());
                        }
                        pre_buffer.clear();
                        silence_frames_count = 0;
                        vad_progress.Publish(vad_prob, cfg.vad_threshold, audio_buffer.size(), true, true);
                    }
                } else {
                    barge_in_confirm_frames = 0;
                    pre_buffer.push_back(vad_frame);
                    if (pre_buffer.size() > PRE_BUFFER_FRAMES) {
                        pre_buffer.pop_front();
                    }
                }
                continue;
            }

            std::lock_guard<std::mutex> lock(buffer_mutex);

            if (vad_prob > cfg.vad_threshold) {
                if (!is_speaking) {
#ifdef USE_DOA
                    if (doa_runtime.enabled()) {
                        doa_runtime.Reset();
                    }
#endif
                    is_speaking = true;
                    audio_buffer.clear();

                    if (wake_command_pending.exchange(false)) {
                        appendPreBuffer(POST_WAKE_PRE_BUFFER_FRAMES);
                    } else {
                        appendPreBuffer(PRE_BUFFER_FRAMES);
                    }

                    std::cout << "\n";
                }
                audio_buffer.insert(audio_buffer.end(), vad_frame.begin(), vad_frame.end());
                vad_segment_max_prob = std::max(vad_segment_max_prob, vad_prob);
                vad_progress.Publish(vad_prob, cfg.vad_threshold, audio_buffer.size(), true, true);
                silence_frames_count = 0;
            } else if (is_speaking) {
                audio_buffer.insert(audio_buffer.end(), vad_frame.begin(), vad_frame.end());
                vad_segment_max_prob = std::max(vad_segment_max_prob, vad_prob);
                vad_progress.Publish(vad_prob, cfg.vad_threshold, audio_buffer.size(), true);
                silence_frames_count++;

                if (silence_frames_count >= silence_frames_threshold) {
                    is_speaking = false;
                    barge_in_recording = false;
                    std::cout << "\r" << getTimestamp() << " [VAD] 停止说话，触发识别";
                    std::cout << " max_prob=" << formatFloat(vad_segment_max_prob, 2)
                        << " threshold=" << formatFloat(cfg.vad_threshold, 2);
#ifdef USE_DOA
                    SpacemitAudio::MultiSoundLocatorResult doa_result;
                    if (doa_runtime.GetLatestValid(&doa_result)) {
                        std::cout << " DOA=" << std::fixed << std::setprecision(1)
                            << doa_result.azimuth_deg << "deg";
                    } else if (doa_runtime.enabled()) {
                        std::cout << " DOA=--";
                    }
#endif
                    std::cout << std::endl;

                    if (audio_buffer.size() > 8000) {
                        wake_barge_in_request_ms.store(0);
                        g_processing = true;
                        enqueueRecognition(audio_buffer);
                    }

                    audio_buffer.clear();
                    silence_frames_count = 0;
                    vad_segment_max_prob = 0.0f;
                }
            } else {
                pre_buffer.push_back(vad_frame);
                if (pre_buffer.size() > PRE_BUFFER_FRAMES) {
                    pre_buffer.pop_front();
                }
                vad_progress.Publish(vad_prob, cfg.vad_threshold, 0, false);
            }
        }
    };

    std::thread capture_processing_thread([&]() {
        while (true) {
            CaptureChunk chunk;
            {
                std::unique_lock<std::mutex> lock(capture_queue_mutex);
                capture_queue_cv.wait(lock, [&]() {
                    return !g_running || !capture_queue.empty();
                });
                if (capture_queue.empty()) {
                    if (!g_running) {
                        break;
                    }
                    continue;
                }
                chunk = std::move(capture_queue.front());
                capture_queue.pop_front();
            }

            if (!isCurrentCaptureGeneration(chunk.generation)) {
                continue;
            }
            size_t dropped = capture_queue_drops.exchange(0);
            if (dropped > 0) {
                std::cout << getTimestamp() << " [Audio] capture queue dropped "
                    << dropped << " chunks\n";
            }
            processCaptureChunk(chunk.bytes.data(), chunk.bytes.size(), chunk.generation);
        }
    });

    capture.SetCallback([&](const uint8_t* data, size_t size) {
        if (!g_running || data == nullptr || size == 0) return;
        long long ignore_until_ms = capture_ignore_until_ms.load(std::memory_order_acquire);
        if (ignore_until_ms > 0) {
            long long now_ms = monotonicMs();
            if (now_ms < ignore_until_ms) {
                return;
            }
            capture_ignore_until_ms.compare_exchange_strong(
                ignore_until_ms, 0, std::memory_order_acq_rel);
        }
        CaptureChunk chunk;
        chunk.bytes.assign(data, data + size);
        chunk.generation = capture_queue_generation.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lock(capture_queue_mutex);
            if (capture_queue.size() >= kMaxCaptureQueueChunks) {
                capture_queue.pop_front();
                capture_queue_drops.fetch_add(1);
            }
            capture_queue.push_back(std::move(chunk));
        }
        capture_queue_cv.notify_one();
    });

    auto shutdownRuntime = [&](bool capture_started) {
        g_running = false;
        wake_listener.Stop();
        clearPlayback();
        if (capture_started) {
            capture.Stop();
            capture.Close();
        }
        capture_queue_cv.notify_all();
        if (capture_processing_thread.joinable()) {
            capture_processing_thread.join();
        }
        vad_progress.Stop();
        recognition_cv.notify_all();
        playback_cv.notify_all();
        if (recognition_thread.joinable()) {
            recognition_thread.join();
        }
        {
            std::lock_guard<std::mutex> lock(g_process_thread_mutex);
            if (g_process_thread && g_process_thread->joinable()) {
                g_process_thread->join();
            }
        }
        if (playback_thread.joinable()) {
            playback_thread.join();
        }
        player.Stop();
        player.Close();
    };

    // -------------------------------------------------------------------------
    // 开始对话
    // -------------------------------------------------------------------------
    if (cfg.wake_enabled) {
        std::string wake_error;
        if (!wake_listener.Start(cfg.wake_device, [&]() {
                if (g_processing) {
                    wake_barge_in_request_ms.store(monotonicMs());
                }
            }, &wake_error)) {
            std::cerr << getTimestamp() << " 错误: 无法启动 HID 唤醒: "
                << wake_error << "\n";
            shutdownRuntime(false);
            return 1;
        }
        std::cout << getTimestamp() << " HID唤醒监听: " << cfg.wake_device << "\n";
    }

    playStartupGreeting(pipeline_ctx, cfg.startup_greeting);
    std::cout << getTimestamp() << " [等待语音输入...]\n" << std::flush;

    if (!capture.Start(cfg.capture_rate, cfg.capture_channels)) {
        std::cerr << getTimestamp() << " 错误: 无法启动录音设备\n";
        shutdownRuntime(false);
        return 1;
    }

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    shutdownRuntime(true);

#ifdef USE_MCP
    if (mcp.enabled) {
        if (mcp.registry_poll_thread.joinable()) {
            mcp.registry_poll_thread.join();
        }
        if (mcp.manager) {
            mcp.manager->stopAll();
        }
        std::cout << getTimestamp() << " [MCP] 已清理\n";
    }
#endif

    if (cfg.save_audio && !recorded_audio.empty()) {
        std::cout << getTimestamp() << " [保存音频] " << cfg.audio_file
            << " (" << recorded_audio.size() << " samples, "
            << (recorded_audio.size() / 16000.0f) << " 秒)\n";
        saveWav(cfg.audio_file, recorded_audio, 16000);
    }
    if (cfg.save_asr_audio && !asr_recorded_audio.empty()) {
        std::cout << getTimestamp() << " [保存ASR音频] " << cfg.asr_audio_file
            << " (" << asr_recorded_audio.size() << " samples, "
            << (asr_recorded_audio.size() / 16000.0f) << " 秒)\n";
        saveWav(cfg.asr_audio_file, asr_recorded_audio, 16000);
    }
    if (cfg.save_tts_audio && !tts_recorded_audio.empty()) {
        std::cout << getTimestamp() << " [保存TTS音频] " << cfg.tts_audio_file
            << " (" << tts_recorded_audio.size() << " samples, "
            << (tts_recorded_audio.size() / static_cast<double>(tts_sample_rate)) << " 秒)\n";
        saveWav(cfg.tts_audio_file, tts_recorded_audio, tts_sample_rate);
    }

    std::cout << "\n" << getTimestamp() << " [已退出]\n";
    return 0;
}
