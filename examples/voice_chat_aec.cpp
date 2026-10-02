/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * 带 AEC 的语音对话系统 Demo (全双工模式)
 *
 * 基于 main_voice_chat.cpp，使用 AecDuplexProcessor 实现回声消除
 * 支持 barge-in（用户打断 TTS 播放）
 *
 * 线程模型: 声卡回调 → AEC 处理线程 (APM + VAD) → 识别线程 (声纹 + ASR)
 *           → LLM/TTS 线程；识别和生成都不会阻塞 APM。
 *
 * 用法:
 *   ./voice_chat_aec [--tts matcha:zh|matcha:en|matcha:zh-en|kokoro|kokoro:<voice>] [--model qwen2.5:0.5b] [--input-device 0] [--output-device 0] [--sample-rate 48000]
 */

#include <iostream>
#include <string>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <deque>
#include <functional>
#include <memory>
#include <sstream>
#include <vector>
#include <cmath>

// AEC 处理器
#include "aec_duplex_processor.hpp"

// 全双工音频（用于列出设备）
#include "audio_duplex.hpp"

// Shared modules
#include "voice_common.hpp"
#include "wake_text_filter.hpp"
#include "engine_init.hpp"
#include "hid_wake_listener.hpp"
#ifdef USE_KWS
#include "kws_wake_listener.hpp"
#endif
#include "voice_pipeline.hpp"
#ifdef USE_DOA
#include "doa_runtime.hpp"
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
    std::string wake_source = "hid";       // hid = SPV board report, kws = on-device KWS model (USE_KWS)
    std::string wake_device = "/dev/hidraw0";
    std::string kws_model_dir;             // empty = KWS_MODEL_DIR or ~/.cache/models/kws/xiaojin-v1
    float kws_threshold = 0.3f;
    int kws_holdoff_ms = 1000;
    float kws_partial_threshold = 0.0f;    // 0 = off
    bool kws_echo_null = false;            // KWS path: online loudspeaker null over 3 raw mics
    int kws_echo_null_first_channel = 2;   // 1-based first of the three mics
    bool wake_interrupt_mode = true;
    std::string wake_ack_audio = "~/.cache/models/assets/audio/006_im_here.wav";
    bool wake_strip_asr = true;
    std::vector<std::string> wake_phrases =
        omni_agent::flattenWakePhrases(omni_agent::defaultWakePhrases());
    bool wake_phrases_set = false;
    int wake_command_timeout_ms = 5000;
    int max_tokens = 150;
    int reasoning_budget = -1;
    std::string system_prompt = "You are a helpful assistant.";
    std::string startup_greeting;
    bool list_devices = false;

    // AEC 配置
    bool aec_enabled = true;
    bool ns_enabled = true;
    // 0=kLow 1=kModerate 2=kHigh 3=kVeryHigh；默认 kModerate（强语音下噪声降 13~22 dB 而语音几乎不变，
    // 弱语音下也不像 kHigh 那样把语音一起压掉）
    int ns_level = 1;
    bool agc_enabled = false;
    // AGC2 固定数字增益 (dB)，NS 之后、限幅器之前；0 = 关闭
    float fixed_gain_db = 0.0f;
    int aec_delay_ms = 50;
    // AEC3 参考: 0 = 软件回采(写给喇叭的样本); n = 第 n 路采集是板端硬件回采, 乘 aec_reference_gain_db
    int aec_reference_channel = 0;
    float aec_reference_gain_db = 12.0f;
    // 唤醒后屏蔽一段音频（提示音 + 被打断 TTS 的回声）: auto = 硬件回采参考时不屏蔽, 软件回采时屏蔽
    std::string wake_echo_guard = "auto";
    int buffer_frames = 0;
    int sample_rate = 48000;
    int capture_channels = 1;
    int playback_channels = 1;
    int speech_channel = 1;
    bool capture_channels_set = false;

    // 调试：音频录制
    bool save_audio = false;
    std::string audio_file = "aec_debug.wav";
    // 处理前的那一路麦克风（speech_channel），用于对照 AEC/NS 的实际增益
    bool save_raw_audio = false;
    std::string raw_audio_file = "aec_raw_debug.wav";
    bool save_asr_audio = false;
    std::string asr_audio_file = "aec_asr_debug.wav";
    bool save_tts_audio = false;
    std::string tts_audio_file = "tts_debug.wav";
    std::string aec_dump_dir;              // non-empty: stream frame-aligned debug WAVs here (--aec-dump-dir)

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
        } else if (strcmp(argv[i], "--no-aec") == 0) {
            cfg.aec_enabled = false;
        } else if (strcmp(argv[i], "--no-ns") == 0) {
            cfg.ns_enabled = false;
        } else if (strcmp(argv[i], "--ns-level") == 0 && i + 1 < argc) {
            const std::string lv = argv[++i];
            if (lv == "low") cfg.ns_level = 0;
            else if (lv == "moderate") cfg.ns_level = 1;
            else if (lv == "high") cfg.ns_level = 2;
            else if (lv == "veryhigh") cfg.ns_level = 3;
            else {
                std::cerr << "错误: --ns-level 需要 low|moderate|high|veryhigh，收到 " << lv << "\n";
                exit(1);
            }
        } else if (strcmp(argv[i], "--fixed-gain") == 0 && i + 1 < argc) {
            cfg.fixed_gain_db = std::stof(argv[++i]);
        } else if (strcmp(argv[i], "--agc") == 0) {
            cfg.agc_enabled = true;
        } else if (strcmp(argv[i], "--aec-delay") == 0 && i + 1 < argc) {
            cfg.aec_delay_ms = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--aec-reference-channel") == 0 && i + 1 < argc) {
            cfg.aec_reference_channel = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--aec-reference-gain") == 0 && i + 1 < argc) {
            cfg.aec_reference_gain_db = std::stof(argv[++i]);
        } else if (strcmp(argv[i], "--wake-echo-guard") == 0 && i + 1 < argc) {
            cfg.wake_echo_guard = argv[++i];
        } else if (strcmp(argv[i], "--buffer-frames") == 0 && i + 1 < argc) {
            cfg.buffer_frames = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--sample-rate") == 0 && i + 1 < argc) {
            cfg.sample_rate = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--capture-channels") == 0 && i + 1 < argc) {
            cfg.capture_channels = std::stoi(argv[++i]);
            cfg.capture_channels_set = true;
        } else if (strcmp(argv[i], "--playback-channels") == 0 && i + 1 < argc) {
            cfg.playback_channels = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--speech-channel") == 0 && i + 1 < argc) {
            cfg.speech_channel = std::stoi(argv[++i]);
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
        } else if (strcmp(argv[i], "--aec-dump-dir") == 0 && i + 1 < argc) {
            cfg.aec_dump_dir = argv[++i];
        } else if (strcmp(argv[i], "--save-raw-audio") == 0) {
            cfg.save_raw_audio = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                cfg.raw_audio_file = argv[++i];
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
        } else if (strcmp(argv[i], "--wake-source") == 0 && i + 1 < argc) {
            cfg.wake_source = argv[++i];
        } else if (strcmp(argv[i], "--wake-device") == 0 && i + 1 < argc) {
            cfg.wake_device = argv[++i];
        } else if (strcmp(argv[i], "--kws-model-dir") == 0 && i + 1 < argc) {
            cfg.kws_model_dir = argv[++i];
        } else if (strcmp(argv[i], "--kws-threshold") == 0 && i + 1 < argc) {
            cfg.kws_threshold = std::stof(argv[++i]);
        } else if (strcmp(argv[i], "--kws-holdoff-ms") == 0 && i + 1 < argc) {
            cfg.kws_holdoff_ms = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--kws-partial-threshold") == 0 && i + 1 < argc) {
            cfg.kws_partial_threshold = std::stof(argv[++i]);
        } else if (strcmp(argv[i], "--kws-echo-null") == 0) {
            cfg.kws_echo_null = true;
        } else if (strcmp(argv[i], "--kws-echo-null-first-channel") == 0 && i + 1 < argc) {
            cfg.kws_echo_null_first_channel = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--wake-interrupt-mode") == 0) {
            cfg.wake_interrupt_mode = true;
        } else if (strcmp(argv[i], "--no-wake-interrupt-mode") == 0) {
            cfg.wake_interrupt_mode = false;
        } else if (strcmp(argv[i], "--wake-ack-audio") == 0 && i + 1 < argc) {
            cfg.wake_ack_audio = argv[++i];
        } else if ((strcmp(argv[i], "--wake-strip-asr") == 0) ||
                (strcmp(argv[i], "--wake-drop-asr") == 0)) {
            cfg.wake_strip_asr = true;
        } else if ((strcmp(argv[i], "--no-wake-strip-asr") == 0) ||
                (strcmp(argv[i], "--no-wake-drop-asr") == 0)) {
            cfg.wake_strip_asr = false;
        } else if (strcmp(argv[i], "--wake-phrase") == 0 && i + 1 < argc) {
            if (!cfg.wake_phrases_set) {
                cfg.wake_phrases.clear();
                cfg.wake_phrases_set = true;
            }
            cfg.wake_phrases.emplace_back(argv[++i]);
        } else if (strcmp(argv[i], "--wake-command-timeout-ms") == 0 && i + 1 < argc) {
            cfg.wake_command_timeout_ms = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--wake-drop-audio-ms") == 0 && i + 1 < argc) {
            ++i;
            std::cerr << "警告: --wake-drop-audio-ms 已弃用，连续采集模式不会丢弃唤醒后音频\n";
        } else if (strcmp(argv[i], "--wake-post-ack-tail-ms") == 0 && i + 1 < argc) {
            ++i;
            std::cerr << "警告: --wake-post-ack-tail-ms 已弃用，连续采集模式不会等待提示音\n";
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
                << "  -i, --input-device <id>   输入设备索引 (默认: 系统默认)\n"
                << "  -o, --output-device <id>  输出设备索引 (默认: 系统默认)\n"
                << "  -l, --list-devices        列出可用音频设备\n"
                << "\nLLM:\n"
                << "  --model <name>            LLM模型 (默认: qwen2.5:0.5b)\n"
                << "  --llm-url <url>           LLM API地址 (必填)\n"
                << "  --max-tokens <n>          最大生成 token 数 (默认: 150)\n"
                << "  --reasoning-budget <n>    reasoning budget；0 表示隐藏思考输出\n"
                << "  --system-prompt <text>    系统提示词\n"
                << "\nVAD:\n"
                << "  --vad-threshold <0-1>     VAD触发阈值 (默认: 0.8)\n"
                << "  --silence-duration <sec>  静音结束判定时长 (默认: 0.5)\n"
                << "\nASR:\n"
                << "  --asr-engine <name>       ASR后端: sensevoice | qwen3-asr | zipformer (默认: qwen3-asr)\n"
                << "  --asr-endpoint <url>      qwen3-asr llama-server endpoint\n"
                << "  --asr-model <name>        qwen3-asr model tag (默认: qwen3-asr)\n"
                << "  --asr-timeout <sec>       qwen3-asr HTTP超时 (默认: 60)\n"
                << "\n唤醒:\n"
                << "  --wake-enabled, --wake    开启唤醒打断\n"
                << "  --no-wake                 关闭唤醒打断\n"
                << "  --wake-source <kws|hid>   唤醒来源: 本机KWS模型 | SPV板端HID上报 (默认: hid)\n"
                << "  --wake-device <path>      hid: hidraw 设备 (默认: /dev/hidraw0)\n"
                << "  --kws-model-dir <dir>     kws: 模型目录 (默认: ~/.cache/models/kws/xiaojin-v1)\n"
                << "  --kws-threshold <f>       kws: 得分阈值 (默认: 0.3)\n"
                << "  --kws-holdoff-ms <n>      kws: 两次唤醒的最小间隔 (默认: 1000)\n"
                << "  --kws-partial-threshold <f> kws: 快读只解出半个唤醒词时的接受阈值, 0 关闭 (默认: 0)\n"
                << "  --kws-echo-null           kws: 用 3 路裸麦在线学习扬声器零陷后再送 KWS (仅 16 kHz, ASR 不变)\n"
                << "  --kws-echo-null-first-channel <n> kws: 零陷用的第一路裸麦, 1 起, 连续 3 路 (默认: 2)\n"
                << "  --wake-interrupt-mode     唤醒只中断TTS并播放提示音\n"
                << "  --no-wake-interrupt-mode  使用旧的唤醒后ASR插话模式\n"
                << "  --wake-ack-audio <wav>    唤醒提示音\n"
                << "  --wake-strip-asr          从ASR句首清除唤醒词\n"
                << "  --no-wake-strip-asr       不清除ASR中的唤醒词\n"
                << "  --wake-phrase <text>      可重复指定唤醒词或ASR别名; 指定后替换内置词表(含别名)\n"
                << "  --wake-command-timeout-ms <n> 纯唤醒后等待命令的窗口 (默认: 5000)\n"
                << "\nTTS:\n"
                << "  --tts <engine>            TTS后端 (默认: matcha:zh-en)\n"
                << "                            matcha:zh / matcha:en / matcha:zh-en\n"
                << "                            kokoro / kokoro:<voice>\n"
                << "  --list-voices             列出 Kokoro 可用音色\n"
                << "  --startup-greeting <text> 启动完成后播放的问候语\n"
                << "\nAEC:\n"
                << "  --no-aec                  禁用回声消除\n"
                << "  --no-ns                   禁用噪声抑制\n"
                << "  --ns-level <level>        噪声抑制等级 low|moderate|high|veryhigh (默认: moderate)\n"
                << "  --fixed-gain <db>         NS 之后的固定补偿增益, 限幅器保护 (默认: 0)\n"
                << "  --agc                     启用 AGC1 固定数字压缩 (默认禁用)\n"
                << "  --aec-delay <ms>          AEC延迟补偿 (默认: 50ms; 预热开启时从 150ms 起自动跟踪, 仅 AEC_WARMUP_SECONDS=0 时生效)\n"
                << "  --aec-reference-channel <n> AEC参考取第 n 路采集(板端硬件回采, 1 起); 0 = 软件回采 (默认: 0)\n"
                << "  --aec-reference-gain <db> 硬件回采送入 AEC 前的增益 (默认: 12)\n"
                << "  --wake-echo-guard <auto|on|off> 唤醒后丢弃提示音回声期间的音频; auto = 仅软件回采时丢弃 (默认: auto)\n"
                << "  --buffer-frames <n>       音频缓冲帧数 (默认: 480)\n"
                << "  --sample-rate <hz>        音频采样率 (默认: 48000, 常用: 44100, 48000)\n"
                << "  --capture-channels <n>    录音声道数 (默认: 1)\n"
                << "  --playback-channels <n>   播放声道数 (默认: 1)\n"
                << "  --speech-channel <n>      送入AEC/VAD/ASR的录音声道 (默认: 1)\n"
#ifdef USE_DOA
                << "\nDOA:\n"
                << "  --doa                     开启三麦0-360度定位\n"
                << "  --no-doa                  关闭定位\n"
                << "  --doa-pick <a,b,c>        1-based定位声道映射 (4ch默认: 2,3,4)\n"
                << "  --doa-side <m>            等边三角形边长 (默认: 0.063)\n"
                << "  --doa-positions <spec>    麦克风坐标: x,y[,z];x,y[,z];x,y[,z]\n"
                << "  --doa-azimuth-offset <d>  阵列到机器人坐标角度偏移\n"
                << "  --doa-min-signal-rms <rms>  低能量帧过滤阈值 (默认: 0.003)\n"
#endif
                << "\n调试:\n"
                << "  --save-audio [file]       保存AEC处理后的音频 (默认: aec_debug.wav)\n"
                << "  --save-raw-audio [file]   保存处理前的麦克风原始音频 (默认: aec_raw_debug.wav)\n"
                << "  --save-asr-audio [file]   保存ASR增益后音频 (默认: aec_asr_debug.wav)\n"
                << "  --save-tts-audio [file]   保存TTS输出 (默认: tts_debug.wav)\n"
                << "  --aec-dump-dir <dir>      边运行边写逐帧对齐的 aec_dump_{raw,in,ref,out,echo_only,null}.wav\n"
                << "                            (全部采集通道/麦克风原始/扬声器参考/ASR 输入/KWS 输入/零陷输出，目录须已存在)\n"
                << "\nMCP:\n"
                << "  --mcp-config <path>       MCP配置文件 (启用工具调用)\n"
                << "\n声纹识别 (Voiceprint):\n"
                << "  -vp, --voiceprint         开启声纹识别\n"
                << "  --vp-database <file>      声纹数据库文件 (开启VP时必填)\n"
                << "  --vp-threads <n>          推理线程数 (默认: 1)\n"
                << "  --vp-threshold <0-1>      相似度阈值 (默认: 0.6)\n"
                << "  --vp-top <n>              显示前N个匹配 (默认: 3)\n"
                << "  --vp-verify <name>        验证特定说话人\n"
                << "  --vp-list                 列出所有已注册说话人\n"
                << "  --vp-verbose              显示所有匹配分数\n"
                << "\n其他:\n"
                << "  -h, --help                显示帮助\n";
            exit(0);
        }
    }
#ifdef USE_DOA
    if (cfg.doa.enabled && !cfg.capture_channels_set) {
        cfg.capture_channels = 4;
    }
    cfg.doa.sample_rate = cfg.sample_rate;
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

// ============================================================================
// 识别 worker：把声纹 + ASR 从 AEC 处理线程里挪出来
// ============================================================================
// 一段语音在 VAD 判定结束时被打包成 RecognitionJob 交给这里；处理线程立刻回去做下一帧
// 的 APM，不再被 0.3 s 的声纹和 0.2–0.7 s 的 ASR 卡住（之前每段语音都让 APM 停摆
// 0.5–1 s，采集队列涨满后声卡回调只能丢帧，而扬声器参考没停，AEC 就错位了）。
// 单线程 FIFO：识别结果的顺序就是说话的顺序。队列有界：旁人连续说话、识别追不上时
// 丢最早的一段并打日志，不无限积压。

struct RecognitionJob {
    std::vector<float> audio;          // 16 kHz，APM 之后的整段语音
    uint64_t wake_id = 0;              // 段开始时所属的唤醒会话（0 = 自由收音）
    uint64_t generation = 0;           // 段结束时的对话代数；唤醒打断后旧段作废
    bool contains_wake_event = false;
    int post_wake_speech_frames = 0;
};

class RecognitionWorker {
public:
    using Handler = std::function<void(RecognitionJob)>;
    static constexpr size_t kMaxPending = 8;

    RecognitionWorker() = default;
    ~RecognitionWorker() {
        Stop();
    }

    RecognitionWorker(const RecognitionWorker&) = delete;
    RecognitionWorker& operator=(const RecognitionWorker&) = delete;

    void Start(Handler handler) {
        handler_ = std::move(handler);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = true;
        }
        worker_ = std::thread([this]() { Loop(); });
    }

    void Submit(RecognitionJob job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return;
            if (pending_.size() >= kMaxPending) {
                std::cout << "\n" << getTimestamp() << " [ASR] 识别队列积压 ("
                    << pending_.size() << " 段)，丢弃最早的一段 "
                    << formatFloat(pending_.front().audio.size() / 16000.0f, 1)
                    << "s\n" << std::flush;
                pending_.pop_front();
            }
            pending_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

    // 退出时还没识别的段直接丢弃；join 之后 handler 不会再被调用。
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return;
            running_ = false;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void Loop() {
        while (true) {
            RecognitionJob job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return !running_ || !pending_.empty(); });
                if (!running_) return;
                job = std::move(pending_.front());
                pending_.pop_front();
            }
            handler_(std::move(job));
        }
    }

    Handler handler_;
    std::deque<RecognitionJob> pending_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = false;
    std::thread worker_;
};

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
    auto input_devices = SpacemitAudio::AudioDuplex::ListInputDevices();
    if (input_devices.empty()) {
        std::cout << getTimestamp() << "   (无可用设备)\n";
    } else {
        for (const auto& dev : input_devices) {
            std::cout << getTimestamp() << "   [" << dev.first << "] " << dev.second << "\n";
        }
    }

    std::cout << getTimestamp() << " \n输出设备 (扬声器):\n";
    auto output_devices = SpacemitAudio::AudioDuplex::ListOutputDevices();
    if (output_devices.empty()) {
        std::cout << getTimestamp() << "   (无可用设备)\n";
    } else {
        for (const auto& dev : output_devices) {
            std::cout << getTimestamp() << "   [" << dev.first << "] " << dev.second << "\n";
        }
    }

    std::cout << getTimestamp() << " \n使用方法:\n";
    std::cout << getTimestamp() << "   voice_chat_aec -i <输入设备ID> -o <输出设备ID>\n";
    std::cout << getTimestamp() << " ========================================\n";
}

// ============================================================================
// 重采样工具
// ============================================================================

std::vector<float> resampleToVad(const float* data, size_t frames, int from_rate) {
    if (from_rate == 16000) {
        return std::vector<float>(data, data + frames);
    }

    const double ratio = static_cast<double>(from_rate) / 16000.0;
    size_t output_frames = static_cast<size_t>(frames / ratio);
    std::vector<float> output(output_frames);

    for (size_t i = 0; i < output_frames; ++i) {
        double src_pos = i * ratio;
        size_t idx = static_cast<size_t>(src_pos);
        double frac = src_pos - idx;

        if (idx + 1 < frames) {
            output[i] = static_cast<float>(
                data[idx] * (1.0 - frac) + data[idx + 1] * frac);
        } else if (idx < frames) {
            output[i] = data[idx];
        } else {
            output[i] = 0.0f;
        }
    }

    return output;
}

std::vector<float> resampleToAec(const std::vector<float>& input, int from_rate, int to_rate) {
    if (from_rate == to_rate) return input;

    double ratio = static_cast<double>(to_rate) / from_rate;
    size_t output_size = static_cast<size_t>(input.size() * ratio);
    std::vector<float> output(output_size);

    for (size_t i = 0; i < output_size; ++i) {
        double src_pos = i / ratio;
        size_t src_idx = static_cast<size_t>(src_pos);
        double frac = src_pos - src_idx;

        if (src_idx + 1 < input.size()) {
            output[i] = static_cast<float>(
                input[src_idx] * (1.0 - frac) + input[src_idx + 1] * frac);
        } else if (src_idx < input.size()) {
            output[i] = input[src_idx];
        }
    }

    return output;
}

// ============================================================================
// 主程序
// ============================================================================

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    Config cfg = parseArgs(argc, argv);
    cfg.wake_ack_audio = expandUserPath(cfg.wake_ack_audio);

    if (cfg.wake_command_timeout_ms <= 0) {
        std::cerr << "错误: --wake-command-timeout-ms 必须大于0\n";
        return 1;
    }
    if (cfg.wake_enabled && cfg.wake_strip_asr && cfg.wake_phrases.empty()) {
        std::cerr << "错误: 开启唤醒词清洗时必须至少指定一个 --wake-phrase\n";
        return 1;
    }
    if (cfg.wake_source != "kws" && cfg.wake_source != "hid") {
        std::cerr << "错误: --wake-source 只能是 kws 或 hid\n";
        return 1;
    }
    const bool use_kws_wake = cfg.wake_enabled && cfg.wake_source == "kws";
#ifndef USE_KWS
    if (use_kws_wake) {
        std::cerr << "错误: 本程序编译时未开启 USE_KWS，请用 -DUSE_KWS=ON 重新编译，"
            << "或改用 --wake-source hid\n";
        return 1;
    }
#endif
    if (!(cfg.kws_threshold > 0.0f && cfg.kws_threshold <= 1.0f)) {
        std::cerr << "错误: --kws-threshold 必须在 (0, 1] 内\n";
        return 1;
    }
    if (!(cfg.kws_partial_threshold >= 0.0f && cfg.kws_partial_threshold <= 1.0f)) {
        std::cerr << "错误: --kws-partial-threshold 必须在 [0, 1] 内\n";
        return 1;
    }
    if (cfg.kws_holdoff_ms < 0) {
        std::cerr << "错误: --kws-holdoff-ms 不能为负\n";
        return 1;
    }
    if (cfg.kws_echo_null && (cfg.kws_echo_null_first_channel < 1 ||
            cfg.kws_echo_null_first_channel + 2 > cfg.capture_channels)) {
        std::cerr << "错误: --kws-echo-null-first-channel 需要 ch" << cfg.kws_echo_null_first_channel
            << "-ch" << (cfg.kws_echo_null_first_channel + 2) << " 都在采集通道内 (共 "
            << cfg.capture_channels << " 路)\n";
        return 1;
    }
    if (cfg.aec_reference_channel < 0 || cfg.aec_reference_channel > cfg.capture_channels ||
            (cfg.aec_reference_channel > 0 && cfg.aec_reference_channel == cfg.speech_channel)) {
        std::cerr << "错误: --aec-reference-channel 只能是 0 (软件回采) 或 ch1-ch" << cfg.capture_channels
            << " 中 speech channel (ch" << cfg.speech_channel << ") 以外的一路\n";
        return 1;
    }
    if (use_kws_wake && cfg.kws_echo_null) {
        const int first = cfg.kws_echo_null_first_channel;
        // Between playbacks the null passes its first mic through, so that mic is what KWS hears.
        if (cfg.speech_channel != first) {
            std::cerr << "错误: 开启 --kws-echo-null 时 --speech-channel (ch" << cfg.speech_channel
                << ") 必须等于 --kws-echo-null-first-channel (ch" << first
                << ")：零陷在不播放时透传的是它的第一路麦\n";
            return 1;
        }
        if (cfg.aec_reference_channel >= first && cfg.aec_reference_channel <= first + 2) {
            std::cerr << "错误: --aec-reference-channel ch" << cfg.aec_reference_channel
                << " 是回采，不能落在零陷的 ch" << first << "-ch" << (first + 2) << " 里\n";
            return 1;
        }
        if (cfg.sample_rate != 16000) {
            std::cerr << "错误: 开启 --kws-echo-null 时 --sample-rate 必须是 16000 (当前 "
                << cfg.sample_rate << ")\n";
            return 1;
        }
    }
    if (cfg.wake_echo_guard != "auto" && cfg.wake_echo_guard != "on" && cfg.wake_echo_guard != "off") {
        std::cerr << "错误: --wake-echo-guard 只能是 auto、on 或 off\n";
        return 1;
    }
    // With the hardware loopback the AEC removes the ack and the cut TTS tail well enough that
    // VAD stays quiet (live 2026-09-30: 0 of 17 acks crossed the VAD threshold, max 0.51 vs 0.8),
    // so dropping that second only costs the start of "小进小进，往前走一米" said in one breath.
    // The software loopback (6.5 dB) still needs the guard.
    const bool wake_echo_guard = cfg.wake_echo_guard == "on" ||
        (cfg.wake_echo_guard == "auto" && cfg.aec_reference_channel == 0);
    const char* wake_label = cfg.wake_source == "kws" ? "KWS唤醒" : "HID唤醒";

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
    std::cout << getTimestamp() << "    带 AEC 的语音对话系统 (全双工模式)\n";
    std::cout << getTimestamp() << " ========================================\n";
    std::cout << getTimestamp() << " TTS后端: " << cfg.tts_type << "\n";
    std::cout << getTimestamp() << " LLM模型: " << cfg.llm_model << "\n";
    std::cout << getTimestamp() << " LLM URL: " << cfg.llm_url << "\n";
    std::cout << getTimestamp() << " ASR后端: " << cfg.asr_engine;
    if (cfg.asr_engine == "qwen3-asr") {
        std::cout << " (" << cfg.asr_endpoint << ", model=" << cfg.asr_model << ")";
    }
    std::cout << "\n";
    std::cout << getTimestamp() << " 唤醒: ";
    if (!cfg.wake_enabled) {
        std::cout << "OFF\n";
    } else if (use_kws_wake) {
        std::cout << "ON (kws model="
            << (cfg.kws_model_dir.empty() ? "default" : cfg.kws_model_dir)
            << " threshold=" << cfg.kws_threshold
            << " holdoff_ms=" << cfg.kws_holdoff_ms
            << " partial_threshold=" << cfg.kws_partial_threshold
            << " echo_null=" << (cfg.kws_echo_null
                ? "ch" + std::to_string(cfg.kws_echo_null_first_channel) + "-ch" +
                    std::to_string(cfg.kws_echo_null_first_channel + 2)
                : std::string("off")) << ")\n";
    } else {
        std::cout << "ON (hid " << cfg.wake_device << ")\n";
    }
    if (cfg.wake_enabled) {
        std::cout << getTimestamp() << " 唤醒打断模式: "
            << (cfg.wake_interrupt_mode ? "ON" : "OFF")
            << " ack=" << cfg.wake_ack_audio
            << " strip_asr=" << (cfg.wake_strip_asr ? "on" : "off")
            << " command_timeout_ms=" << cfg.wake_command_timeout_ms
            << " phrases=" << cfg.wake_phrases.size()
            << " echo_guard=" << (wake_echo_guard ? "on" : "off") << "\n";
    }
    std::cout << getTimestamp() << " AEC: " << (cfg.aec_enabled ? "ON" : "OFF") << "\n";
    std::cout << getTimestamp() << " AEC延迟补偿: " << cfg.aec_delay_ms << " ms\n";
    std::cout << getTimestamp() << " AEC参考: ";
    if (cfg.aec_reference_channel > 0) {
        std::cout << "硬件回采 ch" << cfg.aec_reference_channel << " 增益 " << cfg.aec_reference_gain_db << " dB\n";
    } else {
        std::cout << "软件回采\n";
    }
    static const char* kNsNames[] = {"low", "moderate", "high", "veryhigh"};
    std::cout << getTimestamp() << " 噪声抑制: "
        << (cfg.ns_enabled ? kNsNames[std::clamp(cfg.ns_level, 0, 3)] : "OFF") << "\n";
    std::cout << getTimestamp() << " AGC: " << (cfg.agc_enabled ? "ON" : "OFF")
        << " 固定增益: " << cfg.fixed_gain_db << " dB\n";
    std::cout << getTimestamp() << " 采样率: " << cfg.sample_rate << " Hz (AEC) -> 16000 Hz (VAD/ASR)\n";
    std::cout << getTimestamp() << " 声道: capture=" << cfg.capture_channels
        << "ch playback=" << cfg.playback_channels
        << "ch speech=ch" << cfg.speech_channel << "\n";
    std::cout << getTimestamp() << " 按 Ctrl+C 退出\n";
    std::cout << getTimestamp() << " ========================================\n\n";

    // -------------------------------------------------------------------------
    // 1-4. 初始化引擎
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

    // TTS models take ~9 s to load. Load them in the background so the duplex audio
    // stream can start now and play an AEC warm-up sweep meanwhile: the first playback
    // after stream start goes through the USB device's delay ramp, and this lets the
    // echo canceller converge on the settled path before the user's first interaction.
    decltype(initTTS(cfg.tts_type)) tts_result;
    // Joined on every exit path: an early `return` below while the loader still runs
    // would destroy a joinable std::thread and call std::terminate.
    struct JoinedThread {
        std::thread thread;
        ~JoinedThread() { if (thread.joinable()) thread.join(); }
    } tts_loader{std::thread([&]() { tts_result = initTTS(cfg.tts_type); })};
    decltype(tts_result.tts) tts;   // assigned after the loader is joined (before the greeting)
    int tts_sample_rate = 0;

    AudioClip wake_ack_clip;
    if (cfg.wake_enabled && cfg.wake_interrupt_mode && !cfg.wake_ack_audio.empty()) {
        std::string error;
        if (!loadWavMonoFloat(cfg.wake_ack_audio, &wake_ack_clip, &error)) {
            std::cerr << getTimestamp() << " 错误: 无法加载唤醒提示音 "
                << cfg.wake_ack_audio << ": " << error << "\n";
            return 1;
        }
        {
            // The stock ack clip peaks at -0.3 dBFS (~2 dB hotter than TTS); at high speaker
            // volume its echo clips the mic and clipped echo cannot be cancelled linearly.
            // Env WAKE_ACK_GAIN_DB attenuates it before playback (default 0 dB: stock level)
            // so the reference and the speaker see the same, non-clipping level.
            const char* g = std::getenv("WAKE_ACK_GAIN_DB");
            const float ack_gain_db = (g && *g) ? static_cast<float>(std::atof(g)) : 0.0f;
            const float k = std::pow(10.0f, ack_gain_db / 20.0f);
            for (auto& s : wake_ack_clip.samples) s *= k;
            std::cout << getTimestamp() << " 唤醒提示音增益: " << ack_gain_db << " dB\n";
        }
        // The stock clip carries ~0.6 s of digital silence after "我在". Playing it only
        // stretches the echo guard, during which the user's speech is discarded; trim it
        // and let the guard cover the real echo (clip + loop delay) instead.
        const float clip_seconds = wake_ack_clip.samples.size() /
            static_cast<float>(wake_ack_clip.sample_rate);
        const size_t trimmed = trimTrailingSilence(&wake_ack_clip, -45.0f, 40);
        std::cout << getTimestamp() << " 唤醒提示音: " << cfg.wake_ack_audio
            << " (" << wake_ack_clip.sample_rate << " Hz, " << formatFloat(clip_seconds, 2)
            << "s, 裁去尾部静音 " << formatFloat(trimmed /
                static_cast<float>(wake_ack_clip.sample_rate), 2) << "s)\n";
    }

    // -------------------------------------------------------------------------
    // 5. 初始化 AEC 处理器
    // -------------------------------------------------------------------------
    std::cout << getTimestamp() << " [5/5] 初始化 AEC 音频处理器..." << std::flush;

    AecDuplexProcessor::Config aec_cfg;
    aec_cfg.sample_rate = cfg.sample_rate;
    aec_cfg.channels = 1;
    aec_cfg.capture_channels = cfg.capture_channels;
    aec_cfg.playback_channels = cfg.playback_channels;
    aec_cfg.speech_channel = cfg.speech_channel;
    aec_cfg.frames_per_buffer = cfg.buffer_frames > 0
        ? cfg.buffer_frames
        : std::max(1, cfg.sample_rate / 100);
    aec_cfg.input_device = cfg.input_device;
    aec_cfg.output_device = cfg.output_device;
    aec_cfg.aec_enabled = cfg.aec_enabled;
    aec_cfg.ns_enabled = cfg.ns_enabled;
    aec_cfg.ns_level = cfg.ns_level;
    aec_cfg.agc_enabled = cfg.agc_enabled;
    aec_cfg.fixed_gain_db = cfg.fixed_gain_db;
    aec_cfg.estimated_delay_ms = cfg.aec_delay_ms;
    aec_cfg.reference_channel = cfg.aec_reference_channel;
    aec_cfg.reference_gain_db = cfg.aec_reference_gain_db;
    aec_cfg.dump_dir = cfg.aec_dump_dir;
    aec_cfg.echo_null = use_kws_wake && cfg.kws_echo_null;
    aec_cfg.echo_null_first_channel = cfg.kws_echo_null_first_channel;

    AecDuplexProcessor aec_processor(aec_cfg);
    if (!aec_processor.initialize()) {
        std::cerr << "\n" << getTimestamp() << " 错误: AEC 初始化失败\n";
        return 1;
    }

#ifdef USE_DOA
    omni_agent::DoaRuntime doa_runtime;
    if (cfg.doa.enabled) {
        if (!doa_runtime.Initialize(cfg.doa, std::cerr)) {
            std::cerr << "\n" << getTimestamp() << " 错误: DOA 初始化失败\n";
            return 1;
        }
        std::cout << "\n" << getTimestamp() << " DOA: ON ("
            << doa_runtime.ChannelMapString()
            << ", sample_rate=" << cfg.sample_rate << ")\n";
    }
#endif
    std::cout << " OK\n\n";

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
    // 状态变量
    // -------------------------------------------------------------------------
    std::vector<float> audio_buffer;
    std::mutex buffer_mutex;
    int silence_frames_count = 0;
    const int silence_frames_threshold =
        static_cast<int>(cfg.silence_duration * 16000 / 512);
    bool is_speaking = false;
    float vad_segment_max_prob = 0.0f;

    const size_t PRE_BUFFER_FRAMES = 20;
    std::deque<std::vector<float>> pre_buffer;
    // Echo guard after a wake/interrupt: the cut TTS keeps echoing for ~one loop delay
    // (~330 ms on the SPV device) and the ack tone echoes for its whole length. While
    // the guard is active VAD frames are dropped (not pre-buffered, not sent to ASR).
    std::atomic<long long> echo_guard_until_ms{0};
    // Set once TTS has finished loading; until then the pipeline must not handle
    // speech segments (the warm-up sweep plays while the models load).
    std::atomic<bool> pipeline_ready{false};
    const long long echo_tail_ms = [] {
        const char* v = std::getenv("AEC_ECHO_TAIL_MS");
        return (v && *v) ? std::atoll(v) : 0LL;  // default 0: the user starts talking the moment the ack tone ends
    }();

    int barge_in_confirm_frames = 0;
    const int BARGE_IN_CONFIRM_THRESHOLD = 3;

    int post_barge_in_cooldown = 0;
    const int COOLDOWN_FRAMES = 15;

    std::atomic<bool> barge_in_recording{false};
    std::atomic<uint64_t> wake_id_counter{0};
    std::atomic<uint64_t> pending_wake_id{0};
    std::atomic<uint64_t> active_wake_session_id{0};
    std::atomic<long long> active_wake_deadline_ms{0};
    std::atomic<uint64_t> dialogue_generation{0};
    uint64_t utterance_wake_id = 0;
    std::atomic<uint64_t> pending_initial_wake_segment_id{0};
    bool utterance_contains_wake_event = false;
    int post_wake_speech_frames = 0;

    std::vector<int16_t> recorded_audio;
    std::mutex record_mutex;
    std::vector<int16_t> raw_recorded_audio;
    std::vector<int16_t> asr_recorded_audio;
    std::vector<int16_t> tts_recorded_audio;
    std::mutex tts_record_mutex;

    const size_t VAD_FRAME_SIZE = 512;
    std::vector<float> vad_frame_buffer;
    std::mutex vad_state_mutex;
    VadProgressPrinter vad_progress;
    vad_progress.Start();
    omni_agent::HidWakeListener wake_listener;
#ifdef USE_KWS
    omni_agent::KwsWakeListener kws_wake_listener;
#endif
    auto stopWakeListeners = [&]() {
        wake_listener.Stop();
#ifdef USE_KWS
        kws_wake_listener.Stop();
#endif
    };

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
    pipeline_ctx.enqueue_playback = [&](const std::vector<float>& samples, int rate) {
        auto audio_aec = resampleToAec(samples, rate, cfg.sample_rate);
        aec_processor.enqueuePlayback(audio_aec, cfg.sample_rate);
    };
    pipeline_ctx.is_playing = [&]() { return aec_processor.isPlaying(); };
    pipeline_ctx.clear_playback = [&]() { aec_processor.clearPlayback(); };
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

    auto playWakeAck = [&]() {
        if (!wake_ack_clip.samples.empty() && wake_ack_clip.sample_rate > 0) {
            pipeline_ctx.enqueue_playback(wake_ack_clip.samples, wake_ack_clip.sample_rate);
        }
    };

    auto expireWakeSession = [&]() {
        uint64_t wake_id = active_wake_session_id.load(std::memory_order_acquire);
        if (wake_id == 0 || monotonicMs() <= active_wake_deadline_ms.load()) {
            return;
        }
        if (active_wake_session_id.compare_exchange_strong(wake_id, 0)) {
            active_wake_deadline_ms.store(0);
            uint64_t pending_id = wake_id;
            pending_initial_wake_segment_id.compare_exchange_strong(
                pending_id, 0, std::memory_order_acq_rel);
            std::cout << getTimestamp() << " [Wake] wake_id=" << wake_id
                << " command timeout\n" << std::flush;
        }
    };

    auto closeWakeSession = [&](uint64_t wake_id) {
        if (wake_id == 0) return;
        uint64_t expected = wake_id;
        if (active_wake_session_id.compare_exchange_strong(expected, 0)) {
            active_wake_deadline_ms.store(0);
            uint64_t pending_id = wake_id;
            pending_initial_wake_segment_id.compare_exchange_strong(
                pending_id, 0, std::memory_order_acq_rel);
        }
    };

    auto consumeWakeRequest = [&]() -> uint64_t {
        uint64_t wake_id = pending_wake_id.exchange(0, std::memory_order_acq_rel);
        if (wake_id == 0) {
            return 0;
        }

        active_wake_session_id.store(wake_id, std::memory_order_release);
        active_wake_deadline_ms.store(
            monotonicMs() + cfg.wake_command_timeout_ms, std::memory_order_release);

        const bool was_processing = g_processing.load();
        const bool was_playing = aec_processor.isPlaying();
        if (was_processing) {
            g_barge_in = true;
        }
        if (was_processing || was_playing) {
            aec_processor.clearPlayback();
        }
        long long guard_ms = 0;
        if (cfg.wake_interrupt_mode) {
            playWakeAck();
            if (wake_echo_guard && !wake_ack_clip.samples.empty() && wake_ack_clip.sample_rate > 0) {
                // The clip is trimmed to its voiced part, so its echo keeps arriving for one
                // loop delay after playback ends; add the tracker margin and some room decay.
                guard_ms += static_cast<long long>(
                    wake_ack_clip.samples.size() * 1000.0 / wake_ack_clip.sample_rate)
                    + std::lround(aec_processor.delayMs()) + 100;
            }
        }
        if (wake_echo_guard && (was_playing || was_processing || cfg.wake_interrupt_mode)) {
            guard_ms += echo_tail_ms;
        }
        if (guard_ms > 0) {
            echo_guard_until_ms.store(monotonicMs() + guard_ms, std::memory_order_release);
        }

        {
            std::lock_guard<std::mutex> lock(buffer_mutex);
            pending_initial_wake_segment_id.store(
                wake_id, std::memory_order_release);
            if (was_processing) {
                // The next segment opens at the user's VAD onset, as when idle. Opening it
                // here records the ack window whenever the user waits for the ack, and ASR
                // turns that silence into words. Opening early would not save a no-pause
                // command either: under the cut TTS and the ack, the full AEC3 output has
                // the user ducked (2026-10-01 live; README known limitation).
                audio_buffer.clear();
                if (was_playing) {
                    // The last 640 ms in pre_buffer are the dog's own TTS echo, not the
                    // user's speech onset: never feed them to ASR.
                    pre_buffer.clear();
                }
                is_speaking = false;
                silence_frames_count = 0;
                vad_segment_max_prob = 0.0f;
                barge_in_recording = false;
                post_barge_in_cooldown = 0;
                utterance_wake_id = 0;
                utterance_contains_wake_event = false;
                post_wake_speech_frames = 0;
            }
            if (is_speaking) {
                utterance_wake_id = wake_id;
                utterance_contains_wake_event = true;
                post_wake_speech_frames = 0;
                pending_initial_wake_segment_id.store(
                    0, std::memory_order_release);
            }
        }
        barge_in_confirm_frames = 0;

        std::cout << "\n" << getTimestamp() << " [Wake] " << wake_label << " wake_id="
            << wake_id << " generation=" << dialogue_generation.load();
        if (was_processing) {
            std::cout << "，已中断当前回复";
        }
        if (cfg.wake_interrupt_mode) {
            std::cout << "，播放提示音";
        }
        if (guard_ms > 0) {
            std::cout << "，回声守护 " << guard_ms << "ms" << (was_playing ? "，丢弃预缓冲" : "");
        } else if (!wake_echo_guard) {
            std::cout << "，不屏蔽唤醒后音频" << (was_playing ? "，丢弃预缓冲" : "");
        }
        std::cout << "\n" << std::flush;
        return wake_id;
    };

    // -------------------------------------------------------------------------
    // 设置 AEC 处理器回调
    // -------------------------------------------------------------------------
    // 原始音频录制需要这个回调，DOA 也需要，任一开启就注册
    bool want_raw_callback = cfg.save_raw_audio;
#ifdef USE_DOA
    want_raw_callback = want_raw_callback || doa_runtime.enabled();
#endif
    if (want_raw_callback) {
        aec_processor.setRawAudioCallback(
            [&](const float* data, size_t frames, int channels, int /*sample_rate*/) {
                if (cfg.save_raw_audio) {
                    // 存原始的那一路麦克风，用来对照 AEC/NS 到底改变了多少
                    const int idx = std::clamp(cfg.speech_channel - 1, 0,
                        std::max(0, channels - 1));
                    std::lock_guard<std::mutex> lock(record_mutex);
                    for (size_t i = 0; i < frames; ++i) {
                        const float s =
                            data[i * static_cast<size_t>(channels) + idx];
                        raw_recorded_audio.push_back(static_cast<int16_t>(
                            std::clamp(s, -1.0f, 1.0f) * 32767.0f));
                    }
                }
#ifdef USE_DOA
                if (!g_running) return;
                if (doa_runtime.enabled()) {
                    doa_runtime.ProcessInterleaved(data, frames, channels);
                }
#endif
            });
    }
    // -------------------------------------------------------------------------
    // 识别线程：声纹 + ASR + 唤醒词过滤
    // -------------------------------------------------------------------------
    // 处理线程（APM + VAD）在段结束时只做一件事：把 audio_buffer 和会话快照打包成
    // RecognitionJob 入队。下面这段就是原来同步跑在处理线程里的识别逻辑，逐行搬过来，
    // 日志格式不变；唯一新增的是入口处的代数检查——被唤醒打断后的旧段以前要等 ASR 跑完
    // 才丢，现在直接跳过，省掉 0.5 s 的白算。
    auto runRecognitionJob = [&](RecognitionJob job) {
        if (job.generation != dialogue_generation.load(std::memory_order_acquire)) {
            std::cout << "\n" << getTimestamp() << " [Wake] 丢弃过期语音段 generation="
                << job.generation << "（未识别）\n" << std::flush;
            return;
        }
#ifdef USE_VP
        std::string speaker_tag;
        bool vp_passed = true;
        if (vp_engine) {
            std::shared_ptr<SpacemiT::VpResult> vp_res;
            // 段长跟着分数一起打，方便对照"这句是谁说的"；前置换行是因为
            // VAD 进度行用 \r 原地刷新，不换行的话分数会被下一次刷新盖掉。
            const std::string vp_len = " len=" +
                formatFloat(job.audio.size() / 16000.0f, 1) + "s";
            if (!cfg.vp_verify.empty()) {
                vp_res = vp_engine->Verify(cfg.vp_verify, job.audio, 16000);
                if (vp_res && vp_res->IsSuccess()) {
                    std::cout << "\n" << getTimestamp() << " [VP] 验证 \""
                        << cfg.vp_verify << "\": "
                        << (vp_res->IsVerified() ? "通过" : "不通过")
                        << " (score: " << formatVpScore(vp_res->GetScore())
                        << ", threshold: " << formatVpScore(vp_engine->GetThreshold())
                        << ")" << vp_len << std::endl;
                    if (vp_res->IsVerified()) {
                        speaker_tag = "[" + cfg.vp_verify + "] ";
                    } else {
                        vp_passed = false;
                    }
                } else {
                    vp_passed = false;
                }
            } else {
                vp_res = vp_engine->Identify(job.audio, 16000);
                if (vp_res && vp_res->IsSuccess()) {
                    if (vp_res->IsIdentified()) {
                        std::cout << "\n" << getTimestamp() << " [VP] 说话人: "
                            << vp_res->GetName()
                            << " (score: " << formatVpScore(vp_res->GetScore())
                            << ", threshold: " << formatVpScore(vp_engine->GetThreshold())
                            << ")" << vp_len << std::endl;
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
                std::cout << "\n" << getTimestamp() << " [VP] 未识别说话人，丢弃音频";
                if (!cfg.vp_verify.empty()) {
                    if (vp_res && vp_res->IsSuccess()) {
                        std::cout << " (score: " << formatVpScore(vp_res->GetScore())
                            << ", threshold: " << formatVpScore(vp_engine->GetThreshold())
                            << ")";
                    } else {
                        std::cout << " (threshold: "
                            << formatVpScore(vp_engine->GetThreshold()) << ")";
                    }
                } else {
                    auto matches = vp_res ? vp_res->GetMatches()
                        : std::vector<SpacemiT::SpeakerMatch>{};
                    if (!matches.empty()) {
                        std::cout << " (best: " << matches[0].name
                            << ", score: " << formatVpScore(matches[0].score)
                            << ", threshold: " << formatVpScore(vp_engine->GetThreshold())
                            << ")";
                    } else {
                        std::cout << " (threshold: "
                            << formatVpScore(vp_engine->GetThreshold()) << ")";
                    }
                }
                std::cout << vp_len << std::endl;
            }
        }
        if (vp_passed) {
#endif
        AsrAudioPreprocessStats asr_audio_stats =
            preprocessAsrAudio(&job.audio);
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
            std::vector<int16_t> asr_pcm = floatToPcm16(job.audio);
            asr_recorded_audio.insert(
                asr_recorded_audio.end(), asr_pcm.begin(), asr_pcm.end());
        }

        std::cout << getTimestamp() << " [ASR] 开始识别..." << std::endl;
        auto result = asr->Recognize(job.audio, 16000);
        if (job.generation !=
                dialogue_generation.load(std::memory_order_acquire)) {
            std::cout << getTimestamp()
                << " [Wake] 丢弃过期ASR结果 generation="
                << job.generation << "\n";
        } else if (result && !result->IsEmpty()) {
            std::string text = result->GetText();
            std::cout << getTimestamp() << " [ASR] 识别完成: \""
                << text << "\"" << std::endl;
            bool drop_wake_text = text.empty();
            if (text.empty()) {
                std::cout << getTimestamp()
                    << " [ASR] 规范化后为空，不送入LLM\n";
            }
            if (!drop_wake_text && cfg.wake_enabled &&
                    cfg.wake_strip_asr) {
                auto filtered = omni_agent::filterWakeAsrText(
                    text, cfg.wake_phrases);
                if (filtered.drop) {
                    std::cout << getTimestamp()
                        << " [Wake] wake_id=" << job.wake_id
                        << " 纯唤醒，丢弃ASR: \"" << text
                        << "\" matched=\"" << filtered.matched_phrase
                        << "\"\n";
                    drop_wake_text = true;
                } else if (filtered.changed) {
                    std::cout << getTimestamp()
                        << " [Wake] wake_id=" << job.wake_id
                        << " 清洗ASR: \"" << text
                        << "\" -> \"" << filtered.text
                        << "\" matched=\"" << filtered.matched_phrase
                        << "\"\n";
                    text = filtered.text;
                } else if (job.wake_id != 0 &&
                        omni_agent::shouldDropUnmatchedWakeAsr(
                        job.contains_wake_event,
                        job.post_wake_speech_frames)) {
                    std::cout << getTimestamp()
                        << " [Wake] wake_id=" << job.wake_id
                        << " 首段ASR未命中词表，且唤醒后无连续近端语音，"
                        << "按纯唤醒误转写丢弃: \"" << text << "\"\n";
                    drop_wake_text = true;
                } else if (job.wake_id != 0) {
                    std::cout << getTimestamp() << " [Wake] wake_id="
                        << job.wake_id
                        << " ASR未包含唤醒词，按命令原样通过\n";
                }
            }
            if (!drop_wake_text) {
                closeWakeSession(job.wake_id);
                std::lock_guard<std::mutex> lock2(g_process_thread_mutex);
                if (g_process_thread && g_process_thread->joinable()) {
                    g_process_thread->join();
                }
#ifdef USE_VP
                std::string final_text = speaker_tag + text;
#else
                const std::string& final_text = text;
#endif
                g_process_thread = std::make_unique<std::thread>([&pipeline_ctx, final_text]() {
                    processText(pipeline_ctx, final_text);
                });
            } else {
                if (job.wake_id != 0 &&
                        active_wake_session_id.load() == job.wake_id) {
                    active_wake_deadline_ms.store(
                        monotonicMs() + cfg.wake_command_timeout_ms);
                    std::cout << getTimestamp()
                        << " [Wake] 等待后续命令 wake_id="
                        << job.wake_id << "\n" << std::flush;
                } else {
                    std::cout << getTimestamp()
                        << " [等待语音输入...]\n" << std::flush;
                }
            }
        } else {
            std::cout << getTimestamp()
                << " [ASR] 识别完成: (无结果)" << std::endl;
            if (job.wake_id != 0 &&
                    active_wake_session_id.load() == job.wake_id) {
                active_wake_deadline_ms.store(
                    monotonicMs() + cfg.wake_command_timeout_ms);
                std::cout << getTimestamp()
                    << " [Wake] 等待后续命令 wake_id="
                    << job.wake_id << "\n" << std::flush;
            }
        }
#ifdef USE_VP
        }
#endif
    };
    RecognitionWorker recognizer;
    recognizer.Start(runRecognitionJob);
#ifdef USE_KWS
    if (use_kws_wake) {
        // KWS gets its own AEC-only stream: the model was trained on audio without NS/AGC.
        aec_processor.setEchoOnlyAudioCallback([&](const float* data, size_t frames, int) {
            auto samples_16k = resampleToVad(data, frames, cfg.sample_rate);
            kws_wake_listener.Feed(samples_16k.data(), samples_16k.size());
        });
    }
#endif
    aec_processor.setAudioCallback([&](const float* data, size_t frames, int /*sample_rate*/) {
        if (!g_running) return;

        auto samples_16k = resampleToVad(data, frames, cfg.sample_rate);
        if (samples_16k.empty()) return;

        expireWakeSession();
        const uint64_t consumed_wake_id = consumeWakeRequest();
#ifdef USE_DOA
        if (consumed_wake_id != 0 && doa_runtime.enabled()) {
            doa_runtime.Reset();
        }
#else
        (void)consumed_wake_id;
#endif

        if (cfg.save_audio) {
            std::lock_guard<std::mutex> lock(record_mutex);
            for (float s : samples_16k) {
                recorded_audio.push_back(static_cast<int16_t>(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
            }
        }

        {
            std::lock_guard<std::mutex> lock(vad_state_mutex);
            vad_frame_buffer.insert(vad_frame_buffer.end(),
                samples_16k.begin(), samples_16k.end());
        }

        while (g_running) {
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
            if (monotonicMs() < echo_guard_until_ms.load(std::memory_order_acquire)) {
                // Only our own echo (cut TTS tail / ack tone) is in the mic. A segment that was
                // already open stays open: ASR gets the wake word and the command without the
                // ack between them, and the wake filter strips the wake word.
                continue;
            }
            if (!pipeline_ready.load(std::memory_order_acquire)) {
                continue;  // models still loading: keep adapting the AEC, no speech handling yet
            }

            if (utterance_contains_wake_event &&
                    vad_prob > cfg.vad_threshold) {
                ++post_wake_speech_frames;
            }

            if (g_processing) {
                if (barge_in_recording && is_speaking) {
                    std::lock_guard<std::mutex> lock(buffer_mutex);
                    audio_buffer.insert(audio_buffer.end(), vad_frame.begin(), vad_frame.end());
                    vad_segment_max_prob = std::max(vad_segment_max_prob, vad_prob);
                    vad_progress.Publish(vad_prob, cfg.vad_threshold, audio_buffer.size(), true);

                    if (vad_prob <= cfg.vad_threshold) {
                        if (post_barge_in_cooldown > 0) {
                            post_barge_in_cooldown--;
                        } else {
                            silence_frames_count++;
                        }
                    } else {
                        silence_frames_count = 0;
                    }
                    continue;
                }

                if (!cfg.wake_enabled && aec_processor.isPlaying() && vad_prob > cfg.vad_threshold) {
                    barge_in_confirm_frames++;
                    pre_buffer.push_back(vad_frame);
                    if (pre_buffer.size() > PRE_BUFFER_FRAMES + BARGE_IN_CONFIRM_THRESHOLD) {
                        pre_buffer.pop_front();
                    }

                    if (barge_in_confirm_frames >= BARGE_IN_CONFIRM_THRESHOLD) {
                        std::cout << "\n" << getTimestamp() << " [Barge-in] 用户打断 (连续"
                            << barge_in_confirm_frames << "帧, prob=" << vad_prob
                            << ")，停止播放\n";
                        aec_processor.clearPlayback();
#ifdef USE_DOA
                        if (doa_runtime.enabled()) {
                            doa_runtime.Reset();
                        }
#endif
                        g_barge_in = true;
                        barge_in_recording = true;
                        barge_in_confirm_frames = 0;

                        post_barge_in_cooldown = COOLDOWN_FRAMES;

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
                    appendPreBuffer(PRE_BUFFER_FRAMES);
                    if (utterance_wake_id == 0) {
                        utterance_wake_id =
                            active_wake_session_id.load(std::memory_order_acquire);
                    }
                    if (utterance_wake_id != 0 &&
                            pending_initial_wake_segment_id.load(
                                std::memory_order_acquire) == utterance_wake_id) {
                        utterance_contains_wake_event = true;
                        post_wake_speech_frames = 1;
                        pending_initial_wake_segment_id.store(
                            0, std::memory_order_release);
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

                if (post_barge_in_cooldown > 0) {
                    post_barge_in_cooldown--;
                } else {
                    silence_frames_count++;
                }

                if (silence_frames_count >= silence_frames_threshold) {
                    is_speaking = false;
                    barge_in_recording = false;
                    std::cout << "\r" << getTimestamp()
                        << " [VAD] 停止说话，触发识别";
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
                        RecognitionJob job;
                        job.audio.swap(audio_buffer);
                        job.wake_id = utterance_wake_id;
                        job.generation = dialogue_generation.load(std::memory_order_acquire);
                        job.contains_wake_event = utterance_contains_wake_event;
                        job.post_wake_speech_frames = post_wake_speech_frames;
                        recognizer.Submit(std::move(job));
                    }

                    audio_buffer.clear();
                    utterance_wake_id = 0;
                    utterance_contains_wake_event = false;
                    post_wake_speech_frames = 0;
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
    });

    // -------------------------------------------------------------------------
    // 开始对话
    // -------------------------------------------------------------------------
    if (!aec_processor.start()) {
        std::cerr << getTimestamp() << " 错误: 无法启动音频处理\n";
        return 1;
    }

    long long warm_end_ms = 0;
    {
        const char* ws = std::getenv("AEC_WARMUP_SECONDS");
        const double warm_s = (ws && *ws) ? std::atof(ws) : 12.0;  // the device's loop delay ramps ~205->330 ms over ~12 s after stream start
        const char* wd = std::getenv("AEC_WARMUP_DBFS");
        const double warm_dbfs = (wd && *wd) ? std::atof(wd) : -22.0;
        if (warm_s > 0.0) {
            // Repeated 1.5 s log sweeps 100 Hz -> 4 kHz with 30 ms raised-cosine fades:
            // broadband, deterministic, and it sounds like a soft boot chime.
            const int sr = cfg.sample_rate;
            const size_t sweep_n = static_cast<size_t>(1.5 * sr);
            const size_t total_n = static_cast<size_t>(warm_s * sr);
            const float amp = static_cast<float>(std::pow(10.0, warm_dbfs / 20.0));
            const double f0 = 100.0, f1 = 4000.0, T = 1.5;
            const size_t fade_n = static_cast<size_t>(0.03 * sr);
            std::vector<float> warm;
            // Prefer a voice prompt as the warm-up signal (env AEC_WARMUP_WAV, or the bundled
            // 027_system_loading.wav); it is looped with 300 ms gaps to fill warm_s. Speech is
            // an excellent AEC trainer and sounds intentional. Falls back to the sweep.
            {
                std::string wav_path;
                if (const char* wp = std::getenv("AEC_WARMUP_WAV")) wav_path = wp;
                if (wav_path.empty()) {
                    const char* home = std::getenv("HOME");
                    std::string cand = std::string(home ? home : "") + "/.cache/models/assets/audio/027_system_loading.wav";
                    if (FILE* f = std::fopen(cand.c_str(), "rb")) { std::fclose(f); wav_path = cand; }
                }
                AudioClip clip; std::string err;
                if (!wav_path.empty() && loadWavMonoFloat(wav_path, &clip, &err) && !clip.samples.empty()) {
                    std::vector<float> s = clip.sample_rate == sr ? clip.samples : resampleToAec(clip.samples, clip.sample_rate, sr);
                    const char* wg = std::getenv("AEC_WARMUP_GAIN_DB");
                    const float g = std::pow(10.0f, ((wg && *wg) ? static_cast<float>(std::atof(wg)) : 0.0f) / 20.0f);
                    const size_t gap = static_cast<size_t>(0.3 * sr);
                    const char* wo = std::getenv("AEC_WARMUP_ONCE");
                    const bool once = !(wo && *wo == '0');   // default: play the prompt once
                    if (once) {
                        for (float v : s) warm.push_back(v * g);
                        warm.insert(warm.end(), gap, 0.0f);
                    } else {
                        while (warm.size() < total_n) {
                            for (float v : s) warm.push_back(v * g);
                            warm.insert(warm.end(), gap, 0.0f);
                        }
                        warm.resize(total_n);
                    }
                    std::cout << getTimestamp() << " [AEC] 预热音频: " << wav_path << " (" << (s.size() / static_cast<double>(sr)) << " s, 循环)\n";
                }
            }
            // Sweep fallback (no prompt, or a silent one). Size the buffer to the sweep first:
            // a once-mode prompt buffer is shorter than total_n and was written past its end.
            if (warm.empty() || std::all_of(warm.begin(), warm.end(), [](float v) { return v == 0.0f; })) {
                warm.assign(total_n, 0.0f);
                for (size_t i = 0; i < total_n; ++i) {
                    const size_t k = i % sweep_n;
                    const double t = static_cast<double>(k) / sr;
                    const double phase = 2.0 * M_PI * f0 * T / std::log(f1 / f0) * (std::pow(f1 / f0, t / T) - 1.0);
                    double g = 1.0;
                    if (k < fade_n) g = 0.5 - 0.5 * std::cos(M_PI * k / fade_n);
                    else if (k + fade_n > sweep_n) g = 0.5 - 0.5 * std::cos(M_PI * (sweep_n - k) / fade_n);
                    warm[i] = static_cast<float>(amp * g * std::sin(phase));
                }
            }
            // During the warm-up the reference delay follows the device ramp (coarse tracker,
            // <=10 ms steps every 0.5 s, sweep gives strong correlation); it is frozen at the end.
            const char* ts_env = std::getenv("AEC_TRACK_SECONDS");
            const double track_s = (ts_env && *ts_env) ? std::atof(ts_env) : 15.0;
            aec_processor.setDelayMs(150.0);
            // Tracking stays on for track_s after stream open (device delay ramps ~12 s) and
            // freezes itself; the greeting / first replies are its signal after the prompt.
            aec_processor.setDelayTracking(true, 40.0, track_s);
            aec_processor.enqueuePlayback(warm, sr);
            warm_end_ms = monotonicMs() + static_cast<long long>(1000.0 * warm.size() / sr);
            echo_guard_until_ms.store(warm_end_ms + 500, std::memory_order_release);
            std::cout << getTimestamp() << " [AEC] 预热信号 " << (1000.0 * warm.size() / sr / 1000.0) << " s (扫频备用 @ " << warm_dbfs
                << " dBFS（TTS 加载期间播放，VAD 屏蔽）\n";
        }
    }

    tts_loader.thread.join();
    while (warm_end_ms > 0 && monotonicMs() < warm_end_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let the sweep finish the ramp
    }
    if (warm_end_ms > 0) {
        std::cout << getTimestamp() << " [AEC] 预热音播完，当前参考延迟 " << aec_processor.delayMs()
            << " ms（跟踪继续到开流后 AEC_TRACK_SECONDS 秒后自动冻结）\n";
    }
    if (!tts_result.tts) {
        std::cerr << getTimestamp() << " 错误: TTS 初始化失败\n";
        stopWakeListeners();
        aec_processor.stop();
        return 1;
    }
    tts = tts_result.tts;
    tts_sample_rate = tts_result.sample_rate;
    pipeline_ctx.tts = tts;
    pipeline_ctx.tts_sample_rate = tts_sample_rate;
    pipeline_ready.store(true, std::memory_order_release);
    echo_guard_until_ms.store(0, std::memory_order_release);
    std::cout << getTimestamp() << " [AEC] TTS 就绪，预热结束\n";
    // The wake listener starts only once the pipeline is ready (same timing as the stock
    // SDK, where the audio stream opened last): wake events during model loading are ignored.
    auto requestWake = [&]() {
        const uint64_t wake_id =
            wake_id_counter.fetch_add(1, std::memory_order_acq_rel) + 1;
        dialogue_generation.fetch_add(1, std::memory_order_acq_rel);
        pending_wake_id.store(wake_id, std::memory_order_release);
    };
    if (cfg.wake_enabled && !use_kws_wake) {
        std::string wake_error;
        if (!wake_listener.Start(cfg.wake_device, requestWake, &wake_error)) {
            std::cerr << getTimestamp() << " 错误: 无法启动 HID 唤醒: "
                << wake_error << "\n";
            aec_processor.stop();
            return 1;
        }
        std::cout << getTimestamp() << " HID唤醒监听: " << cfg.wake_device << "\n";
    }
#ifdef USE_KWS
    if (use_kws_wake) {
        omni_agent::KwsWakeListener::Options kws_options;
        kws_options.model_dir = cfg.kws_model_dir;
        kws_options.threshold = cfg.kws_threshold;
        kws_options.holdoff_ms = cfg.kws_holdoff_ms;
        kws_options.partial_threshold = cfg.kws_partial_threshold;
        std::string wake_error;
        if (!kws_wake_listener.Start(kws_options,
                [&](const std::string& keyword, float score) {
                    std::cout << "\n" << getTimestamp() << " [KWS] " << keyword
                        << " score=" << formatFloat(score, 3) << "\n" << std::flush;
                    requestWake();
                }, &wake_error)) {
            std::cerr << getTimestamp() << " 错误: 无法启动 KWS 唤醒: "
                << wake_error << "\n";
            aec_processor.stop();
            return 1;
        }
        std::cout << getTimestamp() << " KWS唤醒监听: AEC 输出 (无 NS/AGC)\n";
    }
#endif
    playStartupGreeting(pipeline_ctx, cfg.startup_greeting);
    std::cout << getTimestamp() << " [等待语音输入...]\n" << std::flush;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    stopWakeListeners();
    recognizer.Stop();
    {
        std::lock_guard<std::mutex> lock(g_process_thread_mutex);
        if (g_process_thread && g_process_thread->joinable()) {
            g_process_thread->join();
        }
    }

    vad_progress.Stop();
    aec_processor.stop();

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
    if (cfg.save_raw_audio && !raw_recorded_audio.empty()) {
        const double raw_rate = static_cast<double>(cfg.sample_rate);
        std::cout << getTimestamp() << " [保存原始音频] " << cfg.raw_audio_file
            << " (" << raw_recorded_audio.size() << " samples, "
            << (raw_recorded_audio.size() / raw_rate) << " 秒)\n";
        saveWav(cfg.raw_audio_file, raw_recorded_audio,
                static_cast<int>(cfg.sample_rate));
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
