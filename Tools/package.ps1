# Builds the release zips from a finished Release build:
#   dist\OpenVocalStudio-<ver>-win64.zip    plugin + standalone app + docs + license
#   dist\OpenVocalStudio-<ver>-source.zip   complete corresponding source (GNU AGPLv3), incl. JUCE and the ASIO SDK
# Usage (from the project folder, after "cmake --build build --config Release"):
#   pwsh -File Tools\package.ps1   (PowerShell 7: its zips use "/" paths, which every unzip tool reads correctly)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cmake = Get-Content "$root\CMakeLists.txt" -Raw
$ver = [regex]::Match($cmake, 'project\(VocalStudio VERSION ([0-9.]+)').Groups[1].Value
$name = [regex]::Match($cmake, 'set\(VS_PRODUCT_NAME "([^"]+)"\)').Groups[1].Value
$file = $name.Replace(' ', '')                       # "VocalStudio"
$art = Join-Path $root 'build\VocalStudio_artefacts\Release'
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null

# ---- Version check: exe, vst3 binary and moduleinfo must all report the build version ----
$exeV = (Get-Item "$art\Standalone\$name.exe").VersionInfo.FileVersion
$vstV = (Get-Item "$art\VST3\$name.vst3\Contents\x86_64-win\$name.vst3").VersionInfo.FileVersion
$miV = [regex]::Match((Get-Content "$art\VST3\$name.vst3\Contents\Resources\moduleinfo.json" -Raw), '"Version"\s*:\s*"([^"]+)"').Groups[1].Value   # (JSON5: Windows PowerShell's ConvertFrom-Json rejects it)
foreach ($v in @($exeV, $vstV, $miV)) { if (-not ($v -like "$ver*")) { throw "Version mismatch: exe $exeV, vst3 $vstV, moduleinfo $miV (want $ver)" } }
"versions: exe $exeV, vst3 $vstV, moduleinfo $miV"

# ---- Binary package ----
$stage = Join-Path $dist "$name $ver"
$zip = Join-Path $dist "$file-$ver-win64.zip"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage\Standalone", "$stage\VST3" | Out-Null
Copy-Item "$art\Standalone\$name.exe" "$stage\Standalone\"
Copy-Item -Recurse "$art\VST3\$name.vst3" "$stage\VST3\"
foreach ($f in 'desktop.ini', 'Plugin.ico') {    # the build may already have put them there, marked system/hidden
    $d = "$stage\VST3\$name.vst3\$f"
    if (Test-Path -LiteralPath $d) { attrib -s -h -r "$d" }
    Copy-Item -Force "$root\Resources\$f" $d
}
Copy-Item "$root\Tools\Install VST3.bat", "$root\LICENSE.txt", "$root\THIRD-PARTY-NOTICES.txt", "$root\README.md" $stage
Copy-Item "$root\Reports\measurements.md" "$stage\MEASUREMENTS.md"
# licenses of the neural engine and model (named in THIRD-PARTY-NOTICES.txt)
Copy-Item "$root\external\deepfilternet3\LICENSE-MIT" "$stage\LICENSE-DeepFilterNet-MIT.txt"
Copy-Item "$root\external\deepfilternet3\LICENSE-APACHE" "$stage\LICENSE-DeepFilterNet-APACHE.txt"
# since 1.1.0 the plugin runs the network itself: no ONNX Runtime file may end up in the package
if (Get-ChildItem -Recurse $stage -Filter '*onnxruntime*') { throw "an ONNX Runtime file ended up in the package" }
$quick = (Get-Content "$root\Tools\QuickStart-template.txt" -Raw).Replace('{VERSION}', $ver) -replace "(?<!`r)`n", "`r`n"
Set-Content -Path "$stage\Quick Start.txt" -Value $quick -NoNewline
attrib +s "$stage\VST3\$name.vst3"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip
"binary zip: $zip $([math]::Round((Get-Item $zip).Length / 1MB, 2)) MB"

# ---- Source package (complete corresponding source) ----
$src = Join-Path $dist "$file-$ver-source"
$szip = Join-Path $dist "$file-$ver-source.zip"
if (Test-Path $src) { Remove-Item -Recurse -Force $src }
New-Item -ItemType Directory -Force $src | Out-Null
foreach ($f in 'CMakeLists.txt', 'README.md', 'LICENSE.txt', 'THIRD-PARTY-NOTICES.txt', '.gitignore') { if (Test-Path "$root\$f") { Copy-Item "$root\$f" $src } }
foreach ($d in 'Source', 'Tests', 'Resources', 'Tools', 'Reports', 'docs') { Copy-Item -Recurse "$root\$d" "$src\$d" }
New-Item -ItemType Directory -Force "$src\external" | Out-Null
Copy-Item -Recurse "$root\external\asiosdk" "$src\external\asiosdk"
Copy-Item -Recurse "$root\external\onnxruntime" "$src\external\onnxruntime"
Copy-Item -Recurse "$root\external\deepfilternet3" "$src\external\deepfilternet3"
# JUCE without its examples and docs (not needed to build; JUCE's own license files are kept)
New-Item -ItemType Directory -Force "$src\external\JUCE" | Out-Null
Get-ChildItem -Force "$root\external\JUCE" | Where-Object { $_.Name -notin @('examples', 'docs', '.github', '.gitlab-ci.yml', '.clang-tidy', '.git') } |
    ForEach-Object { Copy-Item -Recurse $_.FullName "$src\external\JUCE\$($_.Name)" }
if (Test-Path $szip) { Remove-Item -Force $szip }
Compress-Archive -Path $src -DestinationPath $szip
Remove-Item -Recurse -Force $src
"source zip: $szip $([math]::Round((Get-Item $szip).Length / 1MB, 2)) MB"

# ---- Verify the binary zip matches the build ----
Add-Type -AssemblyName System.IO.Compression.FileSystem
$tmp = Join-Path $env:TEMP "vs_zipcheck_$ver"
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
[IO.Compression.ZipFile]::ExtractToDirectory($zip, $tmp)
$ok1 = (Get-FileHash "$art\Standalone\$name.exe").Hash -eq (Get-FileHash "$tmp\$name $ver\Standalone\$name.exe").Hash
$ok2 = (Get-FileHash "$art\VST3\$name.vst3\Contents\x86_64-win\$name.vst3").Hash -eq (Get-FileHash "$tmp\$name $ver\VST3\$name.vst3\Contents\x86_64-win\$name.vst3").Hash
$ok3 = (Test-Path "$tmp\$name $ver\LICENSE.txt") -and (Test-Path "$tmp\$name $ver\LICENSE-DeepFilterNet-MIT.txt")
"exe hash match: $ok1; vst3 hash match: $ok2; LICENSE.txt and model licenses included: $ok3"
Remove-Item -Recurse -Force $tmp
