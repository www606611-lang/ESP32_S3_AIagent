param(
    [Parameter(Mandatory = $true)]
    [string]$TextPath,

    [Parameter(Mandatory = $true)]
    [string]$WavPath,

    [int]$Rate = 2,

    [string]$VoiceName = "",

    [int]$Volume = 50,

    [int]$SampleRate = 22050
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Speech

$text = Get-Content -LiteralPath $TextPath -Raw -Encoding UTF8
if ([string]::IsNullOrWhiteSpace($text)) {
    $text = " "
}

$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(
    $SampleRate,
    [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
    [System.Speech.AudioFormat.AudioChannel]::Mono
)

if (-not [string]::IsNullOrWhiteSpace($VoiceName)) {
    $synth.SelectVoice($VoiceName)
} else {
    $zhVoice = $synth.GetInstalledVoices() |
        ForEach-Object { $_.VoiceInfo } |
        Where-Object { $_.Culture.Name -like "zh-*" } |
        Select-Object -First 1

    if ($null -ne $zhVoice) {
        $synth.SelectVoice($zhVoice.Name)
    }
}

try {
    $synth.Rate = [Math]::Max(-10, [Math]::Min(10, $Rate))
    $synth.Volume = [Math]::Max(0, [Math]::Min(100, $Volume))
    $synth.SetOutputToWaveFile($WavPath, $format)
    $synth.Speak($text)
} finally {
    $synth.Dispose()
}
