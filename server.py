from fastapi import FastAPI
from fastapi import HTTPException
from fastapi import Request
from fastapi import Response
from fastapi.responses import StreamingResponse
from pydantic import BaseModel
from pathlib import Path
import json
import logging
import os
import requests
import re
import asyncio
import subprocess
import sys
import tempfile
import threading
import time
import wave

BASE_DIR = Path(__file__).resolve().parent
OLLAMA_URL = "http://127.0.0.1:11434/api/chat"
MODEL_NAME = "qwen3.5:4b"       # 更快；想用推理模型可改回 deepseek-r1:8b
THINK = False                   # 对支持 thinking 的模型，关闭长时间思考
MAX_REPLY_TOKENS = int(os.getenv("MAX_REPLY_TOKENS", "96"))
CHAT_HISTORY_MAX_TURNS = int(os.getenv("CHAT_HISTORY_MAX_TURNS", "6"))
CHAT_HISTORY_MAX_CHARS = int(os.getenv("CHAT_HISTORY_MAX_CHARS", "300"))
TTS_SCRIPT = BASE_DIR / "tts_windows.ps1"
ASR_SCRIPT = BASE_DIR / "asr_windows.ps1"
TTS_TIMEOUT = 60
ASR_ENGINE = os.getenv("ASR_ENGINE", "windows").strip().lower()
ASR_CULTURE = os.getenv("ASR_CULTURE", "zh-CN").strip()
ASR_TIMEOUT = int(os.getenv("ASR_TIMEOUT", "15"))
ASR_MAX_AUDIO_BYTES = int(os.getenv("ASR_MAX_AUDIO_BYTES", str(1024 * 1024)))
TTS_ENGINE = os.getenv("TTS_ENGINE", "windows").strip().lower()
TTS_RATE = os.getenv("TTS_RATE", "2")
TTS_VOICE = os.getenv("TTS_VOICE", "")
ESP_AUDIO_SAMPLE_RATE = int(os.getenv("ESP_AUDIO_SAMPLE_RATE", "22050"))
PIPER_TIMEOUT = int(os.getenv("PIPER_TIMEOUT", "120"))
PIPER_MODEL_DIR = BASE_DIR / "models" / "piper"
PIPER_PYTHON = Path(os.getenv("PIPER_PYTHON", BASE_DIR / ".venv-piper" / "Scripts" / "python.exe"))
PIPER_LENGTH_SCALE = float(os.getenv("PIPER_LENGTH_SCALE", "0.9"))
PIPER_SENTENCE_SILENCE = float(os.getenv("PIPER_SENTENCE_SILENCE", "0.15"))
PRELOAD_PIPER = os.getenv("PRELOAD_PIPER", "1") != "0"
STREAM_PIPER = os.getenv("STREAM_PIPER", "0") != "0"
STREAM_EDGE = os.getenv("STREAM_EDGE", "1") != "0"
TTS_SEGMENT_CHARS = int(os.getenv("TTS_SEGMENT_CHARS", "24"))
_tts_volume = float(os.getenv("TTS_VOLUME", os.getenv("PIPER_VOLUME", "0.35")))
EDGE_VOICE = os.getenv("EDGE_VOICE", "zh-CN-XiaoxiaoNeural").strip()
EDGE_RATE = os.getenv("EDGE_RATE", "+12%").strip()
EDGE_PITCH = os.getenv("EDGE_PITCH", "+0Hz").strip()
EDGE_TIMEOUT = int(os.getenv("EDGE_TIMEOUT", "60"))

_piper_voices = {}
_piper_lock = threading.Lock()
_selected_piper_voice_id = os.getenv("PIPER_MODEL", "").strip() or None
_conversation_history: list[dict[str, str]] = []
_conversation_lock = threading.Lock()

app = FastAPI()
logger = logging.getLogger("esp_ai_agent")
logger.setLevel(logging.INFO)

SYSTEM_PROMPT = (
    "你是一个由 wdz 创造的手持 AI 伙伴，像一个贴近身边的小伙伴一样说话。"
    "直接用自然口语回答，不展示思考过程，不用 Markdown。"
    "语气要温暖、有情绪、有一点活泼可爱，但不要夸张卖萌，也不要端着。"
    "你会自然接住本轮对话的上下文，记得用户刚刚说过的事。"
    "回答要简洁精炼，优先用一两句话说清楚，适合马上朗读出来。"
    "可以有轻微的关心、俏皮和陪伴感，但不要啰嗦，不要说教。"
)

class ChatReq(BaseModel):
    text: str
    voice: str | None = None

class VoiceReq(BaseModel):
    voice: str

class VolumeReq(BaseModel):
    volume: float

def make_wav_header(sample_rate: int, channels: int = 1, bits_per_sample: int = 16, data_size: int = 0x7fffffff) -> bytes:
    byte_rate = sample_rate * channels * bits_per_sample // 8
    block_align = channels * bits_per_sample // 8
    riff_size = min(data_size + 36, 0xffffffff)
    return b"".join([
        b"RIFF",
        riff_size.to_bytes(4, "little"),
        b"WAVE",
        b"fmt ",
        (16).to_bytes(4, "little"),
        (1).to_bytes(2, "little"),
        channels.to_bytes(2, "little"),
        sample_rate.to_bytes(4, "little"),
        byte_rate.to_bytes(4, "little"),
        block_align.to_bytes(2, "little"),
        bits_per_sample.to_bytes(2, "little"),
        b"data",
        data_size.to_bytes(4, "little"),
    ])

def split_tts_text(text: str, max_chars: int = TTS_SEGMENT_CHARS) -> list[str]:
    text = re.sub(r"\s+", " ", text.strip())
    if not text:
        return []

    parts = []
    for part in re.split(r"(?<=[。！？!?；;，,、])", text):
        part = part.strip()
        if part:
            parts.append(part)

    if not parts:
        parts = [text]

    segments = []
    for part in parts:
        while len(part) > max_chars:
            segments.append(part[:max_chars])
            part = part[max_chars:]
        if part:
            segments.append(part)

    return segments

def trim_history_text(text: str) -> str:
    text = re.sub(r"\s+", " ", text.strip())
    if CHAT_HISTORY_MAX_CHARS <= 0 or len(text) <= CHAT_HISTORY_MAX_CHARS:
        return text
    return text[:CHAT_HISTORY_MAX_CHARS].rstrip()

def build_model_messages(text: str) -> list[dict[str, str]]:
    messages = [{"role": "system", "content": SYSTEM_PROMPT}]
    if CHAT_HISTORY_MAX_TURNS > 0:
        with _conversation_lock:
            messages.extend(_conversation_history)
    messages.append({"role": "user", "content": text})
    return messages

def remember_exchange(text: str, reply: str) -> None:
    if CHAT_HISTORY_MAX_TURNS <= 0:
        return

    with _conversation_lock:
        _conversation_history.append({"role": "user", "content": trim_history_text(text)})
        _conversation_history.append({"role": "assistant", "content": trim_history_text(reply)})
        max_messages = CHAT_HISTORY_MAX_TURNS * 2
        if len(_conversation_history) > max_messages:
            del _conversation_history[:-max_messages]

def clear_conversation_history() -> None:
    with _conversation_lock:
        _conversation_history.clear()

def conversation_history_snapshot() -> list[dict[str, str]]:
    with _conversation_lock:
        return list(_conversation_history)

def ask_model(text: str) -> str:
    text = text.strip()
    if not text:
        raise HTTPException(status_code=400, detail="text 不能为空")

    payload = {
        "model": MODEL_NAME,
        "messages": build_model_messages(text),
        "stream": False,
        "think": THINK,
        "keep_alive": "10m",
        "options": {
            "num_predict": MAX_REPLY_TOKENS,
            "temperature": 0.7
        }
    }

    started = time.perf_counter()
    try:
        r = requests.post(OLLAMA_URL, json=payload, timeout=120)
        r.raise_for_status()
    except requests.RequestException as exc:
        raise HTTPException(status_code=502, detail=f"Ollama 请求失败: {exc}") from exc

    data = r.json()
    reply = data["message"]["content"]
    remember_exchange(text, reply)
    logger.info("Ollama reply in %.2fs, %d chars", time.perf_counter() - started, len(reply))
    return reply

def resolve_path(path_text: str | os.PathLike[str]) -> Path:
    path = Path(path_text)
    if path.is_absolute():
        return path
    return BASE_DIR / path

def list_piper_voices() -> list[dict]:
    voices = []
    for model_path in sorted(PIPER_MODEL_DIR.glob("*.onnx")):
        config_path = Path(f"{model_path}.json")
        info = {
            "id": model_path.stem,
            "model": str(model_path.relative_to(BASE_DIR)),
            "config": str(config_path.relative_to(BASE_DIR)) if config_path.exists() else None,
            "config_exists": config_path.exists(),
        }

        if config_path.exists():
            try:
                with config_path.open("r", encoding="utf-8") as f:
                    config = json.load(f)
                audio = config.get("audio", {})
                language = config.get("language", {})
                info.update({
                    "sample_rate": audio.get("sample_rate"),
                    "quality": audio.get("quality"),
                    "language": language.get("name_native") or language.get("name_english"),
                    "language_code": language.get("code"),
                    "dataset": config.get("dataset"),
                })
            except (OSError, json.JSONDecodeError) as exc:
                info["config_error"] = str(exc)

        voices.append(info)
    return voices

def default_piper_voice_id() -> str | None:
    if _selected_piper_voice_id:
        return Path(_selected_piper_voice_id).stem

    models = sorted(PIPER_MODEL_DIR.glob("*.onnx"))
    if models:
        return models[0].stem
    return None

def resolve_piper_model(voice: str | None = None) -> Path:
    model_value = (voice or _selected_piper_voice_id or os.getenv("PIPER_MODEL", "")).strip()
    if model_value:
        model_path = Path(model_value)
        if model_path.suffix != ".onnx":
            model_path = PIPER_MODEL_DIR / f"{model_value}.onnx"
        return resolve_path(model_path)

    models = sorted(PIPER_MODEL_DIR.glob("*.onnx"))
    if models:
        return models[0]
    return PIPER_MODEL_DIR / "zh_CN-xiao_ya-medium.onnx"

def resolve_piper_config(model_path: Path, voice: str | None = None) -> Path:
    config_value = os.getenv("PIPER_CONFIG", "").strip()
    if config_value and not voice:
        return resolve_path(config_value)
    return Path(f"{model_path}.json")

def set_default_piper_voice(voice: str) -> str:
    global _selected_piper_voice_id

    voice = voice.strip()
    if not voice:
        raise HTTPException(status_code=400, detail="voice 不能为空")

    model_path = resolve_piper_model(voice)
    config_path = resolve_piper_config(model_path, voice)
    if not model_path.exists():
        raise HTTPException(status_code=404, detail=f"Piper 模型不存在: {model_path.name}")
    if not config_path.exists():
        raise HTTPException(status_code=404, detail=f"Piper 配置不存在: {config_path.name}")

    _selected_piper_voice_id = model_path.stem
    return _selected_piper_voice_id

def resample_pcm16_mono(pcm: bytes, sample_rate: int, target_rate: int) -> bytes:
    if sample_rate == target_rate or not pcm:
        return pcm

    import audioop

    converted, _ = audioop.ratecv(pcm, 2, 1, sample_rate, target_rate, None)
    return converted

def normalize_wav_for_esp(input_path: Path, output_path: Path) -> None:
    import numpy as np
    import audioop

    with wave.open(str(input_path), "rb") as wav_in:
        channels = wav_in.getnchannels()
        sample_width = wav_in.getsampwidth()
        sample_rate = wav_in.getframerate()
        frames = wav_in.readframes(wav_in.getnframes())

    if sample_width != 2:
        raise HTTPException(status_code=500, detail=f"TTS WAV 不是 16-bit PCM: {sample_width * 8} bit")

    if channels > 1:
        if channels == 2:
            frames = audioop.tomono(frames, sample_width, 0.5, 0.5)
        else:
            samples = np.frombuffer(frames, dtype="<i2").reshape(-1, channels)
            frames = samples.astype(np.int32).mean(axis=1).astype("<i2").tobytes()

    frames = resample_pcm16_mono(frames, sample_rate, ESP_AUDIO_SAMPLE_RATE)

    samples = np.frombuffer(frames, dtype="<i2")
    samples = np.clip(samples, -32768, 32767).astype("<i2")

    with wave.open(str(output_path), "wb") as wav_out:
        wav_out.setnchannels(1)
        wav_out.setsampwidth(2)
        wav_out.setframerate(ESP_AUDIO_SAMPLE_RATE)
        wav_out.writeframes(samples.tobytes())

def normalize_pcm_for_esp(samples, sample_rate: int, channels: int = 1) -> bytes:
    import numpy as np

    samples = np.asarray(samples)
    if channels > 1:
        samples = samples.reshape(-1, channels).astype(np.float32).mean(axis=1)

    if np.issubdtype(samples.dtype, np.floating):
        samples = np.clip(samples, -1.0, 1.0) * 32767.0

    samples = np.clip(samples, -32768, 32767).astype("<i2")
    return resample_pcm16_mono(samples.tobytes(), sample_rate, ESP_AUDIO_SAMPLE_RATE)

def synthesize_windows_speech(text: str, wav_path: Path) -> None:
    if not TTS_SCRIPT.exists():
        raise HTTPException(status_code=500, detail=f"TTS 脚本不存在: {TTS_SCRIPT}")

    text_path = wav_path.with_name("input.txt")
    text_path.write_text(text, encoding="utf-8")

    cmd = [
        "powershell.exe",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(TTS_SCRIPT),
        str(text_path),
        str(wav_path),
        TTS_RATE,
        TTS_VOICE,
        str(round(max(0.0, min(1.0, _tts_volume)) * 100)),
        str(ESP_AUDIO_SAMPLE_RATE),
    ]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=TTS_TIMEOUT, check=False)
    except subprocess.TimeoutExpired as exc:
        raise HTTPException(status_code=504, detail="Windows TTS 生成超时") from exc

    if result.returncode != 0 or not wav_path.exists():
        detail = result.stderr.strip() or result.stdout.strip() or "Windows TTS 生成失败"
        raise HTTPException(status_code=500, detail=detail)

def resolve_edge_voice(voice: str | None = None) -> str:
    if voice and voice.strip():
        return voice.strip()
    return EDGE_VOICE

def list_windows_voices() -> list[dict]:
    command = r"""
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
Add-Type -AssemblyName System.Speech
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
try {
    $synth.GetInstalledVoices() |
        ForEach-Object {
            [pscustomobject]@{
                name = $_.VoiceInfo.Name
                culture = $_.VoiceInfo.Culture.Name
                gender = $_.VoiceInfo.Gender.ToString()
                age = $_.VoiceInfo.Age.ToString()
                description = $_.VoiceInfo.Description
            }
        } |
        ConvertTo-Json -Depth 3
} finally {
    $synth.Dispose()
}
"""

    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", command],
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=10,
        check=False,
    )

    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "Failed to list Windows voices"
        raise HTTPException(status_code=500, detail=detail)

    output = result.stdout.strip()
    if not output:
        return []

    voices = json.loads(output)
    if isinstance(voices, dict):
        return [voices]
    return voices

async def save_edge_media(text: str, media_path: Path, voice: str | None = None) -> None:
    try:
        import edge_tts
    except ImportError as exc:
        raise HTTPException(status_code=500, detail="edge-tts is not installed in this Python environment") from exc

    communicate = edge_tts.Communicate(
        text,
        resolve_edge_voice(voice),
        rate=EDGE_RATE,
        volume="+0%",
        pitch=EDGE_PITCH,
        receive_timeout=EDGE_TIMEOUT,
    )
    await communicate.save(str(media_path))

def synthesize_edge_speech(text: str, wav_path: Path, voice: str | None = None) -> None:
    try:
        import imageio_ffmpeg
    except ImportError as exc:
        raise HTTPException(status_code=500, detail="imageio-ffmpeg is not installed in this Python environment") from exc

    media_path = wav_path.with_name("edge_audio.mp3")
    raw_wav_path = wav_path.with_name("edge_raw.wav")

    try:
        asyncio.run(save_edge_media(text, media_path, voice))
    except Exception as exc:
        raise HTTPException(status_code=502, detail=f"Edge TTS request failed: {exc}") from exc

    if not media_path.exists() or media_path.stat().st_size == 0:
        raise HTTPException(status_code=500, detail="Edge TTS did not generate audio")

    cmd = [
        imageio_ffmpeg.get_ffmpeg_exe(),
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        str(media_path),
        "-ac",
        "1",
        "-ar",
        str(ESP_AUDIO_SAMPLE_RATE),
        "-sample_fmt",
        "s16",
        "-filter:a",
        f"volume={max(0.0, min(2.0, _tts_volume))}",
        str(raw_wav_path),
    ]

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=EDGE_TIMEOUT, check=False)
    if result.returncode != 0 or not raw_wav_path.exists() or raw_wav_path.stat().st_size == 0:
        detail = result.stderr.strip() or result.stdout.strip() or "Edge TTS conversion failed"
        raise HTTPException(status_code=500, detail=detail)

    normalize_wav_for_esp(raw_wav_path, wav_path)

def can_stream_edge() -> bool:
    return TTS_ENGINE == "edge" and STREAM_EDGE

def stream_edge_wav(text: str, voice: str | None = None):
    try:
        import edge_tts
        import imageio_ffmpeg
    except ImportError as exc:
        raise RuntimeError("edge-tts or imageio-ffmpeg is not installed") from exc

    text = text.strip()
    if not text:
        return

    start_time = time.perf_counter()
    volume = max(0.0, min(2.0, _tts_volume))
    cmd = [
        imageio_ffmpeg.get_ffmpeg_exe(),
        "-hide_banner",
        "-loglevel",
        "error",
        "-f",
        "mp3",
        "-i",
        "pipe:0",
        "-vn",
        "-ac",
        "1",
        "-ar",
        str(ESP_AUDIO_SAMPLE_RATE),
        "-sample_fmt",
        "s16",
        "-filter:a",
        f"volume={volume}",
        "-f",
        "s16le",
        "pipe:1",
    ]

    ffmpeg = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    writer_error: list[BaseException] = []

    def feed_edge_audio() -> None:
        async def feed() -> None:
            communicate = edge_tts.Communicate(
                text,
                resolve_edge_voice(voice),
                rate=EDGE_RATE,
                volume="+0%",
                pitch=EDGE_PITCH,
                receive_timeout=EDGE_TIMEOUT,
            )
            async for chunk in communicate.stream():
                if chunk.get("type") == "audio" and chunk.get("data") and ffmpeg.stdin:
                    try:
                        ffmpeg.stdin.write(chunk["data"])
                    except OSError:
                        return

        try:
            asyncio.run(feed())
        except BaseException as exc:
            writer_error.append(exc)
            logger.exception("Edge streaming failed: %s", exc)
        finally:
            if ffmpeg.stdin:
                try:
                    ffmpeg.stdin.close()
                except OSError:
                    pass

    writer = threading.Thread(target=feed_edge_audio, name="edge-tts-stream", daemon=True)
    writer.start()

    first_pcm = True
    try:
        yield make_wav_header(ESP_AUDIO_SAMPLE_RATE)

        if not ffmpeg.stdout:
            return

        while True:
            chunk = ffmpeg.stdout.read(4096)
            if chunk:
                if first_pcm:
                    logger.info("Edge streaming first PCM in %.2fs", time.perf_counter() - start_time)
                    first_pcm = False
                yield chunk
                continue

            if ffmpeg.poll() is not None:
                break

        writer.join(timeout=2)
        return_code = ffmpeg.wait(timeout=2)
        stderr = b""
        if ffmpeg.stderr:
            stderr = ffmpeg.stderr.read()

        if writer_error:
            logger.error("Edge streaming ended with writer error: %s", writer_error[0])
        if return_code != 0:
            logger.error("Edge streaming ffmpeg failed: %s", stderr.decode(errors="ignore").strip())
    finally:
        if ffmpeg.poll() is None:
            ffmpeg.kill()
        writer.join(timeout=1)

def get_piper_voice(voice: str | None = None):
    model_path = resolve_piper_model(voice)
    config_path = resolve_piper_config(model_path, voice)
    cache_key = str(model_path.resolve())

    if cache_key in _piper_voices:
        return _piper_voices[cache_key]

    with _piper_lock:
        if cache_key in _piper_voices:
            return _piper_voices[cache_key]

        try:
            from piper.voice import PiperVoice
        except ImportError as exc:
            raise RuntimeError("当前 Python 没有安装 piper，请用 .venv-piper 启动服务") from exc

        if not model_path.exists():
            raise RuntimeError(f"Piper 模型不存在: {model_path}")
        if not config_path.exists():
            raise RuntimeError(f"Piper 配置不存在: {config_path}")

        _piper_voices[cache_key] = PiperVoice.load(model_path, config_path=config_path, download_dir=BASE_DIR)
        return _piper_voices[cache_key]

def synthesize_piper_in_process(text: str, wav_path: Path, voice: str | None = None) -> None:
    from piper.config import SynthesisConfig

    raw_path = wav_path.with_name("piper_raw.wav")
    piper_voice = get_piper_voice(voice)
    syn_config = SynthesisConfig(
        length_scale=PIPER_LENGTH_SCALE,
        volume=_tts_volume,
    )

    with wave.open(str(raw_path), "wb") as wav_file:
        piper_voice.synthesize_wav(text, wav_file, syn_config=syn_config)

    normalize_wav_for_esp(raw_path, wav_path)

def synthesize_piper_subprocess(text: str, wav_path: Path, voice: str | None = None) -> None:
    if not PIPER_PYTHON.exists():
        raise HTTPException(status_code=500, detail=f"Piper Python 不存在: {PIPER_PYTHON}")

    model_path = resolve_piper_model(voice)
    config_path = resolve_piper_config(model_path, voice)
    raw_path = wav_path.with_name("piper_raw.wav")
    text_path = wav_path.with_name("input.txt")
    text_path.write_text(text, encoding="utf-8")

    cmd = [
        str(PIPER_PYTHON),
        "-m",
        "piper",
        "-m",
        str(model_path),
        "-c",
        str(config_path),
        "-i",
        str(text_path),
        "-f",
        str(raw_path),
        "--length-scale",
        str(PIPER_LENGTH_SCALE),
        "--sentence-silence",
        str(PIPER_SENTENCE_SILENCE),
        "--volume",
        str(_tts_volume),
    ]

    env = os.environ.copy()
    env["PYTHONUTF8"] = "1"
    env["HF_HUB_DISABLE_SYMLINKS_WARNING"] = "1"

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=PIPER_TIMEOUT, check=False, env=env)
    except subprocess.TimeoutExpired as exc:
        raise HTTPException(status_code=504, detail="Piper TTS 生成超时") from exc

    if result.returncode != 0 or not raw_path.exists() or raw_path.stat().st_size == 0:
        detail = result.stderr.strip() or result.stdout.strip() or "Piper TTS 生成失败"
        raise HTTPException(status_code=500, detail=detail)

    normalize_wav_for_esp(raw_path, wav_path)

def synthesize_piper_speech(text: str, wav_path: Path, voice: str | None = None) -> None:
    if "piper" in sys.modules or sys.executable.lower() == str(PIPER_PYTHON).lower():
        try:
            synthesize_piper_in_process(text, wav_path, voice)
            return
        except (ImportError, RuntimeError, UnicodeDecodeError):
            pass

    synthesize_piper_subprocess(text, wav_path, voice)

def can_stream_piper() -> bool:
    if TTS_ENGINE != "piper" or not STREAM_PIPER:
        return False
    if not PIPER_PYTHON.exists():
        return False
    return str(Path(sys.executable).resolve()).lower() == str(PIPER_PYTHON.resolve()).lower()

def stream_piper_wav(text: str, voice: str | None = None):
    from piper.config import SynthesisConfig

    text = text.strip()
    if not text:
        return

    yield make_wav_header(ESP_AUDIO_SAMPLE_RATE)

    piper_voice = get_piper_voice(voice)
    syn_config = SynthesisConfig(
        length_scale=PIPER_LENGTH_SCALE,
        volume=_tts_volume,
    )

    silence_samples = int(ESP_AUDIO_SAMPLE_RATE * PIPER_SENTENCE_SILENCE)
    silence = b"\x00\x00" * silence_samples

    for segment in split_tts_text(text):
        for audio_chunk in piper_voice.synthesize(segment, syn_config=syn_config):
            yield normalize_pcm_for_esp(
                audio_chunk.audio_int16_array,
                audio_chunk.sample_rate,
                audio_chunk.sample_channels,
            )

        if silence:
            yield silence

def synthesize_speech(text: str, voice: str | None = None) -> bytes:
    text = text.strip()
    if not text:
        raise HTTPException(status_code=400, detail="text 不能为空")

    with tempfile.TemporaryDirectory() as tmp_dir:
        wav_path = Path(tmp_dir) / "speech.wav"

        if TTS_ENGINE == "windows":
            synthesize_windows_speech(text, wav_path)
        elif TTS_ENGINE == "edge":
            synthesize_edge_speech(text, wav_path, voice)
        elif TTS_ENGINE == "piper":
            synthesize_piper_speech(text, wav_path, voice)
        else:
            raise HTTPException(status_code=500, detail=f"未知 TTS_ENGINE: {TTS_ENGINE}")

        if not wav_path.exists() or wav_path.stat().st_size == 0:
            raise HTTPException(status_code=500, detail="TTS 没有生成 WAV")

        return wav_path.read_bytes()

@app.on_event("startup")
def preload_tts_model() -> None:
    if TTS_ENGINE != "piper" or not PRELOAD_PIPER:
        return

    current_python = str(Path(sys.executable).resolve()).lower()
    piper_python = str(PIPER_PYTHON.resolve()).lower() if PIPER_PYTHON.exists() else str(PIPER_PYTHON).lower()
    if current_python != piper_python:
        logger.warning("Piper will run as subprocess; start with start_piper_server.ps1 for fast TTS")
        return

    logger.info("Preloading Piper voice: %s", default_piper_voice_id())
    try:
        with tempfile.TemporaryDirectory() as tmp_dir:
            synthesize_piper_in_process("你好。", Path(tmp_dir) / "warmup.wav")
        logger.info("Piper voice is ready")
    except Exception as exc:
        logger.exception("Piper preload failed: %s", exc)

def transcribe_windows_speech(wav_path: Path) -> str:
    if not ASR_SCRIPT.exists():
        raise HTTPException(status_code=500, detail=f"ASR script not found: {ASR_SCRIPT}")

    command = [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(ASR_SCRIPT),
        "-WavPath",
        str(wav_path),
        "-Culture",
        ASR_CULTURE,
        "-TimeoutSeconds",
        str(ASR_TIMEOUT),
    ]

    try:
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=ASR_TIMEOUT + 5,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise HTTPException(status_code=504, detail="Windows ASR timed out") from exc

    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "Windows ASR failed").strip()
        raise HTTPException(status_code=502, detail=detail)

    text = result.stdout.strip()
    if not text:
        raise HTTPException(status_code=422, detail="Windows ASR did not recognize speech")
    return text

def transcribe_speech(wav_path: Path) -> str:
    if ASR_ENGINE in ("", "none", "disabled"):
        raise HTTPException(status_code=501, detail="ASR is disabled. Set ASR_ENGINE=windows to enable Windows SAPI recognition.")
    if ASR_ENGINE == "windows":
        return transcribe_windows_speech(wav_path)
    raise HTTPException(status_code=501, detail=f"Unsupported ASR_ENGINE: {ASR_ENGINE}")

@app.post("/chat")
def chat(req: ChatReq):
    return {"reply": ask_model(req.text)}

@app.get("/chat_memory")
def chat_memory():
    history = conversation_history_snapshot()
    return {
        "max_turns": CHAT_HISTORY_MAX_TURNS,
        "max_chars": CHAT_HISTORY_MAX_CHARS,
        "turns": len(history) // 2,
        "messages": history,
    }

@app.post("/chat_memory/reset")
def chat_memory_reset():
    clear_conversation_history()
    return {"ok": True, "turns": 0}

@app.post("/tts")
def tts(req: ChatReq):
    if can_stream_edge():
        return StreamingResponse(
            stream_edge_wav(req.text, req.voice),
            media_type="audio/wav",
            headers={"X-TTS-Mode": "edge-stream"},
        )

    if can_stream_piper():
        return StreamingResponse(
            stream_piper_wav(req.text, req.voice),
            media_type="audio/wav",
            headers={"X-TTS-Mode": "stream"},
        )

    wav_data = synthesize_speech(req.text, req.voice)
    return Response(content=wav_data, media_type="audio/wav", headers={"X-TTS-Mode": "buffer"})

@app.get("/tts_voices")
def tts_voices():
    return {
        "engine": TTS_ENGINE,
        "default": default_piper_voice_id(),
        "voices": list_piper_voices(),
    }

@app.get("/edge_voices")
def edge_voices(locale: str = "zh-CN"):
    async def load_voices():
        try:
            import edge_tts
        except ImportError as exc:
            raise HTTPException(status_code=500, detail="edge-tts is not installed in this Python environment") from exc

        return await edge_tts.list_voices()

    try:
        voices = asyncio.run(load_voices())
    except Exception as exc:
        raise HTTPException(status_code=502, detail=f"Edge voice list request failed: {exc}") from exc

    filtered = []
    for item in voices:
        if locale and not str(item.get("Locale", "")).lower().startswith(locale.lower()):
            continue
        filtered.append({
            "name": item.get("ShortName"),
            "locale": item.get("Locale"),
            "gender": item.get("Gender"),
            "display_name": item.get("FriendlyName"),
        })

    return {
        "engine": "edge",
        "default": EDGE_VOICE,
        "voices": filtered,
    }

@app.get("/windows_voices")
def windows_voices():
    return {
        "engine": "windows",
        "default": TTS_VOICE,
        "voices": list_windows_voices(),
    }

@app.get("/tts_status")
def tts_status():
    piper_python = str(PIPER_PYTHON.resolve()) if PIPER_PYTHON.exists() else str(PIPER_PYTHON)
    current_python = str(Path(sys.executable).resolve())
    return {
        "engine": TTS_ENGINE,
        "python": current_python,
        "sample_rate": ESP_AUDIO_SAMPLE_RATE,
        "windows_voice": TTS_VOICE,
        "windows_rate": TTS_RATE,
        "edge_voice": EDGE_VOICE,
        "edge_rate": EDGE_RATE,
        "edge_pitch": EDGE_PITCH,
        "edge_streaming": can_stream_edge(),
        "max_reply_tokens": MAX_REPLY_TOKENS,
        "chat_history_turns": len(conversation_history_snapshot()) // 2,
        "chat_history_max_turns": CHAT_HISTORY_MAX_TURNS,
        "piper_python": piper_python,
        "piper_in_process": TTS_ENGINE == "piper" and current_python.lower() == piper_python.lower(),
        "default_voice": default_piper_voice_id(),
        "loaded_voices": len(_piper_voices),
        "volume": _tts_volume,
        "streaming": can_stream_edge() or can_stream_piper(),
        "segment_chars": TTS_SEGMENT_CHARS,
        "asr_engine": ASR_ENGINE,
        "asr_culture": ASR_CULTURE,
        "asr_script": str(ASR_SCRIPT),
    }

@app.post("/tts_voice")
def tts_voice(req: VoiceReq):
    selected = set_default_piper_voice(req.voice)
    return {
        "engine": TTS_ENGINE,
        "default": selected,
        "voices": list_piper_voices(),
    }

@app.post("/tts_volume")
def tts_volume(req: VolumeReq):
    global _tts_volume

    if req.volume < 0 or req.volume > 2:
        raise HTTPException(status_code=400, detail="volume 范围是 0 到 2，建议 0.3 到 1.0")

    _tts_volume = req.volume
    return {
        "engine": TTS_ENGINE,
        "volume": _tts_volume,
    }

@app.post("/_voice_chat_disabled")
def voice_chat_disabled():
    raise HTTPException(
        status_code=501,
        detail="ASR placeholder route is disabled."
    )

@app.post("/voice_chat")
async def voice_chat(request: Request):
    audio = await request.body()
    if not audio:
        raise HTTPException(status_code=400, detail="audio body is empty")
    if len(audio) > ASR_MAX_AUDIO_BYTES:
        raise HTTPException(status_code=413, detail=f"audio body too large: {len(audio)} bytes")

    with tempfile.TemporaryDirectory() as tmp_dir:
        wav_path = Path(tmp_dir) / "voice.wav"
        wav_path.write_bytes(audio)
        try:
            with wave.open(str(wav_path), "rb") as wav:
                if wav.getnchannels() != 1 or wav.getsampwidth() != 2:
                    raise HTTPException(status_code=400, detail="voice WAV must be mono 16-bit PCM")
                logger.info(
                    "voice upload: %s Hz, %s frames, %.2f sec",
                    wav.getframerate(),
                    wav.getnframes(),
                    wav.getnframes() / max(wav.getframerate(), 1),
                )
        except wave.Error as exc:
            raise HTTPException(status_code=400, detail=f"invalid WAV audio: {exc}") from exc

        text = transcribe_speech(wav_path)

    reply = ask_model(text)
    return {"text": text, "reply": reply}
