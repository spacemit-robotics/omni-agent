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
        "capture_rate":     null,
        "playback_rate":    null,
        "capture_channels": 1,
        "playback_channels": 1
    },
    "tts": "matcha:zh-en",
    "vad": {
        "threshold":        0.8,
        "silence_duration": 0.5
    },
    "debug": {
        "save_audio":      false,
        "save_audio_file": "voice_debug.wav"
    },
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
    "model_path":        "~/.cache/models/llm/Qwen3-0.6B-Q4_0.gguf",
    "model_url":         "https://archive.spacemit.com/spacemit-ai/model_zoo/llm/Qwen3-0.6B-Q4_0.gguf",
    "model_name":        "Qwen3-0.6B",
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
    "servers": []
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
}

void ParseVoiceChat(const json& j, DaemonConfig& cfg) {
    std::string mode = cfg.mode;
    AudioCfg audio = cfg.audio;
    std::string tts = cfg.tts;
    VadCfg vad = cfg.vad;
    DebugCfg debug = cfg.debug;
    std::string log_dir = cfg.log_dir;
    std::string pid_file = cfg.pid_file;

    GetOpt(j, "mode", mode);
    ParseAudio(j, audio);
    GetOpt(j, "tts", tts);
    if (auto it = j.find("vad"); it != j.end() && it->is_object()) {
        GetOpt(*it, "threshold", vad.threshold);
        GetOpt(*it, "silence_duration", vad.silence_duration);
    }
    if (auto it = j.find("debug"); it != j.end() && it->is_object()) {
        GetOpt(*it, "save_audio", debug.save_audio);
        GetOpt(*it, "save_audio_file", debug.save_audio_file);
    }
    GetOpt(j, "log_dir", log_dir);
    GetOpt(j, "pid_file", pid_file);

    cfg.mode = mode;
    cfg.audio = audio;
    cfg.tts = tts;
    cfg.vad = vad;
    cfg.debug = debug;
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

    json j;
    j["mode"] = cfg.mode;
    j["audio"] = audio;
    j["tts"] = cfg.tts;
    j["vad"] = {
        {"threshold", cfg.vad.threshold},
        {"silence_duration", cfg.vad.silence_duration},
    };
    j["debug"] = {
        {"save_audio", cfg.debug.save_audio},
        {"save_audio_file", cfg.debug.save_audio_file},
    };
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
