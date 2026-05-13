/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DAEMON_CONFIG_HPP
#define DAEMON_CONFIG_HPP

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace omni_agent {

struct AudioCfg {
    // 输入设备名 substring 匹配序列，按顺序首个命中即采用。
    std::vector<std::string> input_device_hints = {"SPV Composite", "USB Audio"};
    // 输出设备名 substring 匹配序列，按顺序首个命中即采用。
    std::vector<std::string> output_device_hints = {"SPV Composite", "USB Audio"};
    // -1 表示走 hints 匹配；非负值直接作为 PortAudio 设备 index。
    int input_device_id = -1;
    int output_device_id = -1;
    // -1 表示使用 voice_chat_daemon/底层工具默认采样率。
    int capture_rate = -1;
    int playback_rate = -1;
    // 录音/播放声道数。
    int capture_channels = 1;
    int playback_channels = 1;
};

struct VadCfg {
    // Silero VAD 触发阈值，范围 0~1。
    float threshold = 0.8f;
    // 语音结束前需要持续静音的时长，单位秒。
    float silence_duration = 0.5f;
};

struct DebugCfg {
    // 是否保存录音调试文件。
    bool save_audio = false;
    // 保存录音调试文件的路径。
    std::string save_audio_file = "voice_debug.wav";
    // 是否保存 TTS 输出音频。
    bool save_tts_audio = false;
    // 保存 TTS 输出音频的路径。
    std::string save_tts_audio_file = "tts_debug.wav";
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
    // TTS 后端。
    std::string tts = "matcha:zh-en";
    VadCfg vad;
    DebugCfg debug;
    // 子进程完全初始化后播放的开机问候；空字符串表示不播放。
    std::string startup_greeting = "你好，请问有什么可以帮到您？";
    LlmCfg llm;
    VoiceprintCfg voiceprint;
    McpCfg mcp;
    AecCfg aec;
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
