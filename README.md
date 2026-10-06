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

`config-init` 会在用户配置目录下写入缺失的 5 个 JSON，已有文件不会覆盖。需要还原默认配置时使用 `config-init --force`，daemon 会先把现有目录备份为 `<config_dir>.backup-<timestamp>`，再覆盖写入默认 JSON。任一文件不存在、字段缺失或字段为 `null` 时使用内置默认。`voice_chat.json` 的语法或唤醒词表非法时，`start` 会明确报错并退出，避免静默退回默认唤醒词；其他配置文件的加载错误仍会打印警告并使用对应默认值。

已经生成过旧版配置的用户，新默认字段不会自动写入已有文件。需要使用新默认模型、默认 MCP 三件套、启动问候或 TTS 音频保存字段时，可以运行 `voice_chat_daemon config-init --force` 后再按需修改；原配置会保留在备份目录中。

| 文件 | 职责 |
| --- | --- |
| `voice_chat.json` | 运行模式、音频设备、TTS、VAD、唤醒、启动问候、调试录音/TTS 音频、daemon 日志和 PID 路径 |
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

让会动设备的工具“先说再动”（`pre_tool_speech`，工具名 → 兜底播报）：

```json
{
    "enabled": true,
    "pre_tool_speech": {
        "perform_action": "好的，请注意安全。"
    },
    "servers": [
        {
            "name": "LingLong",
            "type": "stdio",
            "command": "humanoid_operator_mcp",
            "args": ["--robot", "linglong"]
        }
    ]
}
```

列出的工具调用前，先把模型这一轮的话播完；模型只给了工具调用、没有说话时，播这里配置的兜底句（配空字符串则不播）。播放结束后才调用工具；播放期间被唤醒词或 barge-in 打断时不调用，并在对话里记为“用户打断了，没有执行”。没列出的工具照旧立即调用，停止类工具（如 `stop_action`）不要列进来。

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
    "model_name": "deepseek-v4-flash",
    "extra_body": {"thinking": {"type": "disabled"}}
}
```

`extra_body` 会原样合并进每个 LLM 请求体，用于服务商专有参数（daemon 通过 `--llm-extra-body` 传给 `voice_chat`）。DeepSeek V4 默认开启推理，推理 token 计入 `max_tokens`：默认 150 时上下文稍复杂就会被推理用完，回复为空。语音场景建议像上面这样关闭推理；保留推理时把 `max_tokens` 调到 1024 以上。

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
| 切换 ASR 后端 | `voice_chat.json` 的 `asr.engine`，默认 `qwen3-asr`；qwen3-asr 还需 `asr.endpoint` / `asr.model` |
| 开启唤醒打断 | `voice_chat.json` 的 `wake.enabled` / `wake.source` / `wake.kws` / `wake.device` / `wake.interrupt_mode` / `wake.ack_audio` / `wake.ack_audio_url` |
| 配置唤醒词 ASR 清洗 | `voice_chat.json` 的 `wake.strip_from_asr` / `wake.phrases` / `wake.command_timeout_ms` |
| 修改或关闭启动问候 | `voice_chat.json` 的 `startup_greeting`；设为空字符串可关闭 |
| 保存调试录音、ASR 输入或 TTS 输出 | `voice_chat.json` 的 `debug.save_audio` / `debug.save_asr_audio` / `debug.save_tts_audio` |
| 保存逐帧对齐的 AEC/KWS 测试录音 | `voice_chat.json` 的 `debug.save_aec_dump` / `debug.aec_dump_dir`，见“唤醒与 AEC 测试录音” |
| 换模型、端口、线程数 | `llm.json` |
| 调回复长度、思考输出或系统提示词 | `llm.json` 的 `max_tokens` / `reasoning_budget` / `system_prompt` |
| 启用声纹验证 | `voiceprint.json` 的 `enabled` / `verify` |
| 启用 MCP 工具 | `mcp.json` 的 `enabled` / `servers` |
| 调 AEC 参数 | `aec.json`，仅 `mode=voice_chat_aec` 生效 |
| AEC 参考改用板端硬件回采 | `aec.json` 的 `reference_channel` / `reference_gain_db`，见“AEC 参考信号” |

### 唤醒来源

`wake.source` 选择唤醒事件从哪里来：

- `kws`：本机 KWS 模型（`components/model_zoo/kws`，cFSMN char-CTC，唤醒词由模型目录的 `keywords.txt` 决定）。
  只在 `voice_chat_aec` 模式可用：KWS 吃的是同一路 speech channel 经过独立一份 AEC3 的输出，不经过 NS/AGC，
  与模型训练数据一致；TTS 播放期间也能唤醒。参数在 `wake.kws`：`model_dir`（默认
  `~/.cache/models/kws/xiaojin-v1`）、`threshold`（默认 0.3）、`holdoff_ms`（两次唤醒最小间隔，默认 1000）、`partial_threshold`
  （快读或 TTS 播放中只解出半个唤醒词"小进"时的接受阈值，默认 0 关闭；0.9 时 ft05 的小姐/小杰误唤醒与关闭时相同）。
  `USE_AEC=ON` 时默认编入（`USE_KWS` 默认 ON，`kws` 组件经 `package.xml` 依赖先行构建）。`model_dir` 用默认目录时，
  首次启动由 kws（1.1.0 起）自动下载模型发布包 `xiaojin-v1` 并校验 SHA256，需要 `curl` 和网络，下载期间启动会等待
  （离线约 11 s 后失败）；`KWS_MODEL_DOWNLOAD=0` 关闭自动下载。其他目录须先放好模型，见 `components/model_zoo/kws`
  README 2.2。模型下载或加载失败时 `voice_chat_aec` 报错退出。

  `wake.kws.echo_null`（默认 false）打开 KWS 这一路的扬声器零陷：用 3 路裸麦（从 `echo_null_first_channel`
  起连续 3 路，1 起，默认 2 即 SPV 的第 2~4 路）按频点做空间滤波，把机器人自己喇叭方向的声音压掉后直接送 KWS
  （零陷学会之前仍用原来的单麦 AEC3 输出）。零陷之后不再过 AEC3：AEC3 仍按参考信号能量做抑制，会把播放中的人声一起压低，
  现场双讲测试里零陷后接 AEC3 只唤醒 5 次、不接 11 次，30 分钟纯 TTS 两者都是 0 次自触发。
  零陷权重在机器人播放 TTS 时在线学习（约 3 s 收敛），不需要标定，也不需要知道麦克风和喇叭的位置；喇叭挪动后
  会自动重学；只在播放期间及播放结束后 2 s 内生效，其余时间原样透传第一路零陷麦（晚 22 ms、不过 AEC3，所以 speech channel 应与 `echo_null_first_channel` 相同）。它不指向用户，所以不需要 DOA，除了正对喇叭的方向，任何方向的人声都能通过。只影响 KWS 这一路，
  ASR/VAD 和 DOA 不变；要求 16 kHz、10 ms 帧，KWS 路径多 22 ms 延迟，K3 上约占单核 1%。
  9/20 的 4 声道录音上，TTS 播放中 3 m 喊 10 次：单麦 4 次唤醒，零陷后 12 次；安静时都是 10/10，负样本都是 0。
  用 `echo_null_replay` 可以在 `aec_dump_raw.wav` + `aec_dump_ref.wav` 上离线复现线上的零陷输出。
- `hid`（默认，没写 `source` 的旧配置保持不变）：SPV 子板固件的板端唤醒，经 `wake.device`（默认 `/dev/hidraw0`）上报（固件黑盒，未单独评测）。

两种来源之后的处理完全相同（打断、提示音、回声保护、ASR 清洗、命令等待窗口），下文的“唤醒事件”对两者都适用。

### 唤醒响应与 ASR 清洗

启用唤醒后，`interrupt_mode=true` 时每次唤醒事件都会立即播放
`ack_audio`，无论设备正在播放 TTS 还是处于空闲状态。唤醒后的分段：

- 空闲时唤醒：包含唤醒事件的现有 utterance 保留，唤醒词由下文的词表清洗。
- 回复中唤醒（`voice_chat_aec`）：停止播放、打断回复并丢弃已录音频；正在播放 TTS 时，pre-buffer（最近 640 ms，是本机 TTS 的回声）也一并丢弃。下一段从 VAD 检测到用户说话开始，并带上唤醒之后的 pre-buffer；用户等提示音时不会多出一段只有静音的录音（ASR 会把这种段放大后误转写成“Okay.”之类）。
- 回复中唤醒（`voice_chat`，无 AEC）：唤醒时立即开始录音，因为提示音回声本身就会触发 VAD。
- 已知限制：回复进行中零停顿地说“小进小进 + 命令”时，命令与被打断的 TTS、提示音重叠，完整 AEC3 的抑制器会压低人声，命令可能丢失或误识别（2026-10-01 实测 2 次均失败；空闲时零停顿 4 次分段均正确）。回复中请等“我在”之后再说命令。

提示音之后是否丢弃一段音频由 `aec.json` 的 `wake_echo_guard` 决定，见“AEC 参考信号”。除词表清洗外，若首段 ASR 没有命中任何唤醒词、且唤醒之后没有连续 VAD 近端语音，该结果会按纯唤醒误转写丢弃；停顿后形成的下一段命令不受这条保护影响。

`wake.enabled=true` 不会改变原有的连续语音识别行为：IDLE 状态下由 VAD 截出的有效语音仍会送入 ASR。`wake_id` 只用于关联提示音、打断、首段保护和命令等待窗口，不作为 ASR 门禁，避免唤醒事件缺失或时序滞后时把用户语音全部丢弃。

唤醒词表用于清洗所有 ASR 结果的句首，句首连续重复的唤醒词会一并清除；命中纯唤醒词时不送入 LLM，命中“唤醒词 + 命令”时只把命令正文送入 LLM。例如：

```json
{
    "wake": {
        "enabled": true,
        "source": "kws",
        "kws": {
            "model_dir": "~/.cache/models/kws/xiaojin-v1",
            "threshold": 0.3,
            "holdoff_ms": 1000
        },
        "interrupt_mode": true,
        "ack_audio": "~/.cache/models/assets/audio/006_im_here.wav",
        "strip_from_asr": true,
        "command_timeout_ms": 5000,
        "phrases": [
            {
                "id": "xiaojin",
                "canonical": "小进小进",
                "asr_aliases": ["小金小金", "小静小静", "小鲸小鲸"]
            }
        ]
    }
}
```

- “小静小静，打开空调”会只把“打开空调”送给 LLM。
- 只有“小静小静”时不调用 LLM，并在 `command_timeout_ms` 内继续等待下一句话。
- 不含唤醒词表前缀的普通对话原样通过；是否关联唤醒事件不再决定这段语音能否识别。

这里的 `phrases` 只描述主机侧 ASR 文本的规范词和常见误识别，不会修改唤醒模型实际识别的词：`kws` 来源由模型目录决定，`hid` 来源由子板固件决定（HID 报告不携带命中的短语 ID）。

### 唤醒与 AEC 测试录音

排查“播放 TTS 时唤不醒/打断难”这类问题时，打开 `debug.save_aec_dump`，用正常的 daemon 启停即可，不需要额外脚本：

```json
{
    "debug": {
        "save_aec_dump": true,
        "aec_dump_dir": "~/.cache/omni_agent/aec_dumps"
    }
}
```

```bash
voice_chat_daemon start --aec    # 终端会打印本次录音目录
# ……测试……
voice_chat_daemon stop
```

每次启动在 `aec_dump_dir` 下新建一个与日志 `voice_chat-<时间戳>.log` 同名的子目录，里面是逐帧对齐的 16 bit WAV：

| 文件 | 内容 |
| --- | --- |
| `aec_dump_raw.wav` | 全部采集通道的原始信号（SPV 为 4 声道：第 1 路取决于固件，旧固件是板端处理结果、2026-09-28 起的固件是硬件回采；第 2~4 路裸麦），供零陷/多麦实验 |
| `aec_dump_in.wav` | speech channel 的麦克风原始信号 |
| `aec_dump_ref.wav` | 按延迟补偿对齐后的软件参考（写给喇叭的样本）；`reference_channel` 开启时 AEC 实际用的是 raw 里那一路乘增益 |
| `aec_dump_out.wav` | AEC+NS(+AGC) 之后、送 VAD/ASR 的信号 |
| `aec_dump_echo_only.wav` | 实际送 KWS 的信号：零陷学会之前是只过 AEC 的单麦，之后是零陷输出（仅 `wake.source=kws`） |
| `aec_dump_null.wav` | 扬声器零陷的输出（仅 `wake.kws.echo_null=true`，比其它轨晚 22 ms） |

录音边运行边写盘（SPV 4 声道时约 290 KB/s，每小时约 1 GB），每秒刷新一次 WAV 头，进程被杀也能读。只在 `voice_chat_aec` 模式生效；测完请关掉。
把 `aec_dump_echo_only.wav` 去掉 44 字节头后送给 KWS 组件的 `kws_stream_demo --stdin --no-beam`，得到的唤醒与在线完全一致，可用来离线复现漏唤醒。

### AEC 参考信号

默认（`aec.json` 的 `reference_channel: 0`）AEC3 的参考是写给喇叭的样本（软件回采），按 `aec_delay_ms` 和启动预热时的延迟跟踪对齐。
SPV 的 USB 端点是异步（sync type NONE），喇叭和麦克风不在同一个时钟上，参考到回声的延迟会慢慢漂移，AEC3 的线性滤波收敛不了，
主要靠抑制器，TTS 会有残留。

SPV 2026-09-28 起的固件把采集第 1 路改成了硬件回采，与麦克风同一时钟、延迟固定。此时设置：

```json
{
    "speech_channel": 2,
    "reference_channel": 1,
    "reference_gain_db": 12.0
}
```

AEC3 改用第 1 路乘 `reference_gain_db`（该固件的回采比麦克风低约 12 dB）作参考；延迟补偿只继续用于零陷的播放门控和调试录音里的软件参考。
同一段 18 分钟现场录音离线对比，完整 AEC3 的回声抑制从 6.5 dB 提到 22.4 dB。旧固件的第 1 路是板端处理结果，不能这样用。

唤醒后默认会丢掉约 1 s 音频（提示音“我在”及被打断 TTS 的回声，`aec.json` 的 `wake_echo_guard`）。
`auto`（默认）只在软件回采时丢；用硬件回采时不丢，紧跟唤醒词的命令不会被守护截掉。
硬件回采下 26 次提示音（两段现场录音）的 AEC 残留都没有触发 VAD（最高 0.51，阈值 0.8），在底噪附近；
40% 音量实测，大音量需要复测。`on` / `off` 可强制。

## 构建

```bash
source build/envsetup.sh
lunch k3-com260-omni-agent

cd application/native/omni_agent && mm

# 需要软件 AEC 时（默认含本机 KWS；kws 组件还没编过时用 mm --with-deps 先编依赖）
cd application/native/omni_agent && mm -DUSE_AEC=ON

# 软件 AEC 但不要本机 KWS 唤醒时
cd application/native/omni_agent && mm -DUSE_AEC=ON -DUSE_KWS=OFF

# 需要 voice_chat 的 WebRTC AGC/NS/HPF 前端时
cd application/native/omni_agent && mm -DUSE_AUDIO_FRONTEND=ON
```

| CMake 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `USE_MCP` | `ON` | MCP 工具调用支持 |
| `USE_AEC` | `OFF` | 编译 `voice_chat_aec` |
| `USE_AUDIO_FRONTEND` | `OFF` | 为 `voice_chat` 编译 WebRTC AGC/NS/HPF |
| `USE_VP` | `ON` | 声纹验证支持 |
| `USE_KWS` | `ON` | `voice_chat_aec` 的本机 KWS 唤醒（`wake.source=kws`），仅在 `USE_AEC=ON` 时生效 |

`USE_AEC=ON` 或 `USE_AUDIO_FRONTEND=ON` 会拉取并构建
`webrtc-audio-processing`，需要系统已安装 `meson` 和 `ninja-build`。

## 附录：底层调试工具

普通使用不需要直接调用这些工具；它们保留用于向后兼容和问题定位。

### voice_chat

```bash
voice_chat --llm-url http://127.0.0.1:9191/v1 --model qwen2.5-0.5b --tts matcha:zh-en
voice_chat -l
```

常用参数包括 `-i/-o`、`--capture-rate`、`--playback-rate`、`--vad-threshold`、`--silence-duration`、`--wake-enabled`、`--wake-source hid`、`--wake-device`、`--wake-interrupt-mode`、`--wake-ack-audio`、`--wake-strip-asr`、可重复的 `--wake-phrase`（一旦指定就替换内置词表，含 ASR 别名）、`--wake-command-timeout-ms`、`--max-tokens`、`--system-prompt`、`--mcp-config`、`-vp` 和 `--save-audio`。

### voice_chat_aec

```bash
voice_chat_aec --llm-url http://127.0.0.1:9191/v1 --sample-rate 48000
```

常用参数同 `voice_chat`，AEC 额外支持 `--no-aec`、`--no-ns`、`--agc`、`--aec-delay`、`--buffer-frames` 和 `--aec-dump-dir`；唤醒额外支持 `--wake-source kws`、`--kws-model-dir`、`--kws-threshold`、`--kws-holdoff-ms` 和 `--kws-partial-threshold`。

以下环境变量只用于现场调参和排查，默认值即推荐值，生产配置不需要设置：

| 环境变量 | 默认 | 作用 |
| --- | --- | --- |
| `WAKE_ACK_GAIN_DB` | `0` | 唤醒提示音增益（dB）。大音量下提示音回声削顶、AEC 消不掉时调低；`voice_chat` 没有 AEC，不读取 |
| `AEC_ECHO_TAIL_MS` | `0` | 回声守护在提示音之外再多丢的毫秒数（只在 `wake_echo_guard` 生效时） |
| `AEC_WARMUP_SECONDS` | `12` | 启动预热时长（秒），预热期间播放提示语/扫频让 AEC 跟上设备延迟爬升；`0` 关闭 |
| `AEC_WARMUP_WAV` | `~/.cache/models/assets/audio/027_system_loading.wav` | 预热用的语音，缺失时退回扫频 |
| `AEC_WARMUP_GAIN_DB` / `AEC_WARMUP_DBFS` | `0` / `-22` | 预热语音增益 / 扫频电平 |
| `AEC_WARMUP_ONCE` | `1` | `0` 时循环预热语音填满 `AEC_WARMUP_SECONDS` |
| `AEC_TRACK_SECONDS` | `15` | 启动后参考延迟跟踪持续的秒数，之后冻结 |
| `AEC_TRACK_DELAY` / `AEC_TRACK_MARGIN_MS` | 关 / `30` | 构造时就打开延迟跟踪 / 跟踪余量 |
| `AEC_DYNAMIC_DELAY` / `AEC_DYNAMIC_MARGIN_MS` | 关 / `-20` | 按设备上报的回路延迟动态对齐参考 / 余量 |
| `AEC_IDLE_DITHER_DBFS` | `-50` | 不播放时往喇叭输出里加的抖动噪声电平（dBFS），`0` 关闭 |
| `AEC_QUEUE_MAX` | `100` | 音频回调到处理线程的帧队列上限，满了丢帧（10–100000） |
| `AEC3_CFG` | 空 | 覆盖 WebRTC AEC3 参数，`key=value` 逗号分隔（`filter_len`、`init_len`、`default_delay` 等，见 `src/aec_duplex_processor.cpp`） |
| `AEC_DUMP_DIR` | 空 | 未配置 `debug.aec_dump_dir` 时的测试录音目录 |
| `AEC_DIAG` | 未设置 | 设置后每 2 s 打印一次队列深度、丢帧、每帧耗时和参考延迟 |

### text_chat

文字输入调试：和 `voice_chat` 走同一个对话流程（LLM → 分句 TTS → MCP 工具），用 stdin 代替麦克风和 ASR，用按音频时长计时的假播放器代替声卡，每轮合成的语音存成 wav。不占用音频设备，适合调 prompt、MCP 工具和“先说再动”的时序。

```bash
export OPENAI_API_KEY=...
text_chat --llm-url https://api.deepseek.com/v1 --model deepseek-flash \
    --mcp-config mcp.json --out-dir /tmp/text_chat
> 跟大家打个招呼
> /barge 1500 来个奥特曼光线    # 开始处理 1.5 s 后模拟用户打断
> /quit
```

`--mcp-config` 读的是 `voice_chat` 的 MCP 配置格式（daemon 生成的 `mcp_resolved.json` 就是这种），其中不支持 `null` 值。`--max-tokens` 默认 150，与 daemon 默认一致。

### ptt_chat 与机器人控制台的语音模式

`ptt_chat` 不开麦克风，录音由外部提供：stdin 收 `wav <path>`，ASR 识别后走同一个对话流程，用本机扬声器播放。适合展会等嘈杂场合，用手机按住说话代替麦克风阵列和 AEC。

```text
手机 ──HTTPS──> tools/ptt_web/ptt_server.py ──┬─ /voice/ 语音面板、录音 ──stdin──> ptt_chat ──> 扬声器 / MCP
                                              └─ 其余原样转发（含 WebSocket）──> 机器人控制台（humanoid_common operator_service）
```

| stdin 命令 | 作用 |
|------------|------|
| `wav <path>` | 识别并回答这段录音（16 位 PCM wav，任意采样率和声道数，内部转成 16 kHz 单声道） |
| `text <文字>` | 跳过识别直接回答 |
| `interrupt` | 停止播放，取消正在进行的回答（还没开始的动作不再执行），丢掉还没处理的录音 |
| `reset` | 清空对话记忆，当前回答结束后生效 |
| `quit` | 退出 |

事件（状态、识别结果、边生成边发的回答文字、准备调用 / 已调用的工具、最终回答）写到 `--event-fd`，每行一个 JSON，格式见 `examples/ptt_chat.cpp` 开头。`-o none` 用假播放器，不出声。

`tools/ptt_web/ptt_server.py`（只依赖 Python 标准库）给机器人控制台加语音模式并管理 `ptt_chat` 子进程。手机浏览器只在 HTTPS 页面里允许录音，而控制台是纯 HTTP，所以由本服务给它套一层 HTTPS：控制台网页在安全上下文里从 `/voice/panel.js` 加载语音面板，同一页里切换「控制 | 语音」（控制台那边的改动见 humanoid_common `clients/web`）。

```bash
export OPENAI_API_KEY=...
python3 tools/ptt_web/ptt_server.py --listen "[::]:8443" --tls-cert fullchain.pem --tls-key key.pem \
    --console 127.0.0.1:8765 --operator-connection ~/.local/state/humanoid-operator/linglong/connection.json \
    --redirect-listen "[::]:8442" --redirect-to https://<域名>:8443/ --chat-log /tmp/ptt_chat.log -- \
    ptt_chat --llm-url https://api.deepseek.com/v1 --model deepseek-flash --mcp-config mcp.json \
    --llm-extra-body '{"thinking":{"type":"disabled"}}' -o "SPV Composite" --playback-rate 16000 --playback-channels 2
```

- `/voice/` 以外的请求原样转发给 `--console`，请求头一字不改（控制台用 Host / Origin 做跨站检查）。控制台只给本机用的 `/api/v1/local-session`、`/pairing.svg` 不转发：经本服务转发时对端就是本机，转发出去等于把配对令牌发给局域网里任何人。
- `--console-overlay <目录>`：机器人上部署的控制台网页还没有「控制 | 语音」时，用目录里改好的文件代替返回，机器人上的文件不动。`base.sha256` 记着改之前那一版的哈希，控制台返回的内容和它一致才替换；对不上（控制台升级了）就原样转发，日志里提示一次。目录用 `tools/ptt_web/make_console_overlay.py <humanoid_common 仓库> <输出目录> [--patched 分支]` 生成：取改好的分支相对它和 main 分叉点改过的 `clients/web` 文件。
- 语音接口 `/voice/api/*` 要带控制台的配对令牌（请求头 `X-Operator-Token`，面板从同源 sessionStorage 取），和 `--operator-connection` 连接文件里的一致；重新配对后文件变了自动重读。
- `--redirect-listen` / `--redirect-to` 另开一个纯 HTTP 端口，所有请求 302 到本服务并保留查询参数：控制台的局域网 HTTP 页面点「语音」经这里跳到 HTTPS 版，令牌和 `mode=voice` 放在 `#` 片段里，不发给服务器。
- 一次只让一个人按住说话；按下即打断机器人。机器人回答某个人时，别人要等它说完（60 s 后不再保护）。
- 面板显示最近 30 轮对话（所有人的，别人的标「其他人」），回答边生成边显示，模型决定做动作时先显示「准备做「挥手」」，做了再换成结果。
- 面板根据控制台广播的状态显示现在能不能让机器人做动作：其他人握着控制权时只能聊天；语音做动作时显示是谁让它做的。语音动作由 MCP 自己申请控制权，控制台切到语音时会放掉本页的控制权。
- `--actions key=显示名,...`（和 `humanoid_operator_mcp --actions` 同一格式）列在面板上，告诉用户能让它做什么。
- 录音只保留最近 20 段，放在 `--upload-dir`（默认 `$XDG_RUNTIME_DIR/ptt_web`）。
- `--idle-reset <秒>` 空闲后清空对话记忆，避免上一位观众的对话带到下一位，面板上画一条分隔线；默认不清。
- 测试：`python3 tests/test_ptt_web.py`（假控制台 + 假 ptt_chat，不需要模型）。

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
