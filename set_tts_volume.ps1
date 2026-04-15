param(
    [double]$Volume = 0.2
)

$ErrorActionPreference = "Stop"

Invoke-RestMethod `
    -Uri "http://127.0.0.1:8000/tts_volume" `
    -Method Post `
    -ContentType "application/json" `
    -Body (@{ volume = $Volume } | ConvertTo-Json)
