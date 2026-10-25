#!/bin/sh
# Builds GhostDrive.app (the main window and the menu-bar extra) under .build/ in this directory
# and writes nothing elsewhere, then prints the app's path.
# Usage: ./make-app.sh [debug|release]
# The app finds `ghostdrive` inside the bundle, next to the app or on PATH. To bundle the tools,
# set GHOSTDRIVE_BUILD_DIR to a CMake build directory (for example ../../build/release); its
# cli/ghostdrive and daemon/ghostdrived are copied into the app.
# GHOSTDRIVE_SWIFT_SCRATCH, an absolute path, moves .build and SwiftPM's caches and
# configuration there instead (scripts/package.sh points it into the CMake build tree).
# GHOSTDRIVE_VERSION sets the bundle version; by default it is the project() version in the
# top-level CMakeLists.txt.
set -eu

cd "$(dirname "$0")"
config="${1:-release}"
version="${GHOSTDRIVE_VERSION:-}"
if [ -z "$version" ]; then
    version="$(sed -n 's/^project([^)]* VERSION \([0-9][0-9.]*\).*/\1/p' ../../CMakeLists.txt)"
fi
[ -n "$version" ] || { echo "make-app.sh: no project version in ../../CMakeLists.txt" >&2; exit 1; }
scratch="${GHOSTDRIVE_SWIFT_SCRATCH:-}"
if [ -n "$scratch" ]; then
    set -- --scratch-path "$scratch" --cache-path "$scratch/cache" \
        --config-path "$scratch/config" --security-path "$scratch/security" --manifest-cache local
else
    set --
    scratch="$PWD/.build"
fi
swift build -c "$config" "$@"
bin="$(swift build -c "$config" "$@" --show-bin-path)"

app="$scratch/GhostDrive.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$bin/GhostDriveApp" "$app/Contents/MacOS/GhostDriveApp"

if [ -n "${GHOSTDRIVE_BUILD_DIR:-}" ]; then
    cp "$GHOSTDRIVE_BUILD_DIR/cli/ghostdrive" "$GHOSTDRIVE_BUILD_DIR/daemon/ghostdrived" \
        "$app/Contents/MacOS/"
fi

# The identifier stays the one of the earlier menu-bar app, so its settings carry over.
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>GhostDriveApp</string>
    <key>CFBundleIdentifier</key>
    <string>org.ghostdrive.menu</string>
    <key>CFBundleName</key>
    <string>GhostDrive</string>
    <key>CFBundleDisplayName</key>
    <string>GhostDrive</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>$version</string>
    <key>CFBundleVersion</key>
    <string>$version</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.utilities</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
EOF

# Ad-hoc signature so the bundle launches on Apple silicon; not for distribution.
codesign --force --sign - "$app" >/dev/null 2>&1 || true
echo "$app"
