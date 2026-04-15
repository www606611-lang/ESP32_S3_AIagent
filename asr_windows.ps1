param(
    [Parameter(Mandatory = $true)]
    [string]$WavPath,

    [string]$Culture = "zh-CN",

    [int]$TimeoutSeconds = 15
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()

if (-not (Test-Path -LiteralPath $WavPath)) {
    throw "WAV file not found: $WavPath"
}

Add-Type -AssemblyName System.Speech

$recognizerInfo = [System.Speech.Recognition.SpeechRecognitionEngine]::InstalledRecognizers() |
    Where-Object { $_.Culture.Name -ieq $Culture } |
    Select-Object -First 1

if ($null -eq $recognizerInfo) {
    $recognizerInfo = [System.Speech.Recognition.SpeechRecognitionEngine]::InstalledRecognizers() |
        Where-Object { $_.Culture.TwoLetterISOLanguageName -ieq "zh" } |
        Select-Object -First 1
}

if ($null -eq $recognizerInfo) {
    throw "No Windows speech recognizer found for $Culture"
}

$engine = New-Object System.Speech.Recognition.SpeechRecognitionEngine -ArgumentList $recognizerInfo
try {
    $grammar = New-Object System.Speech.Recognition.DictationGrammar
    $engine.LoadGrammar($grammar)
    $engine.SetInputToWaveFile($WavPath)

    $result = $engine.Recognize([TimeSpan]::FromSeconds($TimeoutSeconds))
    if ($null -eq $result -or [string]::IsNullOrWhiteSpace($result.Text)) {
        exit 2
    }

    [Console]::Out.WriteLine($result.Text)
} finally {
    $engine.Dispose()
}
