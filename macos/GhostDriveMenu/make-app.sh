#!/bin/sh
# Builds "GhostDrive Menu.app" under .build/ in this directory and writes nothing elsewhere.
# Usage: ./make-app.sh [debug|release]
# The app finds `ghostdrive` on PATH or next to the app. To bundle the tools instead, set
# GHOSTDRIVE_BUILD_DIR to a CMake build directory (for example ../../build/release); its
# cli/ghostdrive and daemon/ghostdrived are copied into the app.
set -eu

cd "$(dirname "$0")"
config="${1:-release}"
swift build -c "$config"
bin="$(swift build -c "$config" --show-bin-path)"

app=".build/GhostDrive Menu.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$bin/GhostDriveMenu" "$app/Contents/MacOS/GhostDriveMenu"

if [ -n "${GHOSTDRIVE_BUILD_DIR:-}" ]; then
    cp "$GHOSTDRIVE_BUILD_DIR/cli/ghostdrive" "$GHOSTDRIVE_BUILD_DIR/daemon/ghostdrived" \
        "$app/Contents/MacOS/"
fi

cat > "$app/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>GhostDriveMenu</string>
    <key>CFBundleIdentifier</key>
    <string>org.ghostdrive.menu</string>
    <key>CFBundleName</key>
    <string>GhostDrive Menu</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>0.1</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>LSUIElement</key>
    <true/>
</dict>
</plist>
EOF

# Ad-hoc signature so the bundle launches on Apple silicon; not for distribution.
codesign --force --sign - "$app" >/dev/null 2>&1 || true
echo "$PWD/$app"
