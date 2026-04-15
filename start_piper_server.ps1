param(
    [switch]$Background
)

$ErrorActionPreference = "Stop"

$env:PYTHONUTF8 = "1"
$env:TTS_ENGINE = "piper"
$env:PIPER_MODEL = "zh_CN-xiao_ya-medium"
$env:STREAM_PIPER = "0"
$env:ESP_AUDIO_SAMPLE_RATE = "22050"

if ([string]::IsNullOrWhiteSpace($env:PIPER_LENGTH_SCALE)) {
    $env:PIPER_LENGTH_SCALE = "0.9"
}

if ([string]::IsNullOrWhiteSpace($env:TTS_SEGMENT_CHARS)) {
    $env:TTS_SEGMENT_CHARS = "18"
}

if ([string]::IsNullOrWhiteSpace($env:PIPER_SENTENCE_SILENCE)) {
    $env:PIPER_SENTENCE_SILENCE = "0.15"
}

if ([string]::IsNullOrWhiteSpace($env:PIPER_VOLUME)) {
    $env:PIPER_VOLUME = "0.6"
}

$python = Join-Path $PSScriptRoot ".venv-piper\Scripts\python.exe"
if (-not (Test-Path -LiteralPath $python)) {
    throw "Piper Python not found: $python"
}

$listener = Get-NetTCPConnection -LocalPort 8000 -State Listen -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($null -ne $listener) {
    $process = Get-Process -Id $listener.OwningProcess -ErrorAction SilentlyContinue
    $name = if ($null -ne $process) { $process.ProcessName } else { "unknown" }
    throw "Port 8000 is already used by PID $($listener.OwningProcess) ($name). Stop that server first, then run this script again."
}

$arguments = @("-m", "uvicorn", "server:app", "--host", "0.0.0.0", "--port", "8000")

if ($Background) {
    Start-Process -FilePath $python -ArgumentList $arguments -WorkingDirectory $PSScriptRoot
    Write-Host "Piper server started in a new window on http://0.0.0.0:8000"
    return
}

& $python @arguments
