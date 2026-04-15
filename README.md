# ESP32_S3_AIagent

这是一个本地语音 AI 终端项目，由两部分组成：

- Windows 主机端：运行 `FastAPI`、`Ollama`、ASR 和 TTS
- `ESP32-S3` 设备端：负责联网、录音、按键控制和扬声器播放

现在这套代码已经不只是一个最小 demo，而是一套能跑通“文字聊天 + 语音对话”的本地局域网方案，目标形态接近手持或桌面陪伴式 AI 设备。

## 项目概览

电脑端是“大脑”和语音服务：

- `server.py` 提供 `/chat`、`/tts`、`/voice_chat` 等接口
- 通过本地 `Ollama` 模型生成回复
- TTS 支持 `Windows SAPI`、`Edge TTS`、`Piper`
- ASR 当前使用 Windows 语音识别

ESP32-S3 是“联网音频终端”：

- 可以通过串口输入文本发起聊天
- 可以通过按住按键录制麦克风语音
- 可以接收主机返回的 WAV 并通过 I2S 播放
- 可以用板载 `WS2812` 指示灯显示状态

## 主要流程

文字聊天流程：

1. 用户通过 ESP32 串口输入文本
2. ESP32 发起 `POST /chat`
3. 主机端调用 `Ollama` 生成回复
4. ESP32 继续发起 `POST /tts`
5. 主机端返回 WAV 音频
6. ESP32 通过 `MAX98357A` 播放语音

语音聊天流程：

1. 用户按住 push-to-talk 按键
2. ESP32 从 `ICS43434` 麦克风录音
3. ESP32 将 WAV 上传到 `POST /voice_chat`
4. 主机端先做 ASR，再调用 `Ollama`
5. ESP32 将回复文本发送到 `POST /tts`
6. 主机端返回 WAV，ESP32 播放语音

## 目录结构

```text
.
|- server.py                     FastAPI 主服务
|- start_windows_server.ps1      使用 Windows TTS 启动服务
|- start_edge_server.ps1         使用 Edge TTS 启动服务
|- start_piper_server.ps1        使用 Piper TTS 启动服务
|- tts_windows.ps1               Windows SAPI WAV 生成脚本
|- asr_windows.ps1               Windows 语音识别脚本
|- g2pW/                         g2p 相关元数据和本地运行文件
|- models/piper/                 Piper 模型元数据和本地模型文件
`- esp32_s3_idf/                 ESP-IDF 固件工程
```

固件相关的接线、菜单配置和更细的说明见 [esp32_s3_idf/README.md](esp32_s3_idf/README.md)。

## 主机端依赖

当前仓库里的 PowerShell 启动脚本默认假设你在 Windows 上运行，并且具备：

- 本地 Python 环境 `.venv-piper\Scripts\python.exe`
- `fastapi`、`uvicorn`、`requests`、`pydantic`
- 本地运行中的 `Ollama`，地址为 `http://127.0.0.1:11434`
- 所选 TTS 后端及其对应资源

`server.py` 当前默认模型是 `qwen3.5:4b`。

## 主机端快速开始

1. 确认已经安装 Ollama，并拉取默认模型：

```powershell
ollama pull qwen3.5:4b
```

2. 在项目根目录选择一种 TTS 模式启动服务：

```powershell
.\start_windows_server.ps1
```

```powershell
.\start_edge_server.ps1
```

```powershell
.\start_piper_server.ps1
```

以上脚本都会启动：

```text
http://0.0.0.0:8000
```

3. 检查服务状态：

```powershell
Invoke-RestMethod http://127.0.0.1:8000/tts_status
```

4. 做一次文字聊天自测：

```powershell
Invoke-RestMethod `
  -Uri http://127.0.0.1:8000/chat `
  -Method Post `
  -ContentType "application/json" `
  -Body '{"text":"你好"}'
```

## 编译与烧录 ESP32-S3 固件

在 `ESP-IDF` 终端中执行：

```powershell
cd .\esp32_s3_idf
idf.py set-target esp32s3
idf.py menuconfig
```

在 `ESP AI Agent` 菜单中设置：

- Wi-Fi SSID
- Wi-Fi 密码
- Chat server URL，例如 `http://192.168.x.x:8000/chat`

然后编译并烧录：

```powershell
idf.py -p COM3 flash monitor
```

把 `COM3` 换成你实际使用的串口号。

## 默认硬件映射

扬声器路径：

- `MAX98357A BCLK` -> `GPIO4`
- `MAX98357A WS` -> `GPIO5`
- `MAX98357A DIN` -> `GPIO6`

麦克风路径：

- `ICS43434 BCLK` -> `GPIO7`
- `ICS43434 WS` -> `GPIO15`
- `ICS43434 SD` -> `GPIO16`

控制与指示：

- push-to-talk 按键 -> `GPIO18`，低电平有效
- 板载 `WS2812` 状态灯 -> `GPIO48`

更详细的接线说明见 [esp32_s3_idf/README.md](esp32_s3_idf/README.md)。

## API 接口

`server.py` 提供的核心接口：

- `POST /chat`：输入文字，返回回复文本
- `POST /tts`：输入文字，返回 `audio/wav`
- `POST /voice_chat`：输入 WAV 音频，返回识别文本和回复文本

辅助接口：

- `GET /tts_status`
- `GET /tts_voices`
- `GET /edge_voices`
- `GET /windows_voices`
- `GET /chat_memory`
- `POST /chat_memory/reset`
- `POST /tts_voice`
- `POST /tts_volume`

## 仓库里故意没有提交的文件

为了避免把本地环境、构建产物和超大模型文件直接塞进 Git，这个仓库默认忽略了这些内容：

- `.venv-piper/`
- `esp32_s3_idf/build/`
- `esp32_s3_idf/sdkconfig`
- `esp32_s3_idf/sdkconfig.old`
- `g2pW/g2pw.onnx`
- `models/piper/*.onnx`

这意味着一个全新 clone 下来的仓库会包含代码和元数据，但不会包含本地运行环境和 ONNX 模型权重。如果你需要使用 `Piper` 或 `g2pW` 相关能力，需要把这些模型文件放回对应目录。

## 当前进度

现在这套代码已经具备：

- ESP32 串口文字聊天
- push-to-talk 语音录制
- Windows 语音识别
- Windows / Edge / Piper 三种 TTS
- ESP32 端流式 WAV 播放

一句话总结：这是一个已经能跑通的“Windows 主机 + ESP32-S3 终端”本地语音助手项目。
