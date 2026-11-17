#!/bin/sh
# Make "Patrician III.app" from out/boot (run scripts/build.sh and runtime/build.sh first), with a copy of the game
# files inside it, so it no longer needs the original folder.
#   scripts/app.sh "/path/to/Patrician III" [output folder, default out/]
# The app keeps settings and saves in ~/Library/Application Support/patrician3-mac and logs to
# ~/Library/Logs/Patrician III.log.
set -e
cd "$(dirname "$0")/.."
GAME="$1"
OUT="${2:-out}"
[ -d "$GAME" ] || { echo "usage: scripts/app.sh /path/to/Patrician III [output folder]" >&2; exit 2; }
[ -x out/boot ] || { echo "out/boot missing: run scripts/build.sh and runtime/build.sh first" >&2; exit 1; }
PY=${P3_PYTHON:-python3}
NAME="Patrician III"
APP="$OUT/$NAME.app"
VERSION=$(cat VERSION 2>/dev/null || echo 0.1.0)
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp out/boot "$APP/Contents/MacOS/$NAME"
# The game files. Windows-only extras (the uninstaller, the setup and hardware-test tools) are left out.
ditto "$GAME" "$APP/Contents/Resources/Game"
( cd "$APP/Contents/Resources/Game" && rm -f unins000.dat unins000.exe )
"$PY" scripts/icon.py "$GAME/$(ls "$GAME" | grep -i '^patrician3\.exe$' | head -1)" "$APP/Contents/Resources/AppIcon.icns" \
    || echo "app.sh: no icon (the app gets the default one)" >&2
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>$NAME</string>
    <key>CFBundleDisplayName</key><string>$NAME</string>
    <key>CFBundleExecutable</key><string>$NAME</string>
    <key>CFBundleIdentifier</key><string>local.patrician3-mac</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.strategy-games</string>
    <key>LSArchitecturePriority</key><array><string>arm64</string></array>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict>
</plist>
PLIST
codesign --force --sign - "$APP" 2>/dev/null || echo "app.sh: ad-hoc signing failed (the app still runs when built here)" >&2
echo "ok: $APP"
