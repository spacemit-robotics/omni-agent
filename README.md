# Omni Agent — 语音对话系统

SpacemiT 语音对话系统，集成 VAD + ASR + LLM + TTS + 声纹验证 + MCP 工具调用的端到端语音交互方案。

## 快速开始

`voice_chat_daemon` 是推荐的唯一用户入口，会自动匹配音频设备、启动本地 LLM、拉起 `voice_chat` 或 `voice_chat_aec`，并把日志写到 `~/.cache/omni_agent/logs/`。daemon 不会自动下载 LLM 模型；首次启动前需要先准备本地模型，或者把 `llm.json` 改成云端 OpenAI-compatible API。

```bash
sudo apt install llama.cpp-tools-spacemit
```

使用默认本地 LLM 时：

```bash
mkdir -p ~/.cache/models/llm
wget -O ~/.cache/models/llm/qwen2.5-0.5b-instruct-q4_0.gguf \
    https://archive.spacemit.com/spacemit-ai/model_zoo/llm/qwen2.5-0.5b-instruct-q4_0.gguf
```

使用云端 LLM 时：

```bash
voice_chat_daemon config-init
vi ~/.config/omni_agent/llm.json
```

然后启动：

```bash
voice_chat_daemon start
voice_chat_daemon status
voice_chat_daemon logs
voice_chat_daemon stop
```

首次 `start` 或 `--register-speaker` 会自动在用户配置目录写入缺失的默认 JSON，已有文件不会覆盖。默认配置目录是 `~/.config/omni_agent/`；如果设置了 `XDG_CONFIG_HOME`，则使用 `$XDG_CONFIG_HOME/omni_agent/`。需要定制时直接编辑对应文件：

```bash
vi ~/.config/omni_agent/voice_chat.json
voice_chat_daemon restart
```

查看合并后的有效配置：

```bash
voice_chat_daemon config-show
```

## 配置文件

`config-init` 会在用户配置目录下写入缺失的 5 个 JSON，已有文件不会覆盖。需要还原默认配置时使用 `config-init --force`，daemon 会先把现有目录备份为 `<config_dir>.backup-<timestamp>`，再覆盖写入默认 JSON。任一文件不存在、字段缺失或字段为 `null` 时使用内置默认；任一 JSON 语法错误时该文件使用默认值，其他文件继续生效。

已经生成过旧版配置的用户，新默认字段不会自动写入已有文件。需要使用新默认模型、默认 MCP 三件套、启动问候或 TTS 音频保存字段时，可以运行 `voice_chat_daemon config-init --force` 后再按需修改；原配置会保留在备份目录中。

| 文件 | 职责 |
| --- | --- |
| `voice_chat.json` | 运行模式、音频设备、TTS、VAD、启动问候、调试录音/TTS 音频、daemon 日志和 PID 路径 |
| `llm.json` | 本地 llama-server 或云端 OpenAI-compatible LLM、模型名、密钥、`max_tokens`、`reasoning_budget`、`system_prompt` |
| `voiceprint.json` | 声纹验证开关、数据库、阈值、verify 目标 |
| `mcp.json` | MCP client 开关、registry 和 servers；servers schema 复用 `components/agent_tools/mcp/examples/configs/` |
| `aec.json` | `voice_chat_aec` 专用的 AEC/NS/AGC/delay/buffer 参数 |

详细字段说明见 `docs/zh/k3/04-AI与算法/4.5-Agent.md`。

## CLI 总览

```text
voice_chat_daemon start [--aec] [--mcp]
voice_chat_daemon restart [--aec] [--mcp]
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
| `restart [--aec] [--mcp]` | 先停止再启动 daemon，参数同 `start` |
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

默认 `mcp.json` 会显式写入三个 HTTP 示例服务（Calculator、TimeService、SystemMonitor）。`voice_chat_daemon start --mcp` 看到这些默认服务时，会使用固定目录 `~/.local/share/omni_agent/mcp/services` 启动对应后端；首次运行会从 SDK 的 `components/agent_tools/mcp/examples` 同步一份到该固定目录，之后从任何工作目录启动都不再依赖源码路径。首次同步要求当前 SDK 树能找到该目录；如果只拷贝了二进制或源码不在默认位置，需要设置 `OMNI_AGENT_MCP_EXAMPLES_DIR` 指向 `components/agent_tools/mcp/examples`。默认示例服务会使用本机端口 `8001`、`8002`、`8003` 和 registry 端口 `9000`。MCP Python 环境固定为 `~/.mcp-env`，daemon 只检查依赖，不会在启动时安装 Python 包；请预先安装 `mcp starlette uvicorn psutil flask`。

`--mcp` 只临时启用 MCP，不会回写 `~/.config/omni_agent/mcp.json`。实际传给 `voice_chat` 的运行时配置会写到 `~/.cache/omni_agent/mcp_resolved.json`。自定义 MCP 服务时，直接编辑 `mcp.json` 的 `servers` 或 `registry_url`；可以保留默认三件套并追加自己的服务，也可以删除默认三件套后只接入自己的服务。

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

接入 mlink HTTP MCP 服务示例：

```json
{
    "enabled": true,
    "backend": "llama",
    "system_prompt": null,
    "timeout": 120,
    "registry_url": null,
    "registry_poll_interval": 5,
    "servers": [
        {
            "name": "mlink-gateway",
            "type": "http",
            "url": "http://127.0.0.1:18765/mcp"
        }
    ]
}
```

使用云端 OpenAI-compatible LLM：

```bash
vi ~/.config/omni_agent/llm.json
voice_chat_daemon start
```

在 `llm.json` 中设置 `api_base`、`api_key` 和 `model_name`。`api_base` 非空时 daemon 不会启动本地 `llama-server`；`api_key` 会通过 `OPENAI_API_KEY` 传给 `voice_chat`，不会出现在命令行参数中。启用 MCP 时，云端模型还需要支持 OpenAI tool calling；普通聊天可用不代表 `start --mcp` 一定可用。

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
vi ~/.config/omni_agent/voiceprint.json
voice_chat_daemon restart
```

常用参数修改位置：

| 需求 | 修改位置 |
| --- | --- |
| 换音频设备或采样率 | `voice_chat.json` 的 `audio` |
| 调 VAD 灵敏度 | `voice_chat.json` 的 `vad.threshold` / `vad.silence_duration` |
| 修改或关闭启动问候 | `voice_chat.json` 的 `startup_greeting`；设为空字符串可关闭 |
| 保存调试录音或 TTS 输出 | `voice_chat.json` 的 `debug.save_audio` / `debug.save_tts_audio` |
| 换模型、端口、线程数 | `llm.json` |
| 调回复长度、思考输出或系统提示词 | `llm.json` 的 `max_tokens` / `reasoning_budget` / `system_prompt` |
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
voice_chat --llm-url http://127.0.0.1:9191/v1 --model qwen2.5-0.5b --tts matcha:zh-en
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
