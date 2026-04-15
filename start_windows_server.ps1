param(
    [switch]$Background,

    [string]$VoiceName = "",

    [int]$Rate = 3,

    [double]$Volume = 0.65
)

$ErrorActionPreference = "Stop"

$env:PYTHONUTF8 = "1"
$env:TTS_ENGINE = "windows"
$env:ESP_AUDIO_SAMPLE_RATE = "22050"

if (-not [string]::IsNullOrWhiteSpace($VoiceName)) {
    $env:TTS_VOICE = $VoiceName
}

if ([string]::IsNullOrWhiteSpace($env:TTS_RATE) -or $PSBoundParameters.ContainsKey("Rate")) {
    $env:TTS_RATE = [string]$Rate
}

if ([string]::IsNullOrWhiteSpace($env:TTS_VOLUME) -or $PSBoundParameters.ContainsKey("Volume")) {
    $env:TTS_VOLUME = [string]$Volume
}

$python = Join-Path $PSScriptRoot ".venv-piper\Scripts\python.exe"
if (-not (Test-Path -LiteralPath $python)) {
    throw "Python environment not found: $python"
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
    Write-Host "Windows TTS server started in a new window on http://0.0.0.0:8000"
    return
}

& $python @arguments
