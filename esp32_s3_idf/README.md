# ESP32-S3 ESP-IDF AI Agent

这个工程会让 ESP32-S3 连接 Wi-Fi，访问电脑上的 FastAPI 服务：

```text
串口输入文字 -> /chat -> Ollama 本地模型 -> 返回文字 -> /tts -> 返回 WAV -> MAX98357A 播放
```

麦克风 `ICS43434` 的 I2S RX 配置也已经预留，但默认关闭。等模块到了以后再开启并接入 ASR。

## 电脑端

在项目根目录启动 FastAPI，必须监听 `0.0.0.0`：

```powershell
cd C:\Users\ASUS\Desktop\esp_ai_agent
python -m uvicorn server:app --host 0.0.0.0 --port 8000
```

电脑端现在有两个主要接口：

```text
POST /chat  JSON {"text":"你好"} -> JSON {"reply":"..."}
POST /tts   JSON {"text":"你好"} -> audio/wav
```

`/tts` 使用 Windows 自带 `System.Speech` 生成 `16 kHz / 16-bit / mono` WAV，不需要额外安装 TTS 模型。

## ESP-IDF 编译烧录

打开 ESP-IDF 终端：

```powershell
cd C:\Users\ASUS\Desktop\esp_ai_agent\esp32_s3_idf
idf.py set-target esp32s3
idf.py menuconfig
```

在 `menuconfig` 里进入 `ESP AI Agent`，配置：

```text
Wi-Fi SSID
Wi-Fi password
Chat server URL，例如 http://10.193.55.146:8000/chat
```

然后烧录：

```powershell
idf.py -p COM3 flash monitor
```

把 `COM3` 换成你的开发板串口。

## 默认接线

先接 MAX98357A，就能测试“AI 说话”。默认引脚如下：

```text
MAX98357A        ESP32-S3
VIN/VCC          5V 或 3V3
GND              GND
BCLK             GPIO4
LRC / WS         GPIO5
DIN              GPIO6
SD               可悬空或接 3V3，按你的模块说明来
喇叭 + / -       接喇叭两端，不要把喇叭负极接 GND
```

ICS43434 麦克风默认预留引脚：

```text
ICS43434         ESP32-S3
VDD              3V3
GND              GND
SCK / BCLK       GPIO7
WS / LRCLK       GPIO15
SD               GPIO16
L/R              GND，选择左声道
```

如果你的开发板有屏幕、摄像头、PSRAM 特殊占脚，后面可以在 `menuconfig` 里改这些 GPIO。

## 现在能做的测试

硬件没到之前，可以继续用串口测试 `/chat`。接上 MAX98357A 后，串口输入一句话，ESP32-S3 会：

```text
1. POST /chat
2. 串口打印 AI 回复
3. POST /tts
4. 播放电脑返回的 WAV
```

麦克风到了以后，再把 `ESP AI Agent -> Enable ICS43434 microphone input` 打开。下一步会把录音上传到电脑端 ASR，再进入 `/chat`。

## 常见问题

如果 ESP32-S3 连不上电脑：

```text
Chat server URL 不能写 127.0.0.1
FastAPI 要用 --host 0.0.0.0 启动
Windows 防火墙要允许 8000 端口
ESP32-S3 和电脑要在同一个 Wi-Fi/局域网
```

如果没有声音：

```text
先确认串口里出现 TTS HTTP status=200
确认 MAX98357A 供电和 GND
确认 BCLK/WS/DIN 引脚和 menuconfig 一致
确认喇叭接在 MAX98357A 的喇叭输出端，而不是接 ESP32-S3 GPIO
```
