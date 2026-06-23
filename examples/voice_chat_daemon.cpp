/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * voice_chat_daemon - omni_agent user-facing launcher
 *
 * 用户入口:
 *   start [--aec] [--mcp]
 *   restart [--aec] [--mcp]
 *   stop
 *   status
 *   logs
 *   config-init
 *   config-show
 *   --register-speaker NAME [--force]
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <portaudio.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include "audio_base.hpp"
#include "daemon_config.hpp"

namespace {

using omni_agent::DaemonConfig;
using json = nlohmann::json;

// -----------------------------------------------------------------------------
// 辅助函数
// -----------------------------------------------------------------------------

class ScopedStderrSilencer {
public:
    ScopedStderrSilencer() {
        original_fd_ = dup(fileno(stderr));
        if (original_fd_ < 0) {
            return;
        }
        null_stream_ = fopen("/dev/null", "w");
        if (!null_stream_) {
            close(original_fd_);
            original_fd_ = -1;
            return;
        }
        fflush(stderr);
        dup2(fileno(null_stream_), fileno(stderr));
    }

    ~ScopedStderrSilencer() {
        if (original_fd_ >= 0) {
            fflush(stderr);
            dup2(original_fd_, fileno(stderr));
            close(original_fd_);
        }
        if (null_stream_) {
            fclose(null_stream_);
        }
    }

    ScopedStderrSilencer(const ScopedStderrSilencer&) = delete;
    ScopedStderrSilencer& operator=(const ScopedStderrSilencer&) = delete;

private:
    int original_fd_ = -1;
    FILE* null_stream_ = nullptr;
};

class CurlGlobalRuntime {
public:
    CurlGlobalRuntime() = default;

    bool Init(std::string* error) {
        CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (rc != CURLE_OK) {
            if (error) {
                *error = std::string("curl_global_init failed: ")
                    + curl_easy_strerror(rc);
            }
            return false;
        }
        initialized_ = true;
        return true;
    }

    ~CurlGlobalRuntime() {
        if (initialized_) {
            curl_global_cleanup();
        }
    }

    CurlGlobalRuntime(const CurlGlobalRuntime&) = delete;
    CurlGlobalRuntime& operator=(const CurlGlobalRuntime&) = delete;

private:
    bool initialized_ = false;
};

std::string Timestamp() {
    char buf[64];
    std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", std::localtime(&t));
    return std::string(buf);
}

bool FileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool DirExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
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
    size_t s = path.find_last_of('/');
    return s == std::string::npos ? "." : path.substr(0, s);
}

size_t WriteDownloadFile(char* ptr, size_t size, size_t nmemb, void* userdata) {
    FILE* f = static_cast<FILE*>(userdata);
    size_t bytes = size * nmemb;
    return std::fwrite(ptr, 1, bytes, f);
}

bool DownloadFile(const std::string& url, const std::string& path,
        std::string* error) {
    if (url.empty()) {
        if (error) *error = "download URL is empty";
        return false;
    }
    if (!MakeDirs(ParentDir(path))) {
        if (error) *error = "failed to create directory: " + ParentDir(path);
        return false;
    }

    std::string tmp_path = path + ".tmp." + std::to_string(getpid());
    FILE* out = std::fopen(tmp_path.c_str(), "wb");
    if (!out) {
        if (error) {
            *error = tmp_path + ": " + std::strerror(errno);
        }
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::fclose(out);
        std::remove(tmp_path.c_str());
        if (error) *error = "curl_easy_init failed";
        return false;
    }

    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteDownloadFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "omni_agent/voice_chat_daemon");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    bool close_ok = std::fclose(out) == 0;
    if (rc != CURLE_OK) {
        std::remove(tmp_path.c_str());
        const char* msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        if (error) *error = std::string("curl: ") + msg;
        return false;
    }
    if (http_code >= 400) {
        std::remove(tmp_path.c_str());
        if (error) {
            *error = "HTTP " + std::to_string(http_code) + " from " + url;
        }
        return false;
    }
    if (!close_ok) {
        std::remove(tmp_path.c_str());
        if (error) *error = tmp_path + ": failed to flush file";
        return false;
    }
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        std::remove(tmp_path.c_str());
        if (error) {
            *error = "rename " + tmp_path + " -> " + path + ": "
                + std::strerror(errno);
        }
        return false;
    }
    return true;
}

bool HasFlag(int argc, char** argv, const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == flag) {
            return true;
        }
    }
    return false;
}

bool BinaryOnPath(const std::string& name) {
    if (name.find('/') != std::string::npos) {
        return access(name.c_str(), X_OK) == 0;
    }
    const char* p = std::getenv("PATH");
    if (!p) {
        return false;
    }
    std::string path(p);
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        if (end == std::string::npos) {
            end = path.size();
        }
        std::string dir = path.substr(start, end - start);
        if (!dir.empty()) {
            std::string full = dir + "/" + name;
            if (access(full.c_str(), X_OK) == 0) {
                return true;
            }
        }
        start = end + 1;
    }
    return false;
}

bool PortInUse(const std::string& host, int port) {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    int gai = getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
    if (gai != 0) {
        return false;
    }

    bool connected = false;
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        int s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s < 0) {
            continue;
        }
        if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0) {
            connected = true;
            close(s);
            break;
        }
        close(s);
    }
    freeaddrinfo(result);
    return connected;
}

bool WaitPortReady(const std::string& host, int port, int timeout_sec) {
    for (int i = 0; i < timeout_sec * 4; ++i) {
        if (PortInUse(host, port)) {
            return true;
        }
        usleep(250 * 1000);
    }
    return false;
}

int RunSyncWithEnv(const std::string& prog,
        const std::vector<std::string>& args,
        const std::vector<std::pair<std::string, std::string>>& envs) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        for (const auto& env : envs) {
            setenv(env.first.c_str(), env.second.c_str(), 1);
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(prog.c_str()));
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        execvp(prog.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int RunSync(const std::string& prog, const std::vector<std::string>& args) {
    return RunSyncWithEnv(prog, args, {});
}

pid_t SpawnAsyncWithEnv(const std::string& prog,
        const std::vector<std::string>& args,
        const std::vector<std::pair<std::string, std::string>>& envs) {
    int err_pipe[2];
    if (pipe(err_pipe) != 0) {
        return -1;
    }
    fcntl(err_pipe[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close(err_pipe[0]);
        close(err_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        close(err_pipe[0]);
        for (const auto& env : envs) {
            setenv(env.first.c_str(), env.second.c_str(), 1);
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(prog.c_str()));
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        execvp(prog.c_str(), argv.data());
        int err = errno;
        ssize_t written = write(err_pipe[1], &err, sizeof(err));
        (void)written;
        std::perror(("execvp " + prog).c_str());
        _exit(127);
    }
    close(err_pipe[1]);
    int child_errno = 0;
    ssize_t n = 0;
    do {
        n = read(err_pipe[0], &child_errno, sizeof(child_errno));
    } while (n < 0 && errno == EINTR);
    close(err_pipe[0]);
    if (n > 0) {
        waitpid(pid, nullptr, 0);
        errno = child_errno;
        return -1;
    }
    return pid;
}

pid_t SpawnAsync(const std::string& prog, const std::vector<std::string>& args) {
    return SpawnAsyncWithEnv(prog, args, {});
}

bool ProcessZombie(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    std::string stat;
    if (!std::getline(f, stat)) {
        return false;
    }
    size_t rparen = stat.rfind(')');
    if (rparen == std::string::npos || rparen + 2 >= stat.size()) {
        return false;
    }
    return stat[rparen + 2] == 'Z';
}

bool ProcessAlive(pid_t pid) {
    if (pid <= 0) {
        return false;
    }
    if (kill(pid, 0) == 0) {
        return !ProcessZombie(pid);
    }
    return errno == EPERM && !ProcessZombie(pid);
}

int MatchDevice(const std::vector<std::pair<int, std::string>>& devs,
        const std::vector<std::string>& hints) {
    for (const auto& hint : hints) {
        for (const auto& d : devs) {
            if (d.second.find(hint) != std::string::npos) {
                return d.first;
            }
        }
    }
    return -1;
}

void PrintConfigLoadErrors(const DaemonConfig& cfg) {
    const std::vector<std::pair<const char*, const omni_agent::LoadStatus*>> statuses = {
        {"voice_chat.json", &cfg.voice_chat_status},
        {"llm.json", &cfg.llm_status},
        {"voiceprint.json", &cfg.voiceprint_status},
        {"mcp.json", &cfg.mcp_status},
        {"aec.json", &cfg.aec_status},
    };
    for (const auto& item : statuses) {
        const auto& status = *item.second;
        if (!status.error.empty()) {
            std::cerr << "[warn] 配置文件解析失败，使用默认值: "
                << status.path << ": " << status.error << "\n";
        }
    }
}

bool EnsureDefaultConfigs() {
    bool ok = true;
    for (const auto& result : omni_agent::WriteDefaultConfigs()) {
        if (!result.error.empty()) {
            std::cerr << "[warn] 自动初始化配置失败: "
                << result.path << ": " << result.error << "\n";
            ok = false;
        } else if (result.written) {
            std::cerr << "[info] 已写入默认配置: " << result.path << "\n";
        }
    }
    return ok;
}

void PrintDevices(const char* label,
        const std::vector<std::pair<int, std::string>>& devs) {
    std::cerr << "  " << label << ":\n";
    if (devs.empty()) {
        std::cerr << "    (无可用设备)\n";
        return;
    }
    for (const auto& d : devs) {
        std::cerr << "    [" << d.first << "] " << d.second << "\n";
    }
}

void PrintHints(const std::vector<std::string>& hints) {
    std::cerr << "  hints: [";
    for (size_t i = 0; i < hints.size(); ++i) {
        std::cerr << (i ? ", " : "") << "\"" << hints[i] << "\"";
    }
    std::cerr << "]\n";
}

bool GetDeviceMaxChannels(bool input, int device_id, int& max_channels) {
    max_channels = 0;
    ScopedStderrSilencer silencer;
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::cerr << "[warn] PortAudio 初始化失败，无法读取设备声道数: "
            << Pa_GetErrorText(err) << "\n";
        return false;
    }

    const int pa_id = device_id >= 0
        ? device_id
        : (input ? Pa_GetDefaultInputDevice() : Pa_GetDefaultOutputDevice());
    const PaDeviceInfo* info = pa_id >= 0 ? Pa_GetDeviceInfo(pa_id) : nullptr;
    if (info) {
        max_channels = input ? info->maxInputChannels : info->maxOutputChannels;
    }
    Pa_Terminate();
    return info != nullptr;
}

bool ResolveDevice(const char* kind,
        int configured_id,
        const std::vector<std::string>& hints,
        const std::vector<std::pair<int, std::string>>& devs,
        int& resolved_id) {
    resolved_id = configured_id;
    if (resolved_id >= 0 || hints.empty()) {
        return true;
    }
    resolved_id = MatchDevice(devs, hints);
    if (resolved_id >= 0) {
        return true;
    }
    std::cerr << "错误: 没有匹配到" << kind << "设备\n";
    PrintHints(hints);
    PrintDevices((std::string("当前可见") + kind + "设备").c_str(), devs);
    std::cerr << "排查:\n";
    std::cerr << "  - 接好 USB 音频设备后重试\n";
    std::cerr << "  - 或编辑 ~/.config/omni_agent/voice_chat.json，调整 audio 设备配置\n";
    return false;
}

// -----------------------------------------------------------------------------
// PID 文件
// -----------------------------------------------------------------------------

struct PidRecord {
    pid_t daemon_pid = -1;
    pid_t llama_pid = -1;
    pid_t asr_pid = -1;
    pid_t voice_pid = -1;
    std::string mode;
    std::string log_path;
};

bool ReadPidFile(const std::string& path, PidRecord* out) {
    std::ifstream f(path);
    if (!f) {
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (k == "daemon") {
            out->daemon_pid = std::stoi(v);
        } else if (k == "llama") {
            out->llama_pid = std::stoi(v);
        } else if (k == "asr") {
            out->asr_pid = std::stoi(v);
        } else if (k == "voice_chat") {
            out->voice_pid = std::stoi(v);
        } else if (k == "mode") {
            out->mode = v;
        } else if (k == "log") {
            out->log_path = v;
        }
    }
    return out->daemon_pid > 0;
}

bool WritePidFile(const std::string& path, const PidRecord& rec) {
    if (!MakeDirs(ParentDir(path))) {
        return false;
    }
    std::ofstream f(path, std::ios::trunc);
    if (!f) {
        return false;
    }
    f << "daemon=" << rec.daemon_pid << "\n";
    f << "llama=" << rec.llama_pid << "\n";
    f << "asr=" << rec.asr_pid << "\n";
    f << "voice_chat=" << rec.voice_pid << "\n";
    f << "mode=" << rec.mode << "\n";
    f << "log=" << rec.log_path << "\n";
    return true;
}

std::string FormatPidStatus(pid_t pid) {
    if (pid <= 0) {
        return "-";
    }
    return std::to_string(pid) + (ProcessAlive(pid) ? "" : " (DEAD)");
}

void WriteStartupStatus(int fd, char status) {
    if (fd < 0) {
        return;
    }
    struct sigaction ignore{};
    struct sigaction old{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, &old);
    ssize_t n = write(fd, &status, 1);
    (void)n;
    sigaction(SIGPIPE, &old, nullptr);
    close(fd);
}

std::string TrimTrailingSlash(std::string api_base) {
    while (!api_base.empty() && api_base.back() == '/') {
        api_base.pop_back();
    }
    return api_base;
}

std::string EffectiveLlmUrl(const DaemonConfig& cfg) {
    if (!cfg.llm.api_base.empty()) {
        return TrimTrailingSlash(cfg.llm.api_base);
    }
    return "http://" + cfg.llm.server_host + ":"
        + std::to_string(cfg.llm.server_port) + "/v1";
}

bool ShouldStartLocalLlm(const DaemonConfig& cfg) {
    return cfg.llm.auto_start_server && cfg.llm.api_base.empty();
}

bool IsQwen3Asr(const std::string& engine) {
    return engine == "qwen3-asr" || engine == "qwen3_asr";
}

std::string EffectiveAsrEndpoint(const DaemonConfig& cfg) {
    if (IsQwen3Asr(cfg.asr.engine) && cfg.asr.auto_start_server) {
        return "http://" + cfg.asr.server_host + ":"
            + std::to_string(cfg.asr.server_port) + "/v1/chat/completions";
    }
    return cfg.asr.endpoint;
}

bool ShouldStartLocalAsr(const DaemonConfig& cfg) {
    return IsQwen3Asr(cfg.asr.engine) && cfg.asr.auto_start_server;
}

bool EnsureWakeAckAudio(const DaemonConfig& cfg) {
    if (!cfg.wake.enabled || !cfg.wake.interrupt_mode ||
            cfg.wake.ack_audio.empty()) {
        return true;
    }
    if (FileExists(cfg.wake.ack_audio)) {
        return true;
    }
    if (cfg.wake.ack_audio_url.empty()) {
        std::cerr << "错误: 唤醒提示音不存在: " << cfg.wake.ack_audio << "\n";
        std::cerr << "      wake.ack_audio_url 为空，无法自动下载\n";
        return false;
    }

    std::cerr << "[info] downloading wake ack audio " << cfg.wake.ack_audio_url
        << "\n";
    std::string error;
    if (!DownloadFile(cfg.wake.ack_audio_url, cfg.wake.ack_audio, &error)) {
        std::cerr << "错误: 下载唤醒提示音失败: " << error << "\n";
        std::cerr << "      target: " << cfg.wake.ack_audio << "\n";
        return false;
    }
    std::cerr << "[info] wake ack audio ready: " << cfg.wake.ack_audio << "\n";
    return true;
}

bool HttpHealthOk(const std::string& host, int port) {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    int gai = getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
    if (gai != 0) {
        return false;
    }

    bool ok = false;
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        int s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s < 0) {
            continue;
        }
        if (connect(s, rp->ai_addr, rp->ai_addrlen) != 0) {
            close(s);
            continue;
        }
        const std::string request =
            "GET /health HTTP/1.1\r\nHost: " + host
            + "\r\nConnection: close\r\n\r\n";
        ssize_t sent = send(s, request.data(), request.size(), 0);
        if (sent < 0) {
            close(s);
            continue;
        }

        std::string response;
        char buf[1024];
        while (true) {
            ssize_t n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            response.append(buf, n);
            if (response.size() > 8192) {
                break;
            }
        }
        close(s);
        if (response.find("HTTP/1.1 200") != std::string::npos ||
                response.find("HTTP/1.0 200") != std::string::npos) {
            ok = true;
            break;
        }
    }
    freeaddrinfo(result);
    return ok;
}

bool WaitHttpHealthReady(const std::string& host, int port, int timeout_sec) {
    if (timeout_sec <= 0) {
        timeout_sec = 1;
    }
    for (int i = 0; i < timeout_sec * 4; ++i) {
        if (HttpHealthOk(host, port)) {
            return true;
        }
        usleep(250 * 1000);
    }
    return false;
}

std::string DirName(const std::string& path);
std::string CurrentWorkingDir();
bool IsMcpServicesDir(const std::string& dir);
std::string DefaultMcpServicesDir();
std::string FindMcpSeedExamplesDir();
std::string EnsureMcpServicesDir();

bool IsAbsolutePath(const std::string& path) {
    return !path.empty() && path[0] == '/';
}

bool StartsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size()
        && value.compare(0, prefix.size(), prefix) == 0;
}

std::string RealPathIfExists(const std::string& path) {
    if (path.empty()) {
        return "";
    }
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved)) {
        return std::string(resolved);
    }
    return "";
}

std::string ResolveMcpServerPath(const std::string& raw_path) {
    if (raw_path.empty()) {
        return raw_path;
    }

    std::string path = omni_agent::ExpandUser(raw_path);
    if (IsAbsolutePath(path)) {
        return path;
    }

    std::vector<std::string> candidates;
    candidates.push_back(CurrentWorkingDir() + "/" + path);

    const bool built_in_style =
        StartsWith(path, "examples/services/") || StartsWith(path, "services/");
    std::string services_dir = DefaultMcpServicesDir();
    if (built_in_style && !IsMcpServicesDir(services_dir)) {
        services_dir = EnsureMcpServicesDir();
    }
    if (!services_dir.empty()) {
        candidates.push_back(services_dir + "/" + path);
        if (StartsWith(path, "examples/")) {
            candidates.push_back(services_dir + "/" + path.substr(std::string("examples/").size()));
        }
    }

    const std::string seed_examples_dir = FindMcpSeedExamplesDir();
    if (!seed_examples_dir.empty()) {
        candidates.push_back(seed_examples_dir + "/" + path);
        candidates.push_back(DirName(seed_examples_dir) + "/" + path);
        if (StartsWith(path, "examples/")) {
            candidates.push_back(seed_examples_dir + "/" + path.substr(std::string("examples/").size()));
        }
    }

    for (const auto& candidate : candidates) {
        std::string resolved = RealPathIfExists(candidate);
        if (!resolved.empty()) {
            return resolved;
        }
    }

    return path;
}

std::string ResolveMcpStdioArg(const std::string& arg) {
    if (arg.empty() || arg[0] == '-') {
        return arg;
    }
    std::string expanded = omni_agent::ExpandUser(arg);
    if (expanded.find('/') == std::string::npos) {
        return expanded;
    }
    return ResolveMcpServerPath(expanded);
}

json ResolveMcpServers(json servers) {
    if (!servers.is_array()) {
        return servers;
    }

    for (auto& server : servers) {
        if (!server.is_object()) {
            continue;
        }
        const std::string type = server.value("type", "http");
        if (type != "stdio") {
            continue;
        }
        if (server.contains("command") && server["command"].is_string()) {
            const std::string command = server["command"].get<std::string>();
            if (command.find('/') != std::string::npos) {
                server["command"] = ResolveMcpServerPath(command);
            }
        }
        if (server.contains("args") && server["args"].is_array()) {
            for (auto& arg : server["args"]) {
                if (arg.is_string()) {
                    arg = ResolveMcpStdioArg(arg.get<std::string>());
                }
            }
        }
    }

    return servers;
}

// -----------------------------------------------------------------------------
// MCP resolved config
// -----------------------------------------------------------------------------

bool WriteMcpResolvedConfig(const DaemonConfig& cfg, std::string& out_path) {
    out_path.clear();
    if (!cfg.mcp.enabled) {
        return true;
    }
    if (cfg.mcp.servers.empty() && cfg.mcp.registry_url.empty()) {
        std::cerr << "[info] mcp enabled but no servers/registry configured; skip --mcp-config\n";
        return true;
    }

    out_path = omni_agent::ExpandUser("~/.cache/omni_agent/mcp_resolved.json");
    if (!MakeDirs(ParentDir(out_path))) {
        std::cerr << "错误: 无法创建 MCP 配置目录 " << ParentDir(out_path) << "\n";
        return false;
    }

    json j;
    j["backend"] = cfg.mcp.backend;
    j["url"] = EffectiveLlmUrl(cfg);
    j["model"] = cfg.llm.model_name;
    j["timeout"] = cfg.mcp.timeout;
    j["system_prompt"] = cfg.mcp.system_prompt_set
        ? cfg.mcp.system_prompt
        : cfg.llm.system_prompt;
    if (!cfg.mcp.registry_url.empty()) {
        j["registry_url"] = cfg.mcp.registry_url;
        j["registry_poll_interval"] = cfg.mcp.registry_poll_interval;
    }
    j["servers"] = ResolveMcpServers(cfg.mcp.servers);

    std::ofstream f(out_path, std::ios::trunc);
    if (!f) {
        std::cerr << "错误: 无法写入 MCP 配置文件 " << out_path << "\n";
        return false;
    }
    f << j.dump(2) << "\n";
    return true;
}

std::string DirName(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

std::string CurrentWorkingDir() {
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf))) {
        return std::string(buf);
    }
    return ".";
}

std::string ExeDir() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return ".";
    }
    buf[n] = '\0';
    return DirName(buf);
}

std::string DefaultMcpServicesDir() {
    if (const char* env = std::getenv("OMNI_AGENT_MCP_SERVICES_DIR")) {
        if (*env) {
            return omni_agent::ExpandUser(env);
        }
    }
    return omni_agent::ExpandUser("~/.local/share/omni_agent/mcp/services");
}

bool IsMcpServicesDir(const std::string& dir) {
    if (dir.empty() || !DirExists(dir)) {
        return false;
    }
    const std::string script = dir + "/start_all_services.sh";
    return access(script.c_str(), X_OK) == 0;
}

std::string FindMcpSeedExamplesDir() {
    std::vector<std::string> candidates;
    if (const char* env = std::getenv("OMNI_AGENT_MCP_EXAMPLES_DIR")) {
        if (*env) {
            candidates.push_back(omni_agent::ExpandUser(env));
        }
    }
    const std::string cwd = CurrentWorkingDir();
    const std::string exe_dir = ExeDir();
    candidates.push_back(cwd + "/components/agent_tools/mcp/examples");
    candidates.push_back(cwd + "/../components/agent_tools/mcp/examples");
    candidates.push_back(exe_dir + "/../../../components/agent_tools/mcp/examples");

    for (const auto& dir : candidates) {
        if (IsMcpServicesDir(dir)) {
            return dir;
        }
    }
    return "";
}

bool SyncMcpServicesDir(const std::string& seed_dir, const std::string& services_dir) {
    if (seed_dir.empty() || services_dir.empty()) {
        return false;
    }
    if (!MakeDirs(services_dir)) {
        std::cerr << "错误: 无法创建 MCP 固定服务目录: " << services_dir << "\n";
        return false;
    }
    int rc = RunSync("cp", {"-a", seed_dir + "/.", services_dir});
    if (rc != 0) {
        std::cerr << "错误: 同步 MCP 服务到固定目录失败 (rc=" << rc << ")\n";
        std::cerr << "  source: " << seed_dir << "\n";
        std::cerr << "  target: " << services_dir << "\n";
        return false;
    }
    return IsMcpServicesDir(services_dir);
}

std::string EnsureMcpServicesDir() {
    const std::string services_dir = DefaultMcpServicesDir();
    if (IsMcpServicesDir(services_dir)) {
        return services_dir;
    }

    const std::string seed_dir = FindMcpSeedExamplesDir();
    if (seed_dir.empty()) {
        std::cerr << "错误: --mcp 需要 MCP 服务种子目录，但找不到 "
            << "components/agent_tools/mcp/examples/start_all_services.sh\n";
        std::cerr << "排查: 确认 SDK 中包含 components/agent_tools/mcp/examples，"
            << "或设置 OMNI_AGENT_MCP_EXAMPLES_DIR\n";
        return "";
    }

    std::cerr << "[info] 初始化 MCP 固定服务目录: " << services_dir << "\n";
    std::cerr << "[info] MCP 服务种子目录: " << seed_dir << "\n";
    if (!SyncMcpServicesDir(seed_dir, services_dir)) {
        return "";
    }
    return services_dir;
}

bool PythonImportsOk(const std::string& python) {
    int rc = RunSync(python, {
        "-c",
        "import mcp, starlette, uvicorn, psutil, flask",
    });
    return rc == 0;
}

void PrintMcpPythonSetupHelp(const std::string& python) {
    std::cerr << "需要包: mcp starlette uvicorn psutil flask\n";
    std::cerr << "初始化示例:\n";
    std::cerr << "  python3 -m venv ~/.mcp-env\n";
    std::cerr << "  ~/.mcp-env/bin/python -m pip install "
        << "mcp starlette uvicorn psutil flask\n";
    std::cerr << "或设置 OMNI_AGENT_MCP_PYTHON 指向已安装依赖的 Python\n";
    if (!python.empty()) {
        std::cerr << "当前 Python: " << python << "\n";
    }
}

std::string EnsureMcpPythonBinDir() {
    if (const char* py = std::getenv("OMNI_AGENT_MCP_PYTHON")) {
        if (*py) {
            std::string python(py);
            if (!PythonImportsOk(python)) {
                std::cerr << "错误: OMNI_AGENT_MCP_PYTHON 缺少 MCP 依赖\n";
                PrintMcpPythonSetupHelp(python);
                return "";
            }
            return DirName(python);
        }
    }

    const std::string env_dir = omni_agent::ExpandUser("~/.mcp-env");
    const std::string python = env_dir + "/bin/python";
    if (access(python.c_str(), X_OK) != 0) {
        std::cerr << "错误: MCP Python 环境不存在: " << python << "\n";
        PrintMcpPythonSetupHelp(python);
        return "";
    }

    if (!PythonImportsOk(python)) {
        std::cerr << "错误: MCP Python 依赖不可用: " << env_dir << "\n";
        PrintMcpPythonSetupHelp(python);
        return "";
    }
    return env_dir + "/bin";
}

std::string BuildMcpPythonPath(const std::string& preferred_bin = "") {
    const char* old_path = std::getenv("PATH");
    std::string path = old_path ? old_path : "";
    std::vector<std::string> bins;
    if (!preferred_bin.empty()) {
        bins.push_back(preferred_bin);
    }
    bins.push_back(omni_agent::ExpandUser("~/.mcp-env/bin"));
    bins.push_back(CurrentWorkingDir() + "/.venv/bin");
    bins.push_back(CurrentWorkingDir() + "/../.venv/bin");

    for (auto it = bins.rbegin(); it != bins.rend(); ++it) {
        if (DirExists(*it)) {
            path = *it + (path.empty() ? "" : ":" + path);
        }
    }
    return path;
}

bool McpExamplePortsReady() {
    return PortInUse("127.0.0.1", 9000)
        && PortInUse("127.0.0.1", 8001)
        && PortInUse("127.0.0.1", 8002)
        && PortInUse("127.0.0.1", 8003);
}

bool WaitMcpExamplePortsReady(int timeout_sec) {
    for (int i = 0; i < timeout_sec * 4; ++i) {
        if (McpExamplePortsReady()) {
            return true;
        }
        usleep(250 * 1000);
    }
    return false;
}

bool IsDefaultMcpExampleUrl(const std::string& url) {
    return url == "http://127.0.0.1:8001/mcp"
        || url == "http://127.0.0.1:8002/mcp"
        || url == "http://127.0.0.1:8003/mcp";
}

bool UsesDefaultMcpExampleServices(const json& servers) {
    if (!servers.is_array()) {
        return false;
    }
    for (const auto& server : servers) {
        if (!server.is_object()) {
            continue;
        }
        const std::string type = server.value("type", "http");
        if (type != "http") {
            continue;
        }
        const std::string url = server.value("url", "");
        if (IsDefaultMcpExampleUrl(url)) {
            return true;
        }
    }
    return false;
}

bool StopMcpExampleServices() {
    std::string services_dir = EnsureMcpServicesDir();
    if (services_dir.empty()) {
        return false;
    }
    std::string script = services_dir + "/start_all_services.sh";
    RunSyncWithEnv(script, {"stop"}, {{"PATH", BuildMcpPythonPath()}});
    return true;
}

bool EnsureConfiguredMcpExampleServices(const DaemonConfig& cfg, bool& auto_started) {
    auto_started = false;
    if (!cfg.mcp.enabled || !UsesDefaultMcpExampleServices(cfg.mcp.servers)) {
        return true;
    }

    if (McpExamplePortsReady()) {
        std::cerr << "[info] MCP example services already running\n";
        return true;
    }

    std::string services_dir = EnsureMcpServicesDir();
    if (services_dir.empty()) {
        return false;
    }
    std::string python_bin = EnsureMcpPythonBinDir();
    if (python_bin.empty()) {
        return false;
    }

    std::string script = services_dir + "/start_all_services.sh";
    std::string path = BuildMcpPythonPath(python_bin);
    std::cerr << "[info] starting configured MCP example services: "
        << script << "\n";
    int rc = RunSyncWithEnv(script, {"start"}, {{"PATH", path}});
    if (rc != 0) {
        std::cerr << "错误: MCP 服务启动失败 (rc=" << rc << ")\n";
        std::cerr << "排查: 查看 /tmp/mcp_registry.log /tmp/mcp_calculator.log "
            << "/tmp/mcp_time.log /tmp/mcp_system.log\n";
        std::cerr << "依赖: mcp starlette uvicorn psutil flask；"
            << "daemon 会优先使用 ~/.mcp-env/bin/python\n";
        return false;
    }
    if (!WaitMcpExamplePortsReady(10)) {
        std::cerr << "错误: MCP 服务 10 秒内未就绪\n";
        std::cerr << "排查: 查看 /tmp/mcp_registry.log /tmp/mcp_calculator.log "
            << "/tmp/mcp_time.log /tmp/mcp_system.log\n";
        StopMcpExampleServices();
        return false;
    }

    auto_started = true;
    std::cerr << "[info] MCP example services ready\n";
    return true;
}

// -----------------------------------------------------------------------------
// 全局子进程 PID（信号处理用）
// -----------------------------------------------------------------------------

volatile sig_atomic_t g_should_stop = 0;

void SignalHandler(int sig) {
    (void)sig;
    g_should_stop = 1;
}

// -----------------------------------------------------------------------------
// register speaker 顶层模式
// -----------------------------------------------------------------------------

int CmdRegisterSpeaker(const std::string& name, bool force) {
    if (!EnsureDefaultConfigs()) {
        return 1;
    }

    DaemonConfig cfg = omni_agent::LoadConfig();
    PrintConfigLoadErrors(cfg);

    int input_id = cfg.audio.input_device_id;
    std::vector<std::pair<int, std::string>> in_devs;
    {
        ScopedStderrSilencer silencer;
        in_devs = SpacemitAudio::AudioCapture::ListDevices();
    }
    if (!ResolveDevice("输入", cfg.audio.input_device_id,
            cfg.audio.input_device_hints, in_devs, input_id)) {
        return 1;
    }

    int register_channels = cfg.audio.capture_channels;
    int speech_channel = cfg.audio.speech_channel;
    int max_input_channels = 0;
    if (GetDeviceMaxChannels(true, input_id, max_input_channels) &&
            max_input_channels > 0 && register_channels > max_input_channels) {
        std::cerr << "[warn] 配置 capture_channels=" << register_channels
            << "，但输入设备最多只有 " << max_input_channels
            << " 路，声纹注册已自动降级\n";
        register_channels = max_input_channels;
    }
    if (speech_channel > register_channels) {
        std::cerr << "[warn] speech_channel=" << speech_channel
            << " 超出声纹注册录音声道数 " << register_channels
            << "，已改为 ch1\n";
        speech_channel = 1;
    }
    if (register_channels < 1) {
        std::cerr << "错误: 声纹注册录音声道数必须至少为 1，当前为 "
            << register_channels << "\n";
        return 1;
    }

    if (!MakeDirs(ParentDir(cfg.voiceprint.database))) {
        std::cerr << "错误: 无法创建声纹数据库目录 "
            << ParentDir(cfg.voiceprint.database) << "\n";
        return 1;
    }

    const int sample_rate = cfg.audio.capture_rate > 0 ? cfg.audio.capture_rate : 16000;
    std::vector<std::string> args = {
        "-n", name,
        "-d", cfg.voiceprint.database,
        "-t", std::to_string(cfg.voiceprint.threads),
        "-r", std::to_string(sample_rate),
        "-c", std::to_string(register_channels),
        "--speech-channel", std::to_string(speech_channel),
    };
    if (input_id >= 0) {
        args.push_back("-i");
        args.push_back(std::to_string(input_id));
    }
    if (force) {
        args.push_back("-f");
    }

    return RunSync("register_speaker", args);
}

// -----------------------------------------------------------------------------
// start 子命令
// -----------------------------------------------------------------------------

std::string JoinInts(const std::vector<int>& values, char sep) {
    std::ostringstream oss;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) oss << sep;
        oss << values[i];
    }
    return oss.str();
}

bool ParseStartOptions(int argc, char** argv,
        bool& aec_override, bool& mcp_override, int& doa_override) {
    aec_override = false;
    mcp_override = false;
    doa_override = -1;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--aec") {
            aec_override = true;
        } else if (a == "--mcp") {
            mcp_override = true;
        } else if (a == "--doa") {
            doa_override = 1;
        } else if (a == "--no-doa") {
            doa_override = 0;
        } else {
            std::cerr << "未知参数: " << a << "\n";
            return false;
        }
    }
    return true;
}

int CmdStart(int argc, char** argv) {
    bool aec_override = false;
    bool mcp_override = false;
    int doa_override = -1;
    if (!ParseStartOptions(argc, argv, aec_override, mcp_override, doa_override)) {
        return 2;
    }

    if (!EnsureDefaultConfigs()) {
        return 1;
    }

    DaemonConfig cfg = omni_agent::LoadConfig();
    PrintConfigLoadErrors(cfg);
    if (aec_override) {
        cfg.mode = "voice_chat_aec";
    }
    if (mcp_override) {
        cfg.mcp.enabled = true;
    }
    if (doa_override >= 0) {
        cfg.doa.enabled = doa_override != 0;
    }
    if (cfg.mode != "voice_chat" && cfg.mode != "voice_chat_aec") {
        std::cerr << "错误: voice_chat.json mode 非法: " << cfg.mode << "\n";
        return 1;
    }
    if (cfg.mode == "voice_chat" && cfg.aec_status.loaded) {
        std::cerr << "[info] aec.json ignored in voice_chat mode\n";
    }

    const std::string pid_file = cfg.pid_file;
    const std::string log_dir = cfg.log_dir;
    const std::string model_path = cfg.llm.model_path;

    // 1. PID 文件预检
    {
        PidRecord existing;
        if (ReadPidFile(pid_file, &existing)) {
            if (ProcessAlive(existing.daemon_pid)) {
                std::cerr << "voice_chat_daemon 已在运行 (pid="
                    << existing.daemon_pid << ")\n";
                std::cerr << "如需重启: voice_chat_daemon stop\n";
                return 1;
            }
            std::cerr << "[warn] 发现 stale PID 文件，清理: " << pid_file << "\n";
            unlink(pid_file.c_str());
        }
    }

    const bool start_local_llm = ShouldStartLocalLlm(cfg);
    const bool start_local_asr = ShouldStartLocalAsr(cfg);
    const std::string llm_url = EffectiveLlmUrl(cfg);
    const std::string asr_endpoint = EffectiveAsrEndpoint(cfg);
    if (!cfg.llm.api_base.empty()) {
        std::cerr << "[info] using external/cloud LLM API: " << llm_url << "\n";
    }
    if (IsQwen3Asr(cfg.asr.engine) && !start_local_asr) {
        std::cerr << "[info] using external Qwen3-ASR API: " << asr_endpoint << "\n";
    }

    // 2. llama-server 预检
    if (start_local_llm && !BinaryOnPath(cfg.llm.server_binary)) {
        std::cerr << "错误: 未找到 " << cfg.llm.server_binary << "\n";
        std::cerr << "安装方法:\n";
        std::cerr << "  sudo apt install llama.cpp-tools-spacemit\n";
        return 1;
    }
    if (start_local_asr && !BinaryOnPath(cfg.asr.server_binary)) {
        std::cerr << "错误: 未找到 " << cfg.asr.server_binary << "\n";
        std::cerr << "安装方法:\n";
        std::cerr << "  sudo apt install llama.cpp-tools-spacemit\n";
        return 1;
    }

    // 3. 端口预检
    if (start_local_llm && PortInUse(cfg.llm.server_host, cfg.llm.server_port)) {
        std::cerr << "错误: 端口 " << cfg.llm.server_port << " 已被占用\n";
        std::cerr << "排查: lsof -i :" << cfg.llm.server_port << "\n";
        return 1;
    }
    if (start_local_asr && PortInUse(cfg.asr.server_host, cfg.asr.server_port)) {
        std::cerr << "错误: 端口 " << cfg.asr.server_port << " 已被占用\n";
        std::cerr << "排查: lsof -i :" << cfg.asr.server_port << "\n";
        return 1;
    }

    // 4. 模型预检
    if (start_local_llm && !FileExists(model_path)) {
        std::cerr << "错误: 本地 LLM 模型不存在: " << model_path << "\n";
        std::cerr << "请先准备模型文件，或在 llm.json 配置云端 api_base/api_key。\n";
        std::cerr << "模型目录: " << ParentDir(model_path) << "\n";
        std::cerr << "参考地址: " << cfg.llm.model_url << "\n";
        return 1;
    }
    if (start_local_asr && !FileExists(cfg.asr.model_path)) {
        std::cerr << "错误: 本地 Qwen3-ASR 模型不存在: " << cfg.asr.model_path << "\n";
        std::cerr << "模型目录: " << ParentDir(cfg.asr.model_path) << "\n";
        std::cerr << "参考地址: " << cfg.asr.model_url << "\n";
        return 1;
    }
    if (start_local_asr && !DirExists(cfg.asr.smt_config_dir)) {
        std::cerr << "错误: Qwen3-ASR SMT 配置目录不存在: "
            << cfg.asr.smt_config_dir << "\n";
        std::cerr << "参考地址: " << cfg.asr.model_url << "\n";
        return 1;
    }
    if (!EnsureWakeAckAudio(cfg)) {
        return 1;
    }

    // 5. 音频设备解析
    int input_id = cfg.audio.input_device_id;
    int output_id = cfg.audio.output_device_id;
    std::vector<std::pair<int, std::string>> in_devs;
    std::vector<std::pair<int, std::string>> out_devs;
    {
        ScopedStderrSilencer silencer;
        in_devs = SpacemitAudio::AudioCapture::ListDevices();
        out_devs = SpacemitAudio::AudioPlayer::ListDevices();
    }

    if (!ResolveDevice("输入", cfg.audio.input_device_id,
            cfg.audio.input_device_hints, in_devs, input_id)) {
        return 1;
    }
    if (!ResolveDevice("输出", cfg.audio.output_device_id,
            cfg.audio.output_device_hints, out_devs, output_id)) {
        return 1;
    }
    std::cerr << "[info] 输入设备: "
        << (input_id >= 0 ? std::to_string(input_id) : std::string("系统默认"))
        << "  输出设备: "
        << (output_id >= 0 ? std::to_string(output_id) : std::string("系统默认"))
        << "\n";

    int max_input_channels = 0;
    if (GetDeviceMaxChannels(true, input_id, max_input_channels) &&
            max_input_channels > 0 &&
            cfg.audio.capture_channels > max_input_channels) {
        std::cerr << "[warn] 配置 capture_channels=" << cfg.audio.capture_channels
            << "，但输入设备最多只有 " << max_input_channels
            << " 路，已降级\n";
        cfg.audio.capture_channels = max_input_channels;
    }
    if (cfg.audio.speech_channel > cfg.audio.capture_channels) {
        std::cerr << "[warn] speech_channel=" << cfg.audio.speech_channel
            << " 超出录音声道数 " << cfg.audio.capture_channels
            << "，已改为 ch1\n";
        cfg.audio.speech_channel = 1;
    }
    if (cfg.doa.enabled && cfg.audio.capture_channels < 3) {
        std::cerr << "[warn] DOA 需要至少 3 路录音；当前只有 "
            << cfg.audio.capture_channels << " 路，已关闭 DOA\n";
        cfg.doa.enabled = false;
    }
    int max_output_channels = 0;
    if (GetDeviceMaxChannels(false, output_id, max_output_channels) &&
            max_output_channels > 0 &&
            cfg.audio.playback_channels > max_output_channels) {
        std::cerr << "[warn] 配置 playback_channels=" << cfg.audio.playback_channels
            << "，但输出设备最多只有 " << max_output_channels
            << " 路，已降级\n";
        cfg.audio.playback_channels = max_output_channels;
    }
    if (cfg.audio.playback_channels < 1) {
        std::cerr << "错误: playback_channels 必须至少为 1，当前为 "
            << cfg.audio.playback_channels << "\n";
        return 1;
    }

    const int capture_rate = cfg.audio.capture_rate > 0 ? cfg.audio.capture_rate : 16000;
    const int playback_rate = cfg.audio.playback_rate > 0 ? cfg.audio.playback_rate : 48000;
    int aec_sample_rate = 48000;
    if (cfg.mode == "voice_chat_aec") {
        if (cfg.audio.capture_rate > 0) {
            if (cfg.audio.capture_rate == 48000) {
                aec_sample_rate = cfg.audio.capture_rate;
            } else {
                std::cerr << "[warn] AEC mode requires 48000 Hz capture; ignoring "
                    << "voice_chat.json audio.capture_rate=" << cfg.audio.capture_rate
                    << "\n";
            }
        }
        std::cerr << "[info] AEC 采样率: " << aec_sample_rate << "\n";
    } else {
        std::cerr << "[info] 采样率: capture=" << capture_rate
            << "  playback=" << playback_rate << "\n";
    }

    if (cfg.voiceprint.enabled && !FileExists(cfg.voiceprint.database)) {
        std::cerr << "[warn] voiceprint enabled but database not found: "
            << cfg.voiceprint.database << "\n";
        std::cerr << "[warn] 请先运行 voice_chat_daemon --register-speaker NAME\n";
    }

    bool mcp_auto_started = false;
    if (!EnsureConfiguredMcpExampleServices(cfg, mcp_auto_started)) {
        return 1;
    }

    auto stop_mcp_if_started = [&]() {
        if (mcp_auto_started) {
            StopMcpExampleServices();
            mcp_auto_started = false;
        }
    };

    std::string mcp_config_path;
    if (!WriteMcpResolvedConfig(cfg, mcp_config_path)) {
        stop_mcp_if_started();
        return 1;
    }

    // 6. 日志目录
    if (!MakeDirs(log_dir)) {
        std::cerr << "错误: 无法创建日志目录 " << log_dir << "\n";
        stop_mcp_if_started();
        return 1;
    }
    std::string log_path = log_dir + "/voice_chat-" + Timestamp() + ".log";

    // 7. double-fork daemon
    int startup_pipe[2];
    if (pipe(startup_pipe) != 0) {
        std::perror("pipe");
        stop_mcp_if_started();
        return 1;
    }
    fcntl(startup_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(startup_pipe[1], F_SETFD, FD_CLOEXEC);

    pid_t p1 = fork();
    if (p1 < 0) {
        std::perror("fork");
        close(startup_pipe[0]);
        close(startup_pipe[1]);
        stop_mcp_if_started();
        return 1;
    }
    if (p1 > 0) {
        close(startup_pipe[1]);
        const int startup_wait_ticks = 100
            + (start_local_llm ? 700 : 0)
            + (start_local_asr ? (cfg.asr.startup_timeout + 10) * 10 : 0);
        bool startup_failed = false;
        for (int i = 0; i < startup_wait_ticks; ++i) {
            char status = 0;
            ssize_t n = read(startup_pipe[0], &status, 1);
            if (n == 1) {
                if (status != '1') {
                    startup_failed = true;
                }
                break;
            }
            if (n == 0) {
                startup_failed = true;
                break;
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                startup_failed = true;
                break;
            }
            usleep(100 * 1000);
        }
        close(startup_pipe[0]);
        PidRecord started;
        if (startup_failed || !ReadPidFile(pid_file, &started) ||
                !ProcessAlive(started.daemon_pid)) {
            std::cerr << "错误: voice_chat_daemon 启动失败，未生成有效 PID 文件\n";
            std::cerr << "  log: " << log_path << "\n";
            stop_mcp_if_started();
            return 1;
        }
        std::cout << "voice_chat_daemon started.\n";
        std::cout << "  mode: " << cfg.mode << "\n";
        std::cout << "  log:  " << log_path << "\n";
        std::cout << "  status: voice_chat_daemon status\n";
        return 0;
    }
    close(startup_pipe[0]);
    if (setsid() < 0) {
        WriteStartupStatus(startup_pipe[1], '0');
        _exit(1);
    }
    pid_t p2 = fork();
    if (p2 < 0) {
        WriteStartupStatus(startup_pipe[1], '0');
        _exit(1);
    }
    if (p2 > 0) {
        close(startup_pipe[1]);
        _exit(0);
    }
    if (chdir("/") != 0) {
        // daemon 不依赖 cwd。
    }
    umask(022);

    int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        WriteStartupStatus(startup_pipe[1], '0');
        _exit(1);
    }
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        close(devnull);
    }
    close(fd);

    setpgid(0, 0);

    // 8. 信号
    struct sigaction sa{};
    sa.sa_handler = SignalHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    // 9. 拉起 llama-server
    pid_t llama_pid = -1;
    if (start_local_llm) {
        std::vector<std::string> args = {
            "-m", model_path,
            "--host", cfg.llm.server_host,
            "--port", std::to_string(cfg.llm.server_port),
            "-c", std::to_string(cfg.llm.ctx_size),
            "-t", std::to_string(cfg.llm.threads),
            "--no-warmup",
        };
        if (cfg.llm.reasoning_budget >= 0) {
            args.push_back("--reasoning-budget");
            args.push_back(std::to_string(cfg.llm.reasoning_budget));
        }
        for (const auto& e : cfg.llm.extra_args) {
            args.push_back(e);
        }
        std::cerr << "[info] starting llama-server " << cfg.llm.model_path << "\n";
        llama_pid = SpawnAsync(cfg.llm.server_binary, args);
        if (llama_pid < 0) {
            std::cerr << "错误: 启动 llama-server 失败\n";
            WriteStartupStatus(startup_pipe[1], '0');
            stop_mcp_if_started();
            return 1;
        }
        if (!WaitPortReady(cfg.llm.server_host, cfg.llm.server_port, 60)) {
            std::cerr << "错误: llama-server 60 秒内未就绪 (port "
                << cfg.llm.server_port << ")\n";
            kill(llama_pid, SIGTERM);
            WriteStartupStatus(startup_pipe[1], '0');
            stop_mcp_if_started();
            return 1;
        }
        std::cerr << "[info] llama-server ready, pid=" << llama_pid << "\n";
    }

    pid_t asr_pid = -1;
    if (start_local_asr) {
        std::vector<std::string> args = {
            "-m", cfg.asr.model_path,
            "--media-backend", "smt",
            "--smt-config-dir", cfg.asr.smt_config_dir,
            "--host", cfg.asr.server_host,
            "--port", std::to_string(cfg.asr.server_port),
            "-c", std::to_string(cfg.asr.ctx_size),
            "-t", std::to_string(cfg.asr.threads),
        };
        for (const auto& e : cfg.asr.extra_args) {
            args.push_back(e);
        }
        std::cerr << "[info] starting qwen3-asr server " << cfg.asr.model_path << "\n";
        asr_pid = SpawnAsync(cfg.asr.server_binary, args);
        if (asr_pid < 0) {
            std::cerr << "错误: 启动 qwen3-asr server 失败\n";
            WriteStartupStatus(startup_pipe[1], '0');
            if (llama_pid > 0) {
                kill(llama_pid, SIGTERM);
            }
            stop_mcp_if_started();
            return 1;
        }
        if (!WaitHttpHealthReady(cfg.asr.server_host, cfg.asr.server_port,
                cfg.asr.startup_timeout)) {
            std::cerr << "错误: qwen3-asr server " << cfg.asr.startup_timeout
                << " 秒内未就绪 (port " << cfg.asr.server_port << ")\n";
            kill(asr_pid, SIGTERM);
            if (llama_pid > 0) {
                kill(llama_pid, SIGTERM);
            }
            WriteStartupStatus(startup_pipe[1], '0');
            stop_mcp_if_started();
            return 1;
        }
        std::cerr << "[info] qwen3-asr server ready, pid=" << asr_pid << "\n";
    }

    // 10. 拉起 voice_chat / voice_chat_aec
    std::vector<std::string> vc_args = {
        "--tts", cfg.tts,
        "--llm-url", llm_url,
        "--model", cfg.llm.model_name,
        "--vad-threshold", std::to_string(cfg.vad.threshold),
        "--silence-duration", std::to_string(cfg.vad.silence_duration),
        "--max-tokens", std::to_string(cfg.llm.max_tokens),
        "--system-prompt", cfg.llm.system_prompt,
    };
    if (cfg.llm.reasoning_budget >= 0) {
        vc_args.push_back("--reasoning-budget");
        vc_args.push_back(std::to_string(cfg.llm.reasoning_budget));
    }
    if (!cfg.startup_greeting.empty()) {
        vc_args.push_back("--startup-greeting");
        vc_args.push_back(cfg.startup_greeting);
    }
    if (cfg.mode == "voice_chat_aec") {
        vc_args.push_back("--sample-rate");
        vc_args.push_back(std::to_string(aec_sample_rate));
        vc_args.push_back("--capture-channels");
        vc_args.push_back(std::to_string(cfg.audio.capture_channels));
        vc_args.push_back("--playback-channels");
        vc_args.push_back(std::to_string(cfg.audio.playback_channels));
        if (cfg.aec.no_aec) {
            vc_args.push_back("--no-aec");
        }
        if (cfg.aec.no_ns) {
            vc_args.push_back("--no-ns");
        }
        if (cfg.aec.agc) {
            vc_args.push_back("--agc");
        }
        vc_args.push_back("--aec-delay");
        vc_args.push_back(std::to_string(cfg.aec.aec_delay_ms));
        if (cfg.aec.buffer_frames > 0) {
            vc_args.push_back("--buffer-frames");
            vc_args.push_back(std::to_string(cfg.aec.buffer_frames));
        }
    } else {
        vc_args.push_back("--capture-rate");
        vc_args.push_back(std::to_string(capture_rate));
        vc_args.push_back("--playback-rate");
        vc_args.push_back(std::to_string(playback_rate));
        vc_args.push_back("--capture-channels");
        vc_args.push_back(std::to_string(cfg.audio.capture_channels));
        vc_args.push_back("--playback-channels");
        vc_args.push_back(std::to_string(cfg.audio.playback_channels));
        vc_args.push_back(cfg.audio_frontend.enabled
            ? "--audio-frontend"
            : "--no-audio-frontend");
        vc_args.push_back(cfg.audio_frontend.highpass
            ? "--audio-frontend-hpf"
            : "--no-audio-frontend-hpf");
        vc_args.push_back(cfg.audio_frontend.noise_suppression
            ? "--audio-frontend-ns"
            : "--no-audio-frontend-ns");
        vc_args.push_back(cfg.audio_frontend.agc
            ? "--audio-frontend-agc"
            : "--no-audio-frontend-agc");
        vc_args.push_back("--audio-frontend-agc-target");
        vc_args.push_back(std::to_string(cfg.audio_frontend.agc_target_level_dbfs));
        vc_args.push_back("--audio-frontend-agc-gain");
        vc_args.push_back(std::to_string(cfg.audio_frontend.agc_compression_gain_db));
        vc_args.push_back(cfg.audio_frontend.agc_limiter
            ? "--audio-frontend-agc-limiter"
            : "--no-audio-frontend-agc-limiter");
    }
    vc_args.push_back("--speech-channel");
    vc_args.push_back(std::to_string(cfg.audio.speech_channel));
    vc_args.push_back("--asr-engine");
    vc_args.push_back(cfg.asr.engine);
    if (!asr_endpoint.empty()) {
        vc_args.push_back("--asr-endpoint");
        vc_args.push_back(asr_endpoint);
    }
    if (!cfg.asr.model.empty()) {
        vc_args.push_back("--asr-model");
        vc_args.push_back(cfg.asr.model);
    }
    vc_args.push_back("--asr-timeout");
    vc_args.push_back(std::to_string(cfg.asr.timeout));
    if (cfg.doa.enabled) {
        vc_args.push_back("--doa");
        if (!cfg.doa.pick.empty()) {
            vc_args.push_back("--doa-pick");
            vc_args.push_back(JoinInts(cfg.doa.pick, ','));
        }
        if (!cfg.doa.positions.empty()) {
            vc_args.push_back("--doa-positions");
            vc_args.push_back(cfg.doa.positions);
        } else {
            vc_args.push_back("--doa-side");
            vc_args.push_back(std::to_string(cfg.doa.side_m));
        }
        vc_args.push_back("--doa-azimuth-offset");
        vc_args.push_back(std::to_string(cfg.doa.azimuth_offset_deg));
        vc_args.push_back("--doa-max-avg-seconds");
        vc_args.push_back(std::to_string(cfg.doa.max_avg_seconds));
        vc_args.push_back("--doa-confidence-threshold");
        vc_args.push_back(std::to_string(cfg.doa.confidence_threshold));
        vc_args.push_back("--doa-margin-threshold");
        vc_args.push_back(std::to_string(cfg.doa.margin_threshold));
        vc_args.push_back("--doa-quality-threshold");
        vc_args.push_back(std::to_string(cfg.doa.quality_threshold));
        vc_args.push_back("--doa-min-signal-rms");
        vc_args.push_back(std::to_string(cfg.doa.min_signal_rms));
        vc_args.push_back("--doa-closure-threshold-samples");
        vc_args.push_back(std::to_string(cfg.doa.closure_threshold_samples));
        vc_args.push_back("--doa-closure-threshold-fraction");
        vc_args.push_back(std::to_string(cfg.doa.closure_threshold_fraction));
    }
    if (cfg.wake.enabled) {
        vc_args.push_back("--wake-enabled");
        vc_args.push_back("--wake-device");
        vc_args.push_back(cfg.wake.device);
        vc_args.push_back(cfg.wake.interrupt_mode
            ? "--wake-interrupt-mode"
            : "--no-wake-interrupt-mode");
        vc_args.push_back("--wake-ack-audio");
        vc_args.push_back(cfg.wake.ack_audio);
        vc_args.push_back(cfg.wake.drop_wake_asr
            ? "--wake-drop-asr"
            : "--no-wake-drop-asr");
        vc_args.push_back("--wake-drop-audio-ms");
        vc_args.push_back(std::to_string(cfg.wake.drop_audio_ms));
        vc_args.push_back("--wake-post-ack-tail-ms");
        vc_args.push_back(std::to_string(cfg.wake.post_ack_tail_ms));
    }
    if (input_id >= 0) {
        vc_args.push_back("-i");
        vc_args.push_back(std::to_string(input_id));
    }
    if (output_id >= 0) {
        vc_args.push_back("-o");
        vc_args.push_back(std::to_string(output_id));
    }
    if (cfg.debug.save_audio) {
        vc_args.push_back("--save-audio");
        vc_args.push_back(cfg.debug.save_audio_file);
    }
    if (cfg.debug.save_asr_audio) {
        vc_args.push_back("--save-asr-audio");
        vc_args.push_back(cfg.debug.save_asr_audio_file);
    }
    if (cfg.debug.save_tts_audio) {
        vc_args.push_back("--save-tts-audio");
        vc_args.push_back(cfg.debug.save_tts_audio_file);
    }
    if (cfg.voiceprint.enabled) {
        vc_args.push_back("-vp");
        vc_args.push_back("--vp-database");
        vc_args.push_back(cfg.voiceprint.database);
        vc_args.push_back("--vp-threads");
        vc_args.push_back(std::to_string(cfg.voiceprint.threads));
        vc_args.push_back("--vp-threshold");
        vc_args.push_back(std::to_string(cfg.voiceprint.threshold));
        vc_args.push_back("--vp-top");
        vc_args.push_back(std::to_string(cfg.voiceprint.top));
        if (!cfg.voiceprint.verify.empty()) {
            vc_args.push_back("--vp-verify");
            vc_args.push_back(cfg.voiceprint.verify);
        }
    }
    if (!mcp_config_path.empty()) {
        vc_args.push_back("--mcp-config");
        vc_args.push_back(mcp_config_path);
    }

    std::vector<std::pair<std::string, std::string>> vc_env;
    if (!cfg.llm.api_key.empty()) {
        vc_env.push_back({"OPENAI_API_KEY", cfg.llm.api_key});
    }

    pid_t voice_pid = SpawnAsyncWithEnv(cfg.mode, vc_args, vc_env);
    if (voice_pid < 0) {
        std::cerr << "错误: 启动 " << cfg.mode << " 失败\n";
        WriteStartupStatus(startup_pipe[1], '0');
        if (asr_pid > 0) {
            kill(asr_pid, SIGTERM);
        }
        if (llama_pid > 0) {
            kill(llama_pid, SIGTERM);
        }
        stop_mcp_if_started();
        return 1;
    }
    std::cerr << "[info] " << cfg.mode << " started, pid=" << voice_pid << "\n";

    auto stop_started_children = [&]() {
        if (voice_pid > 0) {
            kill(voice_pid, SIGTERM);
        }
        if (asr_pid > 0) {
            kill(asr_pid, SIGTERM);
        }
        if (llama_pid > 0) {
            kill(llama_pid, SIGTERM);
        }
        stop_mcp_if_started();
    };

    // 11. 写 PID 文件
    PidRecord rec;
    rec.daemon_pid = getpid();
    rec.llama_pid = llama_pid;
    rec.asr_pid = asr_pid;
    rec.voice_pid = voice_pid;
    rec.mode = cfg.mode;
    rec.log_path = log_path;
    if (!WritePidFile(pid_file, rec)) {
        std::cerr << "错误: 写 PID 文件失败 " << pid_file << "\n";
        WriteStartupStatus(startup_pipe[1], '0');
        stop_started_children();
        return 1;
    }

    int voice_start_status = 0;
    bool voice_exited_during_startup = false;
    for (int i = 0; i < 10; ++i) {
        pid_t dead = waitpid(voice_pid, &voice_start_status, WNOHANG);
        if (dead == 0) {
            usleep(100 * 1000);
            continue;
        }
        if (dead == voice_pid) {
            voice_exited_during_startup = true;
            voice_pid = -1;
            break;
        }
        if (dead < 0) {
            if (errno == EINTR) {
                --i;
                continue;
            }
            if (errno == ECHILD) {
                voice_exited_during_startup = true;
                voice_pid = -1;
            }
            break;
        }
    }
    if (voice_exited_during_startup) {
        std::cerr << "错误: " << cfg.mode << " 启动后立即退出"
            << " (status=" << voice_start_status << ")\n";
        std::cerr << "      完整错误见 log: " << log_path << "\n";
        WriteStartupStatus(startup_pipe[1], '0');
        stop_started_children();
        unlink(pid_file.c_str());
        return 1;
    }
    WriteStartupStatus(startup_pipe[1], '1');

    // 12. 主循环
    while (!g_should_stop) {
        int status = 0;
        pid_t dead = waitpid(-1, &status, 0);
        if (dead < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (dead == llama_pid) {
            std::cerr << "[warn] llama-server 退出 (status=" << status
                << ")，停止 daemon\n";
            llama_pid = -1;
            if (voice_pid > 0) {
                kill(voice_pid, SIGTERM);
            }
            if (asr_pid > 0) {
                kill(asr_pid, SIGTERM);
            }
            break;
        }
        if (dead == asr_pid) {
            std::cerr << "[warn] qwen3-asr server 退出 (status=" << status
                << ")，停止 daemon\n";
            asr_pid = -1;
            if (voice_pid > 0) {
                kill(voice_pid, SIGTERM);
            }
            if (llama_pid > 0) {
                kill(llama_pid, SIGTERM);
            }
            break;
        }
        if (dead == voice_pid) {
            std::cerr << "[warn] " << cfg.mode << " 退出 (status=" << status
                << ")，停止 daemon\n";
            std::cerr << "[hint] voice_chat 提前退出。如果是采样率不匹配 "
                "(Invalid sample rate)，编辑\n";
            std::cerr << "       ~/.config/omni_agent/voice_chat.json 设置：\n";
            std::cerr << "         \"audio\": { \"capture_rate\": 16000, "
                "\"playback_rate\": 16000 }\n";
            std::cerr << "       然后 voice_chat_daemon stop && start。\n";
            std::cerr << "       完整错误见 log: " << log_path << "\n";
            voice_pid = -1;
            if (asr_pid > 0) {
                kill(asr_pid, SIGTERM);
            }
            if (llama_pid > 0) {
                kill(llama_pid, SIGTERM);
            }
            break;
        }
    }

    // 13. 收尾
    std::cerr << "[info] daemon shutting down\n";
    if (voice_pid > 0) {
        kill(voice_pid, SIGTERM);
    }
    if (asr_pid > 0) {
        kill(asr_pid, SIGTERM);
    }
    if (llama_pid > 0) {
        kill(llama_pid, SIGTERM);
    }
    for (int i = 0; i < 20; ++i) {
        bool v = voice_pid > 0 && ProcessAlive(voice_pid);
        bool a = asr_pid > 0 && ProcessAlive(asr_pid);
        bool l = llama_pid > 0 && ProcessAlive(llama_pid);
        if (!v && !a && !l) {
            break;
        }
        usleep(250 * 1000);
    }
    if (voice_pid > 0 && ProcessAlive(voice_pid)) {
        kill(voice_pid, SIGKILL);
    }
    if (asr_pid > 0 && ProcessAlive(asr_pid)) {
        kill(asr_pid, SIGKILL);
    }
    if (llama_pid > 0 && ProcessAlive(llama_pid)) {
        kill(llama_pid, SIGKILL);
    }
    while (waitpid(-1, nullptr, WNOHANG) > 0) {
    }
    stop_mcp_if_started();
    unlink(pid_file.c_str());
    return 0;
}

// -----------------------------------------------------------------------------
// stop / status / logs / config
// -----------------------------------------------------------------------------

int CmdStop() {
    DaemonConfig cfg = omni_agent::LoadConfig();
    PrintConfigLoadErrors(cfg);
    std::string pid_file = cfg.pid_file;

    PidRecord rec;
    if (!ReadPidFile(pid_file, &rec)) {
        std::cout << "voice_chat_daemon not running.\n";
        std::cout << "如怀疑有残留进程，请检查: pgrep -af 'llama-server|voice_chat'\n";
        return 0;
    }
    if (!ProcessAlive(rec.daemon_pid)) {
        std::cout << "voice_chat_daemon not running (清理 stale PID 文件)\n";
        if (rec.llama_pid > 0 && ProcessAlive(rec.llama_pid)) {
            std::cout << "清理残留 llama-server pid=" << rec.llama_pid << "\n";
            kill(rec.llama_pid, SIGKILL);
        }
        if (rec.asr_pid > 0 && ProcessAlive(rec.asr_pid)) {
            std::cout << "清理残留 qwen3-asr server pid=" << rec.asr_pid << "\n";
            kill(rec.asr_pid, SIGKILL);
        }
        if (rec.voice_pid > 0 && ProcessAlive(rec.voice_pid)) {
            std::cout << "清理残留 voice_chat pid=" << rec.voice_pid << "\n";
            kill(rec.voice_pid, SIGKILL);
        }
        unlink(pid_file.c_str());
        return 0;
    }

    std::cout << "stopping voice_chat_daemon pid=" << rec.daemon_pid << " ..."
        << std::flush;
    kill(rec.daemon_pid, SIGTERM);
    killpg(rec.daemon_pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        if (!ProcessAlive(rec.daemon_pid)) {
            break;
        }
        usleep(250 * 1000);
    }
    if (ProcessAlive(rec.daemon_pid)) {
        std::cout << " (超时, 强制 SIGKILL)" << std::flush;
        kill(rec.daemon_pid, SIGKILL);
        killpg(rec.daemon_pid, SIGKILL);
        usleep(500 * 1000);
    }
    if (rec.llama_pid > 0 && ProcessAlive(rec.llama_pid)) {
        kill(rec.llama_pid, SIGKILL);
    }
    if (rec.asr_pid > 0 && ProcessAlive(rec.asr_pid)) {
        kill(rec.asr_pid, SIGKILL);
    }
    if (rec.voice_pid > 0 && ProcessAlive(rec.voice_pid)) {
        kill(rec.voice_pid, SIGKILL);
    }
    unlink(pid_file.c_str());
    std::cout << " OK\n";
    return 0;
}

int CmdRestart(int argc, char** argv) {
    bool aec_override = false;
    bool mcp_override = false;
    int doa_override = -1;
    if (!ParseStartOptions(argc, argv, aec_override, mcp_override, doa_override)) {
        return 2;
    }

    int rc = CmdStop();
    if (rc != 0) {
        return rc;
    }
    return CmdStart(argc, argv);
}

int CmdStatus() {
    DaemonConfig cfg = omni_agent::LoadConfig();
    PrintConfigLoadErrors(cfg);
    std::string pid_file = cfg.pid_file;

    PidRecord rec;
    if (!ReadPidFile(pid_file, &rec) || !ProcessAlive(rec.daemon_pid)) {
        std::cout << "voice_chat_daemon: not running\n";
        return 1;
    }
    std::cout << "voice_chat_daemon: running\n";
    std::cout << "  daemon pid: " << FormatPidStatus(rec.daemon_pid) << "\n";
    std::cout << "  llama  pid: " << FormatPidStatus(rec.llama_pid) << "\n";
    std::cout << "  asr    pid: " << FormatPidStatus(rec.asr_pid) << "\n";
    std::cout << "  voice  pid: " << FormatPidStatus(rec.voice_pid) << "\n";
    std::cout << "  mode:       " << rec.mode << "\n";
    std::cout << "  log:        " << rec.log_path << "\n";
    return 0;
}

int CmdLogs() {
    DaemonConfig cfg = omni_agent::LoadConfig();
    PrintConfigLoadErrors(cfg);
    std::string pid_file = cfg.pid_file;

    PidRecord rec;
    if (!ReadPidFile(pid_file, &rec) || rec.log_path.empty()) {
        std::cerr << "voice_chat_daemon: not running or no log path\n";
        return 1;
    }
    execlp("tail", "tail", "-n", "200", "-f", rec.log_path.c_str(),
        static_cast<char*>(nullptr));
    std::perror("execlp tail");
    return 127;
}

int CmdConfigInit(int argc, char** argv) {
    bool force = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--force") {
            force = true;
        } else {
            std::cerr << "未知参数: " << arg << "\n";
            return 2;
        }
    }

    if (force) {
        const std::string config_dir = omni_agent::DefaultUserConfigDir();
        if (DirExists(config_dir)) {
            const std::string backup_dir = config_dir + ".backup-" + Timestamp();
            int rc = RunSync("cp", {"-a", config_dir, backup_dir});
            if (rc != 0) {
                std::cerr << "错误: 备份现有配置失败 (rc=" << rc << "): "
                    << backup_dir << "\n";
                return 1;
            }
            std::cout << "已备份现有配置: " << backup_dir << "\n";
        }
    }

    auto results = omni_agent::WriteDefaultConfigs("", force);
    int rc = 0;
    for (const auto& result : results) {
        if (!result.error.empty()) {
            std::cerr << "错误: " << result.path << ": " << result.error << "\n";
            rc = 1;
        } else if (result.written && force) {
            std::cout << "已还原默认配置: " << result.path << "\n";
        } else if (result.written) {
            std::cout << "已写入默认配置: " << result.path << "\n";
        } else {
            std::cout << "配置已存在，未覆盖: " << result.path << "\n";
        }
    }
    return rc;
}

int CmdConfigShow() {
    DaemonConfig cfg = omni_agent::LoadConfig();
    PrintConfigLoadErrors(cfg);
    std::cout << omni_agent::DumpMergedConfig(cfg) << "\n";
    return 0;
}

void PrintUsage(const char* prog) {
    std::cout
        << "用法: " << prog << " <command> [options]\n"
        << "      " << prog << " --register-speaker NAME [--force]\n"
        << "\n命令:\n"
        << "  start [--aec] [--mcp] [--doa|--no-doa]\n"
        << "                  启动 omni_agent daemon\n"
        << "                  --aec  临时切换 voice_chat_aec\n"
        << "                  --mcp  临时启用 MCP client\n"
        << "                  --doa  临时启用 3 麦 DOA\n"
        << "                  --no-doa 临时关闭 DOA\n"
        << "  restart [--aec] [--mcp] [--doa|--no-doa]\n"
        << "                  重启 daemon，参数同 start\n"
        << "  stop            停止 daemon 及其所有子进程\n"
        << "  status          查看运行状态\n"
        << "  logs            tail -f 当前 log 文件\n"
        << "  config-init [--force]\n"
        << "                  写默认配置；--force 先备份再覆盖还原 5 个 JSON\n"
        << "  config-show     输出合并后的纯 JSON 配置\n"
        << "  -h, --help      显示帮助\n"
        << "\n顶层模式:\n"
        << "  --register-speaker NAME [--force]\n"
        << "                  直接进入声纹注册流程，完成后退出\n"
        << "\n示例:\n"
        << "  " << prog << " start                    # 一键启动\n"
        << "  " << prog << " start --aec              # AEC 模式启动\n"
        << "  " << prog << " start --doa              # 4路采集: ch1语音, ch2-4定位\n"
        << "  " << prog << " start --mcp              # 临时启用 MCP\n"
        << "  " << prog << " restart --mcp            # 重启并启用 MCP\n"
        << "  " << prog << " --register-speaker alice # 注册声纹\n"
        << "  " << prog << " stop                     # 关闭\n";
}

}  // namespace

int main(int argc, char** argv) {
    CurlGlobalRuntime curl_runtime;
    std::string curl_error;
    if (!curl_runtime.Init(&curl_error)) {
        std::cerr << "错误: " << curl_error << "\n";
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--register-speaker") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "--register-speaker 需要 NAME\n";
                return 2;
            }
            return CmdRegisterSpeaker(argv[i + 1], HasFlag(argc, argv, "--force"));
        }
    }

    if (argc < 2) {
        PrintUsage(argv[0]);
        return 0;
    }

    std::string cmd = argv[1];
    if (cmd == "start") {
        return CmdStart(argc, argv);
    }
    if (cmd == "restart") {
        return CmdRestart(argc, argv);
    }
    if (cmd == "stop") {
        return CmdStop();
    }
    if (cmd == "status") {
        return CmdStatus();
    }
    if (cmd == "logs") {
        return CmdLogs();
    }
    if (cmd == "config-init") {
        return CmdConfigInit(argc, argv);
    }
    if (cmd == "config-show") {
        return CmdConfigShow();
    }
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        PrintUsage(argv[0]);
        return 0;
    }

    std::cerr << "未知命令: " << cmd << "\n\n";
    PrintUsage(argv[0]);
    return 2;
}
