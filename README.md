# Omni Agent — 语音对话系统

SpacemiT 语音对话系统，集成 VAD + ASR + LLM + TTS + 声纹验证 + MCP 工具调用的端到端语音交互方案。

## 快速开始

`voice_chat_daemon` 是推荐的唯一用户入口，会自动匹配音频设备、启动本地 LLM、拉起 `voice_chat` 或 `voice_chat_aec`，并把日志写到 `~/.cache/omni_agent/logs/`。

```bash
sudo apt install llama.cpp-tools-spacemit

voice_chat_daemon start
voice_chat_daemon status
voice_chat_daemon logs
voice_chat_daemon stop
```

使用本地 LLM 时，需要先把 `llm.json.model_url` 对应的模型文件准备到 `llm.json.model_path`。不使用本地模型时，在 `llm.json` 配置 `api_base`、`api_key` 和 `model_name` 即可走云端 OpenAI-compatible API。

首次 `start` 或 `--register-speaker` 会自动在 `~/.config/omni_agent/` 写入缺失的默认 JSON，已有文件不会覆盖。需要定制时直接编辑对应文件：

```bash
${EDITOR:-vi} ~/.config/omni_agent/voice_chat.json
voice_chat_daemon stop && voice_chat_daemon start
```

查看合并后的有效配置：

```bash
voice_chat_daemon config-show
```

## 配置文件

`config-init` 会在 `~/.config/omni_agent/` 下写入缺失的 5 个 JSON，已有文件不会覆盖。需要还原默认配置时使用 `config-init --force`，daemon 会先把现有目录备份为 `~/.config/omni_agent.backup-<timestamp>`，再覆盖写入默认 JSON。任一文件不存在、字段缺失或字段为 `null` 时使用内置默认；任一 JSON 语法错误时该文件使用默认值，其他文件继续生效。

| 文件 | 职责 |
| --- | --- |
| `voice_chat.json` | 运行模式、音频设备、TTS、VAD、调试录音、daemon 日志和 PID 路径 |
| `llm.json` | 本地 llama-server 或云端 OpenAI-compatible LLM、模型名、密钥、`max_tokens`、`system_prompt` |
| `voiceprint.json` | 声纹验证开关、数据库、阈值、verify 目标 |
| `mcp.json` | MCP client 开关、registry 和 servers；servers schema 复用 `components/agent_tools/mcp/examples/configs/` |
| `aec.json` | `voice_chat_aec` 专用的 AEC/NS/AGC/delay/buffer 参数 |

详细字段说明见 `docs/zh/k3/04-AI与算法/4.5-Agent.md`。

## CLI 总览

```text
voice_chat_daemon start [--aec] [--mcp]
voice_chat_daemon stop
voice_chat_daemon status
voice_chat_daemon logs
voice_chat_daemon config-init [--force]
voice_chat_daemon config-show
voice_chat_daemon --register-speaker NAME [--force]
```

| 命令 | 说明 |
| --- | --- |
| `start [--aec] [--mcp]` | 启动 daemon；`--aec` 临时切到 `voice_chat_aec`，`--mcp` 临时启用 MCP |
| `stop` | 停止 daemon、llama-server 和 voice_chat 子进程 |
| `status` | 显示 daemon / llama / voice_chat PID 和当前日志路径 |
| `logs` | `tail -f` 当前日志 |
| `config-init [--force]` | 手动写入缺失的 5 个默认 JSON；`--force` 先备份再覆盖还原默认配置 |
| `config-show` | 输出纯 JSON：5 段合并配置 + `_meta.load_status` |
| `--register-speaker NAME [--force]` | 不启动 daemon，直接调用 `register_speaker` 进入 3 次录音注册流程 |

## 场景示例

启动软件 AEC：

```bash
voice_chat_daemon start --aec
```

临时启用 MCP：

```bash
voice_chat_daemon start --mcp
cat ~/.cache/omni_agent/mcp_resolved.json
```

如果 `mcp.json` 没有配置 `servers` 或 `registry_url`，`--mcp` 会自动使用固定目录 `~/.local/share/omni_agent/mcp/services` 启动内置 MCP 服务（Calculator、TimeService、SystemMonitor）。首次运行时 daemon 会从 SDK 的 `components/agent_tools/mcp/examples` 同步一份到该固定目录，之后从任何工作目录启动都不再依赖源码路径。MCP Python 环境固定为 `~/.mcp-env`，daemon 只检查依赖，不会在启动时安装 Python 包；请预先安装 `mcp starlette uvicorn psutil flask`。自定义 MCP 服务时，直接编辑 `mcp.json` 的 `servers` 或 `registry_url`。

MCP Python 环境准备示例：

```bash
python3 -m venv ~/.mcp-env
~/.mcp-env/bin/python -m pip install flask mcp starlette uvicorn psutil \
    --prefer-binary \
    --retries 0 \
    --timeout 2 \
    --index-url https://git.spacemit.com/api/v4/projects/33/packages/pypi/simple \
    --extra-index-url https://mirrors.aliyun.com/pypi/simple/
```

使用云端 OpenAI-compatible LLM：

```bash
${EDITOR:-vi} ~/.config/omni_agent/llm.json
voice_chat_daemon start
```

在 `llm.json` 中设置 `api_base`、`api_key` 和 `model_name`。`api_base` 非空时 daemon 不会启动本地 `llama-server`；`api_key` 会通过 `OPENAI_API_KEY` 传给 `voice_chat`，不会出现在命令行参数中。

DeepSeek 示例：

```json
{
    "api_base": "https://api.deepseek.com",
    "api_key": "sk-...",
    "model_name": "deepseek-v4-flash"
}
```

注册并启用声纹验证：

```bash
voice_chat_daemon --register-speaker alice
${EDITOR:-vi} ~/.config/omni_agent/voiceprint.json
voice_chat_daemon stop && voice_chat_daemon start
```

常用参数修改位置：

| 需求 | 修改位置 |
| --- | --- |
| 换音频设备或采样率 | `voice_chat.json` 的 `audio` |
| 调 VAD 灵敏度 | `voice_chat.json` 的 `vad.threshold` / `vad.silence_duration` |
| 换模型、端口、线程数 | `llm.json` |
| 调回复长度或系统提示词 | `llm.json` 的 `max_tokens` / `system_prompt` |
| 启用声纹验证 | `voiceprint.json` 的 `enabled` / `verify` |
| 启用 MCP 工具 | `mcp.json` 的 `enabled` / `servers` |
| 调 AEC 参数 | `aec.json`，仅 `mode=voice_chat_aec` 生效 |

## 构建

```bash
source build/envsetup.sh
lunch k3-com260-omni-agent

cd application/native/omni_agent && mm

# 需要软件 AEC 时
cd application/native/omni_agent && mm -DUSE_AEC=ON
```

| CMake 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `USE_MCP` | `ON` | MCP 工具调用支持 |
| `USE_AEC` | `OFF` | 编译 `voice_chat_aec` |
| `USE_VP` | `ON` | 声纹验证支持 |

## 附录：底层调试工具

普通使用不需要直接调用这些工具；它们保留用于向后兼容和问题定位。

### voice_chat

```bash
voice_chat --llm-url http://127.0.0.1:9191/v1 --model Qwen3-0.6B --tts matcha:zh-en
voice_chat -l
```

常用参数包括 `-i/-o`、`--capture-rate`、`--playback-rate`、`--vad-threshold`、`--silence-duration`、`--max-tokens`、`--system-prompt`、`--mcp-config`、`-vp` 和 `--save-audio`。

### voice_chat_aec

```bash
voice_chat_aec --llm-url http://127.0.0.1:9191/v1 --sample-rate 48000
```

AEC 专用参数包括 `--no-aec`、`--no-ns`、`--agc`、`--aec-delay`、`--buffer-frames`。

### 声纹工具

```bash
register_speaker -n alice -d ~/.cache/omni_agent/speakers.db
identify_speaker -d ~/.cache/omni_agent/speakers.db sample.wav
```

推荐注册入口仍是：

```bash
voice_chat_daemon --register-speaker alice
```

## License

Apache-2.0
