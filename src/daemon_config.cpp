/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "daemon_config.hpp"

#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace omni_agent {

namespace {

using json = nlohmann::json;

const char* kVoiceChatConfigTemplate = R"({
    "mode": "voice_chat",
    "audio": {
        "input_device_hints":  ["SPV Composite", "USB Audio"],
        "output_device_hints": ["SPV Composite", "USB Audio"],
        "input_device_id":  null,
        "output_device_id": null,
        "capture_rate":     16000,
        "playback_rate":    16000,
        "capture_channels": 4,
        "playback_channels": 2,
        "speech_channel":   1
    },
    "audio_frontend": {
        "enabled":                 true,
        "highpass":                true,
        "noise_suppression":       false,
        "agc":                     true,
        "agc_target_level_dbfs":   3,
        "agc_compression_gain_db": 12,
        "agc_limiter":             true
    },
    "doa": {
        "enabled": true,
        "pick": [2, 3, 4],
        "side_m": 0.063,
        "positions": null,
        "azimuth_offset_deg": 0.0,
        "max_avg_seconds": 3.0,
        "confidence_threshold": 0.1,
        "margin_threshold": 0.6,
        "quality_threshold": 0.0,
        "min_signal_rms": 0.003,
        "closure_threshold_samples": 0.0,
        "closure_threshold_fraction": 0.3
    },
    "tts": "matcha:zh-en",
    "vad": {
        "threshold":        0.8,
        "silence_duration": 0.5
    },
    "asr": {
        "engine":             "qwen3-asr",
        "endpoint":           "http://127.0.0.1:8063/v1/chat/completions",
        "model":              "qwen3-asr",
        "timeout":            60,
        "auto_start_server":  true,
        "server_binary":      "llama-server",
        "server_host":        "127.0.0.1",
        "server_port":        8063,
        "model_path":         "~/.cache/models/asr/qwen3asr/qwen3-asr-0.6B-dynq-q40/Qwen3-ASR-0.6B-text-q40.gguf",
        "model_url":          "https://archive.spacemit.com/spacemit-ai/model_zoo/asr/qwen3-asr-0.6B-dynq-q40.tar.gz",
        "smt_config_dir":     "~/.cache/models/asr/qwen3asr/qwen3-asr-0.6B-dynq-q40",
        "ctx_size":           4096,
        "threads":            4,
        "startup_timeout":    120,
        "extra_args":         []
    },
    "wake": {
        "enabled": false,
        "device": "/dev/hidraw0",
        "interrupt_mode": true,
        "ack_audio": "~/.cache/models/assets/audio/006_im_here.wav",
        "ack_audio_url": "https://archive.spacemit.com/spacemit-ai/model_zoo/assets/audio/006_im_here.wav",
        "drop_wake_asr": true,
        "drop_audio_ms": 500,
        "post_ack_tail_ms": 0
    },
    "debug": {
        "save_audio":      false,
        "save_audio_file": "voice_debug.wav",
        "save_asr_audio":      false,
        "save_asr_audio_file": "voice_asr_debug.wav",
        "save_tts_audio":      false,
        "save_tts_audio_file": "tts_debug.wav"
    },
    "startup_greeting": "你好，请问有什么可以帮到您？",
    "log_dir":  "~/.cache/omni_agent/logs",
    "pid_file": "~/.cache/omni_agent/voice_chat_daemon.pid"
}
)";

const char* kLlmConfigTemplate = R"({
    "auto_start_server": true,
    "api_base":          null,
    "api_key":           null,
    "server_binary":     "llama-server",
    "server_host":       "127.0.0.1",
    "server_port":       9191,
    "model_path":        "~/.cache/models/llm/qwen2.5-0.5b-instruct-q4_0.gguf",
    "model_url":         "https://archive.spacemit.com/spacemit-ai/model_zoo/llm/qwen2.5-0.5b-instruct-q4_0.gguf",
    "model_name":        "qwen2.5-0.5b",
    "ctx_size":          4096,
    "threads":           4,
    "reasoning_budget":  0,
    "extra_args":        [],
    "max_tokens":        150,
    "system_prompt":     "You are a helpful assistant."
}
)";

const char* kVoiceprintConfigTemplate = R"({
    "enabled":   false,
    "database":  "~/.cache/omni_agent/speakers.db",
    "threads":   1,
    "threshold": 0.6,
    "top":       3,
    "verify":    ""
}
)";

const char* kMcpConfigTemplate = R"({
    "enabled":  false,
    "backend":  "llama",
    "system_prompt": null,
    "timeout":  120,
    "registry_url":           null,
    "registry_poll_interval": 5,
    "servers": [
        {
            "name": "Calculator",
            "type": "http",
            "url":  "http://127.0.0.1:8001/mcp"
        },
        {
            "name": "TimeService",
            "type": "http",
            "url":  "http://127.0.0.1:8002/mcp"
        },
        {
            "name": "SystemMonitor",
            "type": "http",
            "url":  "http://127.0.0.1:8003/mcp"
        }
    ]
}
)";

const char* kAecConfigTemplate = R"({
    "no_aec":        false,
    "no_ns":         false,
    "agc":           false,
    "aec_delay_ms":  50,
    "buffer_frames": 0
}
)";

std::string HomeDir() {
    if (const char* h = std::getenv("HOME")) {
        return std::string(h);
    }
    struct passwd pwbuf;
    struct passwd* pw = nullptr;
    char buf[4096];
    if (getpwuid_r(getuid(), &pwbuf, buf, sizeof(buf), &pw) == 0 && pw) {
        return std::string(pw->pw_dir);
    }
    return "/root";
}

bool MakeDirs(const std::string& path) {
    if (path.empty() || path == "/") {
        return true;
    }
    std::string p;
    p.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        p.push_back(path[i]);
        if (path[i] == '/' && i != 0) {
            if (mkdir(p.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
    }
    if (mkdir(p.c_str(), 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

std::string ParentDir(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

template <typename T>
void GetOpt(const json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it != j.end() && !it->is_null()) {
        out = it->get<T>();
    }
}

void ParseAudio(const json& j, AudioCfg& audio) {
    auto it = j.find("audio");
    if (it == j.end() || !it->is_object()) {
        return;
    }
    const json& a = *it;
    GetOpt(a, "input_device_hints", audio.input_device_hints);
    GetOpt(a, "output_device_hints", audio.output_device_hints);
    GetOpt(a, "input_device_id", audio.input_device_id);
    GetOpt(a, "output_device_id", audio.output_device_id);
    GetOpt(a, "capture_rate", audio.capture_rate);
    GetOpt(a, "playback_rate", audio.playback_rate);
    GetOpt(a, "capture_channels", audio.capture_channels);
    GetOpt(a, "playback_channels", audio.playback_channels);
    GetOpt(a, "speech_channel", audio.speech_channel);
}

void ParseAudioFrontend(const json& j, AudioFrontendCfg& frontend) {
    auto it = j.find("audio_frontend");
    if (it == j.end() || !it->is_object()) {
        return;
    }
    const json& f = *it;
    GetOpt(f, "enabled", frontend.enabled);
    GetOpt(f, "highpass", frontend.highpass);
    GetOpt(f, "noise_suppression", frontend.noise_suppression);
    GetOpt(f, "agc", frontend.agc);
    GetOpt(f, "agc_target_level_dbfs", frontend.agc_target_level_dbfs);
    GetOpt(f, "agc_compression_gain_db", frontend.agc_compression_gain_db);
    GetOpt(f, "agc_limiter", frontend.agc_limiter);
}

void ParseDoa(const json& j, DoaCfg& doa) {
    auto it = j.find("doa");
    if (it == j.end() || !it->is_object()) {
        return;
    }
    const json& d = *it;
    GetOpt(d, "enabled", doa.enabled);
    GetOpt(d, "pick", doa.pick);
    GetOpt(d, "side_m", doa.side_m);
    GetOpt(d, "positions", doa.positions);
    GetOpt(d, "azimuth_offset_deg", doa.azimuth_offset_deg);
    GetOpt(d, "max_avg_seconds", doa.max_avg_seconds);
    GetOpt(d, "confidence_threshold", doa.confidence_threshold);
    GetOpt(d, "margin_threshold", doa.margin_threshold);
    GetOpt(d, "quality_threshold", doa.quality_threshold);
    GetOpt(d, "min_signal_rms", doa.min_signal_rms);
    GetOpt(d, "closure_threshold_samples", doa.closure_threshold_samples);
    GetOpt(d, "closure_threshold_fraction", doa.closure_threshold_fraction);
}

void ParseWake(const json& j, WakeCfg& wake) {
    auto it = j.find("wake");
    if (it == j.end() || !it->is_object()) {
        return;
    }
    const json& w = *it;
    GetOpt(w, "enabled", wake.enabled);
    GetOpt(w, "device", wake.device);
    GetOpt(w, "interrupt_mode", wake.interrupt_mode);
    GetOpt(w, "ack_audio", wake.ack_audio);
    GetOpt(w, "ack_audio_url", wake.ack_audio_url);
    GetOpt(w, "drop_wake_asr", wake.drop_wake_asr);
    GetOpt(w, "drop_audio_ms", wake.drop_audio_ms);
    GetOpt(w, "post_ack_tail_ms", wake.post_ack_tail_ms);
}

void ParseAsr(const json& j, AsrCfg& asr) {
    auto it = j.find("asr");
    if (it == j.end() || !it->is_object()) {
        return;
    }
    const json& a = *it;
    GetOpt(a, "engine", asr.engine);
    GetOpt(a, "endpoint", asr.endpoint);
    GetOpt(a, "model", asr.model);
    GetOpt(a, "timeout", asr.timeout);
    GetOpt(a, "auto_start_server", asr.auto_start_server);
    GetOpt(a, "server_binary", asr.server_binary);
    GetOpt(a, "server_host", asr.server_host);
    GetOpt(a, "server_port", asr.server_port);
    GetOpt(a, "model_path", asr.model_path);
    GetOpt(a, "model_url", asr.model_url);
    GetOpt(a, "smt_config_dir", asr.smt_config_dir);
    GetOpt(a, "ctx_size", asr.ctx_size);
    GetOpt(a, "threads", asr.threads);
    GetOpt(a, "startup_timeout", asr.startup_timeout);
    GetOpt(a, "extra_args", asr.extra_args);
}

void ParseVoiceChat(const json& j, DaemonConfig& cfg) {
    std::string mode = cfg.mode;
    AudioCfg audio = cfg.audio;
    AudioFrontendCfg audio_frontend = cfg.audio_frontend;
    std::string tts = cfg.tts;
    VadCfg vad = cfg.vad;
    AsrCfg asr = cfg.asr;
    WakeCfg wake = cfg.wake;
    DebugCfg debug = cfg.debug;
    DoaCfg doa = cfg.doa;
    std::string startup_greeting = cfg.startup_greeting;
    std::string log_dir = cfg.log_dir;
    std::string pid_file = cfg.pid_file;

    GetOpt(j, "mode", mode);
    ParseAudio(j, audio);
    ParseAudioFrontend(j, audio_frontend);
    ParseDoa(j, doa);
    GetOpt(j, "tts", tts);
    if (auto it = j.find("vad"); it != j.end() && it->is_object()) {
        GetOpt(*it, "threshold", vad.threshold);
        GetOpt(*it, "silence_duration", vad.silence_duration);
    }
    ParseAsr(j, asr);
    ParseWake(j, wake);
    if (auto it = j.find("debug"); it != j.end() && it->is_object()) {
        GetOpt(*it, "save_audio", debug.save_audio);
        GetOpt(*it, "save_audio_file", debug.save_audio_file);
        GetOpt(*it, "save_asr_audio", debug.save_asr_audio);
        GetOpt(*it, "save_asr_audio_file", debug.save_asr_audio_file);
        GetOpt(*it, "save_tts_audio", debug.save_tts_audio);
        GetOpt(*it, "save_tts_audio_file", debug.save_tts_audio_file);
    }
    GetOpt(j, "startup_greeting", startup_greeting);
    GetOpt(j, "log_dir", log_dir);
    GetOpt(j, "pid_file", pid_file);

    cfg.mode = mode;
    cfg.audio = audio;
    cfg.audio_frontend = audio_frontend;
    cfg.tts = tts;
    cfg.vad = vad;
    cfg.asr = asr;
    cfg.wake = wake;
    cfg.debug = debug;
    cfg.doa = doa;
    cfg.startup_greeting = startup_greeting;
    cfg.log_dir = log_dir;
    cfg.pid_file = pid_file;
}

void ParseLlm(const json& j, DaemonConfig& cfg) {
    LlmCfg llm = cfg.llm;
    GetOpt(j, "auto_start_server", llm.auto_start_server);
    GetOpt(j, "api_base", llm.api_base);
    GetOpt(j, "api_key", llm.api_key);
    GetOpt(j, "server_binary", llm.server_binary);
    GetOpt(j, "server_host", llm.server_host);
    GetOpt(j, "server_port", llm.server_port);
    GetOpt(j, "model_path", llm.model_path);
    GetOpt(j, "model_url", llm.model_url);
    GetOpt(j, "model_name", llm.model_name);
    GetOpt(j, "ctx_size", llm.ctx_size);
    GetOpt(j, "threads", llm.threads);
    GetOpt(j, "reasoning_budget", llm.reasoning_budget);
    GetOpt(j, "extra_args", llm.extra_args);
    GetOpt(j, "max_tokens", llm.max_tokens);
    GetOpt(j, "system_prompt", llm.system_prompt);
    cfg.llm = llm;
}

void ParseVoiceprint(const json& j, DaemonConfig& cfg) {
    VoiceprintCfg voiceprint = cfg.voiceprint;
    GetOpt(j, "enabled", voiceprint.enabled);
    GetOpt(j, "database", voiceprint.database);
    GetOpt(j, "threads", voiceprint.threads);
    GetOpt(j, "threshold", voiceprint.threshold);
    GetOpt(j, "top", voiceprint.top);
    GetOpt(j, "verify", voiceprint.verify);
    cfg.voiceprint = voiceprint;
}

void ParseMcp(const json& j, DaemonConfig& cfg) {
    McpCfg mcp = cfg.mcp;
    GetOpt(j, "enabled", mcp.enabled);
    GetOpt(j, "backend", mcp.backend);
    auto prompt = j.find("system_prompt");
    if (prompt != j.end()) {
        if (prompt->is_null()) {
            mcp.system_prompt.clear();
            mcp.system_prompt_set = false;
        } else {
            mcp.system_prompt = prompt->get<std::string>();
            mcp.system_prompt_set = true;
        }
    }
    GetOpt(j, "timeout", mcp.timeout);
    GetOpt(j, "registry_url", mcp.registry_url);
    GetOpt(j, "registry_poll_interval", mcp.registry_poll_interval);
    auto servers = j.find("servers");
    if (servers != j.end() && !servers->is_null()) {
        if (!servers->is_array()) {
            throw std::runtime_error("mcp.servers must be an array");
        }
        mcp.servers = *servers;
    }
    cfg.mcp = mcp;
}

void ParseAec(const json& j, DaemonConfig& cfg) {
    AecCfg aec = cfg.aec;
    GetOpt(j, "no_aec", aec.no_aec);
    GetOpt(j, "no_ns", aec.no_ns);
    GetOpt(j, "agc", aec.agc);
    GetOpt(j, "aec_delay_ms", aec.aec_delay_ms);
    GetOpt(j, "buffer_frames", aec.buffer_frames);
    cfg.aec = aec;
}

std::string ConfigPath(const std::string& dir, const char* name) {
    if (dir.empty()) {
        return DefaultUserConfigDir() + "/" + name;
    }
    std::string actual = ExpandUser(dir);
    if (!actual.empty() && actual.back() == '/') {
        actual.pop_back();
    }
    return actual + "/" + name;
}

bool LoadJson(const std::string& path, LoadStatus& status, json& out) {
    status.path = path;
    status.loaded = false;
    status.error.clear();

    std::ifstream f(path);
    if (!f.good()) {
        return false;
    }
    try {
        f >> out;
        status.loaded = true;
        return true;
    } catch (const std::exception& e) {
        status.error = e.what();
        return false;
    }
}

void ExpandPathFields(DaemonConfig& cfg) {
    cfg.debug.save_audio_file = ExpandUser(cfg.debug.save_audio_file);
    cfg.debug.save_asr_audio_file = ExpandUser(cfg.debug.save_asr_audio_file);
    cfg.debug.save_tts_audio_file = ExpandUser(cfg.debug.save_tts_audio_file);
    cfg.wake.device = ExpandUser(cfg.wake.device);
    cfg.wake.ack_audio = ExpandUser(cfg.wake.ack_audio);
    cfg.asr.model_path = ExpandUser(cfg.asr.model_path);
    cfg.asr.smt_config_dir = ExpandUser(cfg.asr.smt_config_dir);
    cfg.llm.model_path = ExpandUser(cfg.llm.model_path);
    cfg.voiceprint.database = ExpandUser(cfg.voiceprint.database);
    cfg.log_dir = ExpandUser(cfg.log_dir);
    cfg.pid_file = ExpandUser(cfg.pid_file);
}

json NullableInt(int value) {
    if (value >= 0) {
        return value;
    }
    return nullptr;
}

json PositiveOrNull(int value) {
    if (value > 0) {
        return value;
    }
    return nullptr;
}

json StatusJson(const LoadStatus& status) {
    json j;
    j["loaded"] = status.loaded;
    j["path"] = status.path;
    j["error"] = status.error;
    return j;
}

json VoiceChatJson(const DaemonConfig& cfg) {
    json audio;
    audio["input_device_hints"] = cfg.audio.input_device_hints;
    audio["output_device_hints"] = cfg.audio.output_device_hints;
    audio["input_device_id"] = NullableInt(cfg.audio.input_device_id);
    audio["output_device_id"] = NullableInt(cfg.audio.output_device_id);
    audio["capture_rate"] = PositiveOrNull(cfg.audio.capture_rate);
    audio["playback_rate"] = PositiveOrNull(cfg.audio.playback_rate);
    audio["capture_channels"] = cfg.audio.capture_channels;
    audio["playback_channels"] = cfg.audio.playback_channels;
    audio["speech_channel"] = cfg.audio.speech_channel;

    json audio_frontend = {
        {"enabled", cfg.audio_frontend.enabled},
        {"highpass", cfg.audio_frontend.highpass},
        {"noise_suppression", cfg.audio_frontend.noise_suppression},
        {"agc", cfg.audio_frontend.agc},
        {"agc_target_level_dbfs", cfg.audio_frontend.agc_target_level_dbfs},
        {"agc_compression_gain_db", cfg.audio_frontend.agc_compression_gain_db},
        {"agc_limiter", cfg.audio_frontend.agc_limiter},
    };

    json doa = {
        {"enabled", cfg.doa.enabled},
        {"pick", cfg.doa.pick},
        {"side_m", cfg.doa.side_m},
        {"positions", cfg.doa.positions.empty() ? json(nullptr) : json(cfg.doa.positions)},
        {"azimuth_offset_deg", cfg.doa.azimuth_offset_deg},
        {"max_avg_seconds", cfg.doa.max_avg_seconds},
        {"confidence_threshold", cfg.doa.confidence_threshold},
        {"margin_threshold", cfg.doa.margin_threshold},
        {"quality_threshold", cfg.doa.quality_threshold},
        {"min_signal_rms", cfg.doa.min_signal_rms},
        {"closure_threshold_samples", cfg.doa.closure_threshold_samples},
        {"closure_threshold_fraction", cfg.doa.closure_threshold_fraction},
    };

    json j;
    j["mode"] = cfg.mode;
    j["audio"] = audio;
    j["audio_frontend"] = audio_frontend;
    j["doa"] = doa;
    j["tts"] = cfg.tts;
    j["vad"] = {
        {"threshold", cfg.vad.threshold},
        {"silence_duration", cfg.vad.silence_duration},
    };
    j["asr"] = {
        {"engine", cfg.asr.engine},
        {"endpoint", cfg.asr.endpoint},
        {"model", cfg.asr.model},
        {"timeout", cfg.asr.timeout},
        {"auto_start_server", cfg.asr.auto_start_server},
        {"server_binary", cfg.asr.server_binary},
        {"server_host", cfg.asr.server_host},
        {"server_port", cfg.asr.server_port},
        {"model_path", cfg.asr.model_path},
        {"model_url", cfg.asr.model_url},
        {"smt_config_dir", cfg.asr.smt_config_dir},
        {"ctx_size", cfg.asr.ctx_size},
        {"threads", cfg.asr.threads},
        {"startup_timeout", cfg.asr.startup_timeout},
        {"extra_args", cfg.asr.extra_args},
    };
    j["wake"] = {
        {"enabled", cfg.wake.enabled},
        {"device", cfg.wake.device},
        {"interrupt_mode", cfg.wake.interrupt_mode},
        {"ack_audio", cfg.wake.ack_audio},
        {"ack_audio_url", cfg.wake.ack_audio_url},
        {"drop_wake_asr", cfg.wake.drop_wake_asr},
        {"drop_audio_ms", cfg.wake.drop_audio_ms},
        {"post_ack_tail_ms", cfg.wake.post_ack_tail_ms},
    };
    j["debug"] = {
        {"save_audio", cfg.debug.save_audio},
        {"save_audio_file", cfg.debug.save_audio_file},
        {"save_asr_audio", cfg.debug.save_asr_audio},
        {"save_asr_audio_file", cfg.debug.save_asr_audio_file},
        {"save_tts_audio", cfg.debug.save_tts_audio},
        {"save_tts_audio_file", cfg.debug.save_tts_audio_file},
    };
    j["startup_greeting"] = cfg.startup_greeting;
    j["log_dir"] = cfg.log_dir;
    j["pid_file"] = cfg.pid_file;
    return j;
}

json LlmJson(const LlmCfg& llm) {
    return json{
        {"auto_start_server", llm.auto_start_server},
        {"api_base", llm.api_base.empty() ? json(nullptr) : json(llm.api_base)},
        {"api_key", llm.api_key.empty() ? json(nullptr) : json("********")},
        {"server_binary", llm.server_binary},
        {"server_host", llm.server_host},
        {"server_port", llm.server_port},
        {"model_path", llm.model_path},
        {"model_url", llm.model_url},
        {"model_name", llm.model_name},
        {"ctx_size", llm.ctx_size},
        {"threads", llm.threads},
        {"reasoning_budget", llm.reasoning_budget},
        {"extra_args", llm.extra_args},
        {"max_tokens", llm.max_tokens},
        {"system_prompt", llm.system_prompt},
    };
}

json VoiceprintJson(const VoiceprintCfg& voiceprint) {
    return json{
        {"enabled", voiceprint.enabled},
        {"database", voiceprint.database},
        {"threads", voiceprint.threads},
        {"threshold", voiceprint.threshold},
        {"top", voiceprint.top},
        {"verify", voiceprint.verify},
    };
}

json McpJson(const McpCfg& mcp) {
    json j;
    j["enabled"] = mcp.enabled;
    j["backend"] = mcp.backend;
    j["system_prompt"] = mcp.system_prompt_set ? json(mcp.system_prompt) : json(nullptr);
    j["timeout"] = mcp.timeout;
    j["registry_url"] = mcp.registry_url.empty() ? json(nullptr) : json(mcp.registry_url);
    j["registry_poll_interval"] = mcp.registry_poll_interval;
    j["servers"] = mcp.servers;
    return j;
}

json AecJson(const AecCfg& aec) {
    return json{
        {"no_aec", aec.no_aec},
        {"no_ns", aec.no_ns},
        {"agc", aec.agc},
        {"aec_delay_ms", aec.aec_delay_ms},
        {"buffer_frames", aec.buffer_frames},
    };
}

}  // namespace

std::string ExpandUser(const std::string& path) {
    if (path.empty()) {
        return path;
    }
    if (path[0] == '~' && (path.size() == 1 || path[1] == '/')) {
        return HomeDir() + path.substr(1);
    }
    const std::string home_var = "$HOME";
    if (path.compare(0, home_var.size(), home_var) == 0 &&
            (path.size() == home_var.size() || path[home_var.size()] == '/')) {
        return HomeDir() + path.substr(home_var.size());
    }
    return path;
}

std::string DefaultUserConfigDir() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        if (*xdg) {
            return std::string(xdg) + "/omni_agent";
        }
    }
    return HomeDir() + "/.config/omni_agent";
}

std::string DefaultUserConfigPath() {
    return DefaultUserConfigDir() + "/voice_chat.json";
}

DaemonConfig LoadConfig(const std::string& dir) {
    DaemonConfig cfg;
    json j;

    if (LoadJson(ConfigPath(dir, "voice_chat.json"), cfg.voice_chat_status, j)) {
        try {
            ParseVoiceChat(j, cfg);
        } catch (const std::exception& e) {
            cfg.voice_chat_status.loaded = false;
            cfg.voice_chat_status.error = e.what();
        }
    }
    j = json();
    if (LoadJson(ConfigPath(dir, "llm.json"), cfg.llm_status, j)) {
        try {
            ParseLlm(j, cfg);
        } catch (const std::exception& e) {
            cfg.llm_status.loaded = false;
            cfg.llm_status.error = e.what();
        }
    }
    j = json();
    if (LoadJson(ConfigPath(dir, "voiceprint.json"), cfg.voiceprint_status, j)) {
        try {
            ParseVoiceprint(j, cfg);
        } catch (const std::exception& e) {
            cfg.voiceprint_status.loaded = false;
            cfg.voiceprint_status.error = e.what();
        }
    }
    j = json();
    if (LoadJson(ConfigPath(dir, "mcp.json"), cfg.mcp_status, j)) {
        try {
            ParseMcp(j, cfg);
        } catch (const std::exception& e) {
            cfg.mcp_status.loaded = false;
            cfg.mcp_status.error = e.what();
        }
    }
    j = json();
    if (LoadJson(ConfigPath(dir, "aec.json"), cfg.aec_status, j)) {
        try {
            ParseAec(j, cfg);
        } catch (const std::exception& e) {
            cfg.aec_status.loaded = false;
            cfg.aec_status.error = e.what();
        }
    }

    ExpandPathFields(cfg);
    return cfg;
}

std::vector<WriteResult> WriteDefaultConfigs(const std::string& dir, bool overwrite) {
    const std::string actual_dir = dir.empty() ? DefaultUserConfigDir() : ExpandUser(dir);
    const std::vector<std::pair<const char*, const char*>> files = {
        {"voice_chat.json", kVoiceChatConfigTemplate},
        {"llm.json", kLlmConfigTemplate},
        {"voiceprint.json", kVoiceprintConfigTemplate},
        {"mcp.json", kMcpConfigTemplate},
        {"aec.json", kAecConfigTemplate},
    };

    std::vector<WriteResult> results;
    results.reserve(files.size());

    if (!MakeDirs(actual_dir)) {
        for (const auto& item : files) {
            WriteResult r;
            r.path = actual_dir + "/" + item.first;
            r.error = "无法创建目录: " + actual_dir + " (" + std::strerror(errno) + ")";
            results.push_back(r);
        }
        return results;
    }

    for (const auto& item : files) {
        WriteResult r;
        r.path = actual_dir + "/" + item.first;
        struct stat st;
        if (!overwrite && stat(r.path.c_str(), &st) == 0) {
            r.written = false;
            results.push_back(r);
            continue;
        }
        std::ofstream f(r.path, std::ios::trunc);
        if (!f) {
            r.error = "无法写入配置文件: " + r.path + " (" + std::strerror(errno) + ")";
            results.push_back(r);
            continue;
        }
        f << item.second;
        r.written = true;
        results.push_back(r);
    }

    return results;
}

bool WriteDefaultConfig(const std::string& path) {
    std::string actual = path.empty() ? DefaultUserConfigPath() : ExpandUser(path);
    struct stat st;
    if (stat(actual.c_str(), &st) == 0) {
        return false;
    }
    if (!MakeDirs(ParentDir(actual))) {
        throw std::runtime_error(
            "无法创建目录: " + ParentDir(actual) + " (" + std::strerror(errno) + ")");
    }
    std::ofstream f(actual);
    if (!f) {
        throw std::runtime_error(
            "无法写入配置文件: " + actual + " (" + std::strerror(errno) + ")");
    }
    f << kVoiceChatConfigTemplate;
    return true;
}

std::string DumpMergedConfig(const DaemonConfig& cfg) {
    json root;
    root["voice_chat"] = VoiceChatJson(cfg);
    root["llm"] = LlmJson(cfg.llm);
    root["voiceprint"] = VoiceprintJson(cfg.voiceprint);
    root["mcp"] = McpJson(cfg.mcp);
    root["aec"] = AecJson(cfg.aec);
    root["_meta"] = {
        {"config_dir", DefaultUserConfigDir()},
        {"load_status", {
            {"voice_chat", StatusJson(cfg.voice_chat_status)},
            {"llm", StatusJson(cfg.llm_status)},
            {"voiceprint", StatusJson(cfg.voiceprint_status)},
            {"mcp", StatusJson(cfg.mcp_status)},
            {"aec", StatusJson(cfg.aec_status)},
        }},
    };
    return root.dump(2);
}

}  // namespace omni_agent
