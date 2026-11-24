#!/bin/sh
# Builds the Linux beta zip from a finished Release build (Linux, x86-64):
#   dist/OpenVocalStudio-<ver>-linux-x64-beta.zip   VST3 + LV2 + standalone app + docs + license
# The source zip is the same for every platform: make it on Windows with Tools/package.ps1.
# Usage (from the project folder):
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
#   sh Tools/package-linux.sh
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
ver="$(sed -n 's/^project(VocalStudio VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
art="$root/build/VocalStudio_artefacts/Release"
name="Open Vocal Studio"
dist="$root/dist"
stage="$dist/$name $ver"
zip="$dist/OpenVocalStudio-$ver-linux-x64-beta.zip"

# ---- Version check: the VST3 moduleinfo and the LV2 manifest must report the build version ----
miV="$(sed -n 's/.*"Version"[^"]*"\([^"]*\)".*/\1/p' "$art/VST3/$name.vst3/Contents/Resources/moduleinfo.json" | head -1)"
case "$miV" in "$ver"*) ;; *) echo "Version mismatch: moduleinfo $miV (want $ver)" >&2; exit 1 ;; esac
lv2V="$(grep -o 'lv2:\(minor\|micro\)Version [0-9]*' "$art/LV2/$name.lv2/dsp.ttl" | awk '{print $2}' | tr '\n' '.' | sed 's/\.$//')"
echo "versions: vst3 moduleinfo $miV, lv2 minor.micro $lv2V"

# ---- Stage (binaries stripped of debug symbols) ----
rm -rf "$stage" "$zip"
mkdir -p "$stage/Standalone" "$stage/VST3" "$stage/LV2"
cp "$art/Standalone/$name" "$stage/Standalone/"
cp -R "$art/VST3/$name.vst3" "$stage/VST3/"
cp -R "$art/LV2/$name.lv2" "$stage/LV2/"
strip --strip-unneeded "$stage/Standalone/$name" "$stage/VST3/$name.vst3/Contents/x86_64-linux/$name.so" "$stage/LV2/$name.lv2/lib$name.so"
chmod 755 "$stage/Standalone/$name"
cp "$root/Tools/install-linux.sh" "$stage/install.sh"
chmod 755 "$stage/install.sh"
cp "$root/LICENSE.txt" "$root/THIRD-PARTY-NOTICES.txt" "$root/README.md" "$stage/"
cp "$root/Reports/measurements.md" "$stage/MEASUREMENTS.md"
# licenses of the neural engine's model (named in THIRD-PARTY-NOTICES.txt)
cp "$root/external/deepfilternet3/LICENSE-MIT" "$stage/LICENSE-DeepFilterNet-MIT.txt"
cp "$root/external/deepfilternet3/LICENSE-APACHE" "$stage/LICENSE-DeepFilterNet-APACHE.txt"
sed "s/{VERSION}/$ver/g" "$root/Tools/INSTALL-LINUX-template.txt" > "$stage/INSTALL.txt"

# ---- Zip (python keeps the executable permissions) ----
( cd "$dist" && python3 - "$name $ver" "$zip" <<'EOF'
import os, sys, zipfile
folder, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, dirs, files in os.walk(folder):
        dirs.sort()
        z.write(d)
        for f in sorted(files):
            z.write(os.path.join(d, f))
EOF
)
echo "linux zip: $zip $(du -m "$zip" | cut -f1) MB"

# ---- Verify the zip matches the staged files ----
tmp="$(mktemp -d)"
python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$zip" "$tmp"
ok=yes
for f in "Standalone/$name" "VST3/$name.vst3/Contents/x86_64-linux/$name.so" "LV2/$name.lv2/lib$name.so" "LICENSE.txt" "THIRD-PARTY-NOTICES.txt" "install.sh" \
         "LICENSE-DeepFilterNet-MIT.txt" "LICENSE-DeepFilterNet-APACHE.txt"; do
    cmp -s "$stage/$f" "$tmp/$name $ver/$f" || { echo "MISMATCH: $f" >&2; ok=no; }
done
glibc="$(objdump -T "$stage/Standalone/$name" "$stage/VST3/$name.vst3/Contents/x86_64-linux/$name.so" "$stage/LV2/$name.lv2/lib$name.so" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
echo "zip contents match: $ok; newest glibc symbol needed: $glibc"
rm -rf "$tmp"
[ "$ok" = yes ]
