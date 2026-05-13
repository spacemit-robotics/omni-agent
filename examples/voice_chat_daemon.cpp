/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * voice_chat_daemon - omni_agent user-facing launcher
 *
 * 用户入口:
 *   start [--aec] [--mcp]
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

#include "audio_base.hpp"
#include "daemon_config.hpp"

namespace {

using omni_agent::DaemonConfig;
using json = nlohmann::json;

// -----------------------------------------------------------------------------
// 辅助函数
// -----------------------------------------------------------------------------

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
    pid_t daemon_pid = 0;
    pid_t llama_pid = 0;
    pid_t voice_pid = 0;
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
    f << "voice_chat=" << rec.voice_pid << "\n";
    f << "mode=" << rec.mode << "\n";
    f << "log=" << rec.log_path << "\n";
    return true;
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

bool StopMcpExampleServices() {
    std::string services_dir = EnsureMcpServicesDir();
    if (services_dir.empty()) {
        return false;
    }
    std::string script = services_dir + "/start_all_services.sh";
    RunSyncWithEnv(script, {"stop"}, {{"PATH", BuildMcpPythonPath()}});
    return true;
}

bool EnsureZeroConfigMcpServices(DaemonConfig& cfg, bool& auto_started) {
    auto_started = false;
    if (!cfg.mcp.enabled || !cfg.mcp.servers.empty() || !cfg.mcp.registry_url.empty()) {
        return true;
    }

    cfg.mcp.registry_url = "http://127.0.0.1:9000/mcp/services";
    if (McpExamplePortsReady()) {
        std::cerr << "[info] MCP services already running; using registry "
            << cfg.mcp.registry_url << "\n";
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
    std::cerr << "[info] starting MCP services: " << script << "\n";
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
    std::cerr << "[info] MCP services ready; using registry "
        << cfg.mcp.registry_url << "\n";
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
    auto in_devs = SpacemitAudio::AudioCapture::ListDevices();
    if (!ResolveDevice("输入", cfg.audio.input_device_id,
            cfg.audio.input_device_hints, in_devs, input_id)) {
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
        "-c", std::to_string(cfg.audio.capture_channels),
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

int CmdStart(int argc, char** argv) {
    bool aec_override = false;
    bool mcp_override = false;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--aec") {
            aec_override = true;
        } else if (a == "--mcp") {
            mcp_override = true;
        } else {
            std::cerr << "未知参数: " << a << "\n";
            return 2;
        }
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
    const std::string llm_url = EffectiveLlmUrl(cfg);
    if (!cfg.llm.api_base.empty()) {
        std::cerr << "[info] using external/cloud LLM API: " << llm_url << "\n";
    }

    // 2. llama-server 预检
    if (start_local_llm && !BinaryOnPath(cfg.llm.server_binary)) {
        std::cerr << "错误: 未找到 " << cfg.llm.server_binary << "\n";
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

    // 4. 模型预检
    if (start_local_llm && !FileExists(model_path)) {
        std::cerr << "错误: 本地 LLM 模型不存在: " << model_path << "\n";
        std::cerr << "请先准备模型文件，或在 llm.json 配置云端 api_base/api_key。\n";
        std::cerr << "模型目录: " << ParentDir(model_path) << "\n";
        std::cerr << "参考地址: " << cfg.llm.model_url << "\n";
        return 1;
    }

    // 5. 音频设备解析
    int input_id = cfg.audio.input_device_id;
    int output_id = cfg.audio.output_device_id;
    auto in_devs = SpacemitAudio::AudioCapture::ListDevices();
    auto out_devs = SpacemitAudio::AudioPlayer::ListDevices();

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

    const int capture_rate = cfg.audio.capture_rate > 0 ? cfg.audio.capture_rate : 16000;
    const int playback_rate = cfg.audio.playback_rate > 0 ? cfg.audio.playback_rate : 48000;
    int aec_sample_rate = 48000;
    const bool aec_sample_rate_set = cfg.audio.capture_rate > 0;
    if (aec_sample_rate_set) {
        if (cfg.audio.capture_rate == 48000) {
            aec_sample_rate = cfg.audio.capture_rate;
        } else {
            std::cerr << "[warn] AEC mode requires 48000 Hz capture; ignoring "
                << "voice_chat.json audio.capture_rate=" << cfg.audio.capture_rate
                << "\n";
        }
    }
    if (cfg.mode == "voice_chat_aec") {
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
    if (!EnsureZeroConfigMcpServices(cfg, mcp_auto_started)) {
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
        const int startup_wait_ticks = start_local_llm ? 700 : 100;
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
    if (cfg.mode == "voice_chat_aec") {
        if (aec_sample_rate_set && aec_sample_rate == 48000) {
            vc_args.push_back("--sample-rate");
            vc_args.push_back(std::to_string(aec_sample_rate));
        }
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
        if (llama_pid > 0) {
            kill(llama_pid, SIGTERM);
        }
        stop_mcp_if_started();
        return 1;
    }
    std::cerr << "[info] " << cfg.mode << " started, pid=" << voice_pid << "\n";

    // 11. 写 PID 文件
    PidRecord rec;
    rec.daemon_pid = getpid();
    rec.llama_pid = llama_pid;
    rec.voice_pid = voice_pid;
    rec.mode = cfg.mode;
    rec.log_path = log_path;
    if (!WritePidFile(pid_file, rec)) {
        std::cerr << "错误: 写 PID 文件失败 " << pid_file << "\n";
        WriteStartupStatus(startup_pipe[1], '0');
        if (voice_pid > 0) {
            kill(voice_pid, SIGTERM);
        }
        if (llama_pid > 0) {
            kill(llama_pid, SIGTERM);
        }
        stop_mcp_if_started();
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
    if (llama_pid > 0) {
        kill(llama_pid, SIGTERM);
    }
    for (int i = 0; i < 20; ++i) {
        bool v = voice_pid > 0 && ProcessAlive(voice_pid);
        bool l = llama_pid > 0 && ProcessAlive(llama_pid);
        if (!v && !l) {
            break;
        }
        usleep(250 * 1000);
    }
    if (voice_pid > 0 && ProcessAlive(voice_pid)) {
        kill(voice_pid, SIGKILL);
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
    if (rec.voice_pid > 0 && ProcessAlive(rec.voice_pid)) {
        kill(rec.voice_pid, SIGKILL);
    }
    unlink(pid_file.c_str());
    std::cout << " OK\n";
    return 0;
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
    std::cout << "  daemon pid: " << rec.daemon_pid << "\n";
    std::cout << "  llama  pid: " << rec.llama_pid
        << (ProcessAlive(rec.llama_pid) ? "" : " (DEAD)") << "\n";
    std::cout << "  voice  pid: " << rec.voice_pid
        << (ProcessAlive(rec.voice_pid) ? "" : " (DEAD)") << "\n";
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
        << "  start [--aec] [--mcp]\n"
        << "                  启动 omni_agent daemon\n"
        << "                  --aec  临时切换 voice_chat_aec\n"
        << "                  --mcp  临时启用 MCP client\n"
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
        << "  " << prog << " start --mcp              # 临时启用 MCP\n"
        << "  " << prog << " --register-speaker alice # 注册声纹\n"
        << "  " << prog << " stop                     # 关闭\n";
}

}  // namespace

int main(int argc, char** argv) {
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
