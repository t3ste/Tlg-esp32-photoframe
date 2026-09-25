# Generates the speech samples used by host_tests/test_kws.cpp with the
# Windows text-to-speech voices (16 kHz, 16 bit, mono), then trims the silence
# around each word. Re-run only when the sample set should change - the WAV
# files are committed so CI needs no speech engine.
#
#   powershell -File scripts/generate_kws_test_audio.ps1
#
# Needs the "Microsoft Hedda Desktop" (de-DE) and "Microsoft Zira Desktop"
# (en-US) voices that ship with Windows.

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Speech

$outDir = Join-Path $PSScriptRoot "..\host_tests\data\kws"
New-Item -ItemType Directory -Force $outDir | Out-Null

$format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(
    16000,
    [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
    [System.Speech.AudioFormat.AudioChannel]::Mono)

function New-Sample($voice, $rate, $text, $file) {
    $synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
    $synth.SelectVoice($voice)
    $synth.Rate = $rate
    $synth.SetOutputToWaveFile((Join-Path $outDir $file), $format)
    $synth.Speak($text)
    $synth.Dispose()
}

# The wake/stop word in German at several speaking speeds, plus an English
# voice saying "Stop" (a different speaker and accent).
foreach ($rate in -2, 0, 2, 3) {
    New-Sample "Microsoft Hedda Desktop" $rate "Stopp" "stopp_hedda_r$rate.wav"
}
New-Sample "Microsoft Zira Desktop" 0 "Stop" "stopp_zira_r0.wav"

# Words that must NOT trigger: unrelated ones and hard, similar-sounding ones.
$others = @{
    "guten_morgen" = "Guten Morgen"
    "danke"        = "Danke"
    "licht_aus"    = "Licht aus"
    "weiter"       = "Weiter"
    "alarm"        = "Alarm"
    "nein"         = "Nein"
    "hallo"        = "Hallo"
    "stoppuhr"     = "Stoppuhr"
    "stock"        = "Stock"
    "spott"        = "Spott"
}
foreach ($name in $others.Keys) {
    New-Sample "Microsoft Hedda Desktop" 0 $others[$name] "neg_$name.wav"
}

# Trim leading/trailing silence (keep ~120 ms around the word).
python (Join-Path $PSScriptRoot "trim_kws_audio.py") $outDir
Write-Host "Done: $outDir"
