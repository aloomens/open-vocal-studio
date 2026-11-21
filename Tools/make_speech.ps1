# Makes test-output\speech48.wav (48 kHz mono speech from Windows' built-in text-to-speech),
# used by the denoiser tests in VocalTest.exe and DfnCheck.exe. Run from the project folder.
Add-Type -AssemblyName System.Speech
$out = Join-Path (Get-Location) 'test-output\speech48.wav'
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
$f = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo 48000, ([System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen), ([System.Speech.AudioFormat.AudioChannel]::Mono)
$s.SetOutputToWaveFile($out, $f)
$s.Speak('The quick brown fox jumps over the lazy dog. She sells sea shells by the sea shore, and the peppered pickles are perfectly preserved.')
$s.Dispose()
"wrote $out"
