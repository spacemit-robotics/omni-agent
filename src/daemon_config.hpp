/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DAEMON_CONFIG_HPP
#define DAEMON_CONFIG_HPP

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "wake_text_filter.hpp"

namespace omni_agent {

struct AudioCfg {
    // 输入设备名 substring 匹配序列，按顺序首个命中即采用。
    std::vector<std::string> input_device_hints = {"SPV Composite", "USB Audio"};
    // 输出设备名 substring 匹配序列，按顺序首个命中即采用。
    std::vector<std::string> output_device_hints = {"SPV Composite", "USB Audio"};
    // -1 表示走 hints 匹配；非负值直接作为 PortAudio 设备 index。
    int input_device_id = -1;
    int output_device_id = -1;
    // 当前 omni-agent USB 音频设备固定为 16kHz。
    int capture_rate = 16000;
    int playback_rate = 16000;
    // 录音/播放声道数。
    int capture_channels = 4;
    int playback_channels = 2;
    // 1-based；多声道采集时送入 AEC/VAD/ASR 的主路。
    int speech_channel = 1;
};

struct AudioFrontendCfg {
    // 是否在 voice_chat 的 ch1 VAD/ASR 输入前启用 WebRTC 单路前端。
    bool enabled = true;
    // 高通滤波，抑制低频风噪/结构噪声。
    bool highpass = true;
    // WebRTC 低档降噪；默认关闭，避免先引入语音失真。
    bool noise_suppression = false;
    // 自动增益控制，提升远场弱语音输入。
    bool agc = true;
    // AGC 目标峰值余量，单位 dBFS，WebRTC 范围 0~31。
    int agc_target_level_dbfs = 3;
    // AGC 最大数字增益，单位 dB，WebRTC 范围 0~90。
    int agc_compression_gain_db = 12;
    // 是否启用 AGC limiter，避免近场削顶。
    bool agc_limiter = true;
};

struct VadCfg {
    // Silero VAD 触发阈值，范围 0~1。
    float threshold = 0.8f;
    // 语音结束前需要持续静音的时长，单位秒。
    float silence_duration = 0.5f;
};

struct AsrCfg {
    // ASR 后端：sensevoice / qwen3-asr / zipformer。
    std::string engine = "qwen3-asr";
    // Qwen3-ASR llama-server OpenAI-compatible chat completions endpoint。
    std::string endpoint = "http://127.0.0.1:8063/v1/chat/completions";
    // Qwen3-ASR llama-server model tag。
    std::string model = "qwen3-asr";
    // Qwen3-ASR HTTP 请求超时秒数。
    int timeout = 60;
    // 是否由 daemon 自动拉起 Qwen3-ASR llama-server。
    bool auto_start_server = true;
    // Qwen3-ASR llama-server 二进制名或绝对路径。
    std::string server_binary = "llama-server";
    // Qwen3-ASR llama-server 监听地址。
    std::string server_host = "127.0.0.1";
    // Qwen3-ASR llama-server 监听端口。
    int server_port = 8063;
    // Qwen3-ASR text GGUF 模型本地路径。
    std::string model_path =
        "~/.cache/models/asr/qwen3-asr-0.6B-dynq-q40/Qwen3-ASR-0.6B-text-q40.gguf";
    // Qwen3-ASR 模型缺失时输出给用户的参考下载 URL。
    std::string model_url =
        "https://archive.spacemit.com/spacemit-ai/model_zoo/asr/qwen3-asr-0.6B-dynq-q40.tar.gz";
    // Qwen3-ASR SMT 多模态配置目录，包含 encoder onnx 与 config.json。
    std::string smt_config_dir = "~/.cache/models/asr/qwen3-asr-0.6B-dynq-q40";
    // Qwen3-ASR llama-server 上下文长度。
    int ctx_size = 4096;
    // Qwen3-ASR llama-server 推理线程数。
    int threads = 4;
    // Qwen3-ASR llama-server 就绪等待超时，单位秒。
    int startup_timeout = 120;
    // 追加给 Qwen3-ASR llama-server 的原始参数。
    std::vector<std::string> extra_args;
};

struct KwsWakeCfg {
    // 模型目录；空字符串表示 KWS_MODEL_DIR 或 ~/.cache/models/kws/xiaojin-v1。
    std::string model_dir;
    // 关键词得分阈值，(0, 1]。
    float threshold = 0.3f;
    // 两次唤醒的最小间隔，单位毫秒。
    int holdoff_ms = 1000;
    // 快读时只解出半个唤醒词（"小进"）的接受阈值，[0, 1]；0 关闭。
    float partial_threshold = 0.0f;
    // 送 KWS 前先用 3 路裸麦在线学习扬声器零陷（只影响 KWS 这一路，ASR 不变；仅 16 kHz）。
    bool echo_null = false;
    // 零陷用的第一路裸麦（1 起，连续 3 路）；SPV 为 ch2~ch4。
    int echo_null_first_channel = 2;
};

struct WakeCfg {
    // 是否启用唤醒打断。
    bool enabled = false;
    // 唤醒来源："hid" 为 SPV 板端 hidraw 上报（默认，兼容未写 source 的旧配置），
    // "kws" 为本机 KWS 模型（仅 voice_chat_aec，需 -DUSE_KWS=ON 编译）。
    std::string source = "hid";
    // source=hid 时的 hidraw 设备路径。
    std::string device = "/dev/hidraw0";
    // source=kws 时的模型参数。
    KwsWakeCfg kws;
    // TTS 播放中收到唤醒时只中断并播放提示音，不把唤醒词送入 ASR/LLM。
    bool interrupt_mode = true;
    // 唤醒打断后播放的提示音。
    std::string ack_audio = "~/.cache/models/assets/audio/006_im_here.wav";
    // 默认唤醒提示音缺失时的下载地址；空字符串表示不自动下载。
    std::string ack_audio_url =
        "https://archive.spacemit.com/spacemit-ai/model_zoo/assets/audio/006_im_here.wav";
    // 唤醒开启时，从每条识别结果的句首清除唤醒词（重复出现的一并清除）；纯唤醒词不送入 LLM。
    bool strip_from_asr = true;
    // 规范唤醒词及 ASR 常见误识别。
    std::vector<WakePhraseConfig> phrases = defaultWakePhrases();
    // 纯唤醒词后继续等待下一条命令的窗口，单位毫秒。
    int command_timeout_ms = 5000;
    // 旧配置兼容字段；连续采集模式会忽略非零值并打印警告。
    bool legacy_drop_wake_asr_configured = false;
    bool legacy_audio_drop_configured = false;
    int legacy_drop_audio_ms = 0;
    int legacy_post_ack_tail_ms = 0;
};

struct DebugCfg {
    // 是否保存录音调试文件。
    bool save_audio = false;
    // 保存录音调试文件的路径。
    std::string save_audio_file = "voice_debug.wav";
    // 是否保存 ASR 增益后的音频。
    bool save_asr_audio = false;
    // 保存 ASR 增益后音频的路径。
    std::string save_asr_audio_file = "voice_asr_debug.wav";
    // 是否保存 TTS 输出音频。
    bool save_tts_audio = false;
    // 保存 TTS 输出音频的路径。
    std::string save_tts_audio_file = "tts_debug.wav";
    // 是否保存逐帧对齐的 AEC 调试音频（麦克风原始 / 扬声器参考 / ASR 输入 / KWS 输入），
    // 仅 voice_chat_aec 模式生效；SPV 4 声道约 290 KB/s（每小时约 1 GB），只用于测试。
    bool save_aec_dump = false;
    // AEC 调试音频根目录；每次启动在其下建一个与日志同名时间戳的子目录。
    std::string aec_dump_dir = "~/.cache/omni_agent/aec_dumps";
};

struct LlmCfg {
    // 是否由 daemon 自动拉起 llama-server。
    bool auto_start_server = true;
    // OpenAI 兼容云端/外部 API base。非空时 daemon 不启动本地 llama-server。
    std::string api_base;
    // OpenAI 兼容 API key。非空时 daemon 通过 OPENAI_API_KEY 传给 voice_chat 子进程。
    std::string api_key;
    // llama-server 二进制名或绝对路径。
    std::string server_binary = "llama-server";
    // llama-server 监听地址。
    std::string server_host = "127.0.0.1";
    // llama-server 监听端口。
    int server_port = 9191;
    // GGUF 模型本地路径。
    std::string model_path = "~/.cache/models/llm/qwen2.5-0.5b-instruct-q4_0.gguf";
    // 模型缺失时输出给用户的参考下载 URL。
    std::string model_url =
        "https://archive.spacemit.com/spacemit-ai/model_zoo/llm/qwen2.5-0.5b-instruct-q4_0.gguf";
    // 传给 voice_chat --model 的模型名称。
    std::string model_name = "qwen2.5-0.5b";
    // llama-server 上下文长度。
    int ctx_size = 4096;
    // llama-server 推理线程数。
    int threads = 4;
    // Qwen/DeepSeek-R1 等 thinking 模型的 reasoning budget；-1 表示不传。
    int reasoning_budget = 0;
    // 追加给 llama-server 的原始参数。
    std::vector<std::string> extra_args;
    // 传给 voice_chat 的最大生成 token 数。
    int max_tokens = 150;
    // 默认系统提示词；mcp.json 中 system_prompt 非 null 时可覆盖。
    std::string system_prompt = "You are a helpful assistant.";
};

struct VoiceprintCfg {
    // 是否开启声纹验证。
    bool enabled = false;
    // 声纹数据库路径。
    std::string database = "~/.cache/omni_agent/speakers.db";
    // 声纹推理线程数。
    int threads = 1;
    // 声纹相似度阈值。
    float threshold = 0.6f;
    // 输出/检查前 N 个匹配结果。
    int top = 3;
    // 空字符串表示接受所有已注册说话人；非空表示只接受指定 name。
    std::string verify;
};

struct McpCfg {
    // 是否启用 voice_chat MCP client。
    bool enabled = false;
    // MCP LLM backend，透传给 MCP 配置。
    std::string backend = "llama";
    // MCP 专用 system_prompt；system_prompt_set=false 时使用 llm.system_prompt。
    std::string system_prompt;
    bool system_prompt_set = false;
    // MCP 请求超时秒数。
    int timeout = 120;
    // MCP registry URL，空字符串表示不启用 registry。
    std::string registry_url;
    // MCP registry 轮询间隔秒数。
    int registry_poll_interval = 5;
    // MCP servers 原样透传，保留 stdio/http/socket 的类型差异。
    nlohmann::json servers = nlohmann::json::array();
};

struct AecCfg {
    // 是否禁用 AEC。
    bool no_aec = false;
    // 是否禁用噪声抑制。
    bool no_ns = false;
    // 是否启用自动增益控制。
    bool agc = false;
    // AEC 延迟补偿，单位毫秒。
    int aec_delay_ms = 50;
    // AEC buffer frames；0 表示使用 voice_chat_aec 默认值。
    int buffer_frames = 0;
    // WebRTC NS 等级：low|moderate|high|veryhigh；空串表示用 voice_chat_aec 的默认。
    std::string ns_level;
    // NS 之后的固定补偿增益 (dB)，限幅器保护；0 表示不加。
    float fixed_gain_db = 0.0f;
    // AEC 模式下送入前端的录音声道（1-based）；0 表示沿用 audio.speech_channel。
    // 软件 AEC 要在原始麦上工作，本机是 ch2；固件处理过的 ch1 已被门控，不适合。
    int speech_channel = 2;
    // AEC3 参考取哪一路采集（1-based）；0 表示软件回采（写给喇叭的样本）。
    // SPV 2026-09-28 起的固件把 ch1 改成了硬件回采，与麦同时钟，AEC 线性滤波才能收敛。
    int reference_channel = 0;
    // 硬件回采送入 AEC 前的增益 (dB)；该固件的回采比麦低约 12 dB。
    float reference_gain_db = 12.0f;
    // 唤醒后是否丢弃提示音回声期间的音频：auto（硬件回采参考时不丢）| on | off。
    std::string wake_echo_guard = "auto";
};

struct DoaCfg {
    bool enabled = false;
    std::vector<int> pick = {2, 3, 4};
    float side_m = 0.063f;
    std::string positions;
    float azimuth_offset_deg = 0.0f;
    float max_avg_seconds = 3.0f;
    float confidence_threshold = 0.1f;
    float margin_threshold = 0.6f;
    float quality_threshold = 0.0f;
    float min_signal_rms = 0.003f;
    float closure_threshold_samples = 0.0f;
    float closure_threshold_fraction = 0.3f;
};

struct LoadStatus {
    bool loaded = false;
    std::string path;
    std::string error;
};

struct DaemonConfig {
    // 运行模式："voice_chat" 或 "voice_chat_aec"。
    std::string mode = "voice_chat";
    AudioCfg audio;
    AudioFrontendCfg audio_frontend;
    // TTS 后端。
    std::string tts = "matcha:zh-en";
    VadCfg vad;
    AsrCfg asr;
    WakeCfg wake;
    DebugCfg debug;
    // 子进程完全初始化后播放的开机问候；空字符串表示不播放。
    std::string startup_greeting = "你好，请问有什么可以帮到您？";
    LlmCfg llm;
    VoiceprintCfg voiceprint;
    McpCfg mcp;
    AecCfg aec;
    DoaCfg doa;
    // daemon 日志目录。
    std::string log_dir = "~/.cache/omni_agent/logs";
    // daemon PID 文件路径。
    std::string pid_file = "~/.cache/omni_agent/voice_chat_daemon.pid";

    LoadStatus voice_chat_status;
    LoadStatus llm_status;
    LoadStatus voiceprint_status;
    LoadStatus mcp_status;
    LoadStatus aec_status;
};

struct WriteResult {
    std::string path;
    bool written = false;
    std::string error;
};

// 默认用户配置目录（XDG）：~/.config/omni_agent。
std::string DefaultUserConfigDir();

// 兼容旧调用：返回 ~/.config/omni_agent/voice_chat.json。
std::string DefaultUserConfigPath();

// 展开 ~ / $HOME。
std::string ExpandUser(const std::string& path);

// 加载配置目录下的 5 个 JSON；dir 为空 -> DefaultUserConfigDir()。
// 单个文件不存在或解析失败时，该文件字段使用内置默认，并在 LoadStatus 中记录。
DaemonConfig LoadConfig(const std::string& dir = "");

// 一次写入 5 个默认 JSON；overwrite=false 时已存在的文件跳过。
std::vector<WriteResult> WriteDefaultConfigs(const std::string& dir = "", bool overwrite = false);

// 兼容旧调用：只写 voice_chat.json。
bool WriteDefaultConfig(const std::string& path = "");

// 合并视图，输出纯 JSON 字符串：voice_chat/llm/voiceprint/mcp/aec + _meta。
std::string DumpMergedConfig(const DaemonConfig& cfg);

}  // namespace omni_agent

#endif  // DAEMON_CONFIG_HPP
