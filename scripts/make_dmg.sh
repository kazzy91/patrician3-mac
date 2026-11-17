#!/bin/sh
# Make the Setup disk image: out/Patrician-III-Setup-<version>.dmg with "Patrician III Setup.app".
#   scripts/make_dmg.sh
# The image holds no game code or data: Setup translates and compiles the game on the user's Mac from their own copy
# (setup/install.sh). Inside the app: this repository's translator, runtime and manifests, and capstone (pylib/).
set -e
cd "$(dirname "$0")/.."
VERSION=$(cat VERSION)
CAP=$(ls -d .venv/lib/python3*/site-packages/capstone 2>/dev/null | head -1)
[ -n "$CAP" ] || scripts/setup.sh
CAP=$(ls -d .venv/lib/python3*/site-packages/capstone | head -1)
STAGE=out/dmg
APP="$STAGE/Patrician III Setup.app"
rm -rf "$STAGE"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/payload"
clang -arch arm64 -mmacosx-version-min=12.0 -O2 -fobjc-arc -Wall setup/Setup.m -framework Cocoa \
    -o "$APP/Contents/MacOS/Patrician III Setup"

P="$APP/Contents/Resources/payload"
mkdir -p "$P/recomp" "$P/runtime" "$P/manifest" "$P/scripts" "$P/pylib"
cp recomp/recomp.py recomp/p3_build.py recomp/runtime.h "$P/recomp/"
cp runtime/*.c runtime/*.h runtime/*.m runtime/build.sh "$P/runtime/"
cp manifest/*.tsv "$P/manifest/"
cp scripts/build.sh scripts/app.sh scripts/icon.py "$P/scripts/"
cp setup/install.sh VERSION LICENSE LICENSE.nfs-most-wanted-mac NOTICE "$P/"
cp -R "$CAP" "$P/pylib/"
rm -rf "$P/pylib/capstone/__pycache__"
cp "$(dirname "$CAP")"/capstone-*.dist-info/LICENSE* "$P/pylib/capstone/" 2>/dev/null || true

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Patrician III Setup</string>
    <key>CFBundleExecutable</key><string>Patrician III Setup</string>
    <key>CFBundleIdentifier</key><string>local.patrician3-mac.setup</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>LSMinimumSystemVersion</key><string>12.0</string>
    <key>LSArchitecturePriority</key><array><string>arm64</string></array>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
codesign --force --deep --sign - "$APP"
DMG="out/Patrician-III-Setup-$VERSION.dmg"
rm -f "$DMG"
hdiutil create -quiet -volname "Patrician III Setup" -srcfolder "$STAGE" -fs HFS+ -format UDZO "$DMG"
shasum -a 256 "$DMG" | sed 's|out/||' > "$DMG.sha256"
echo "ok: $DMG ($(du -h "$DMG" | cut -f1))"
