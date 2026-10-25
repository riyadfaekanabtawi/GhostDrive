#!/bin/sh
# Builds the release preset and packs the installed programs into
#   build/release/ghostdrive-<version>-<os>-<arch>.tar.gz (plus a .sha256 file).
# On macOS the tarball also holds GhostDrive.app, with ghostdrive and ghostdrived inside.
# Everything, including vcpkg's build trees and SwiftPM's caches, is written under build/release.
#
# Usage: scripts/package.sh [extra cmake configure arguments]
# for example -DVCPKG_INSTALLED_DIR=... to reuse a vcpkg installed tree.
set -eu

root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build/release"
mkdir -p "$build"

# vcpkg keeps its build trees and package staging in the vcpkg checkout unless told otherwise.
# Downloads go there too unless the caller already chose a place.
: "${VCPKG_DOWNLOADS:=$build/vcpkg/downloads}"
export VCPKG_DOWNLOADS
mkdir -p "$VCPKG_DOWNLOADS"

# On macOS, everything (dependencies, programs, desktop app) targets this version, so the
# release runs on it and later rather than only on the build machine's macOS. 13 is the floor of
# the desktop app (MenuBarExtra and the Window scene).
osx_target=""
if [ "$(uname -s)" = Darwin ]; then
    : "${MACOSX_DEPLOYMENT_TARGET:=13.0}"
    export MACOSX_DEPLOYMENT_TARGET
    osx_target="$MACOSX_DEPLOYMENT_TARGET"
fi

cd "$root"
cmake --preset release -DGHOSTDRIVE_BUILD_TESTS=OFF \
    "-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=$build/vcpkg/buildtrees;--x-packages-root=$build/vcpkg/packages" \
    ${osx_target:+"-DCMAKE_OSX_DEPLOYMENT_TARGET=$osx_target"} \
    ${osx_target:+"-DVCPKG_OVERLAY_TRIPLETS=$root/packaging/vcpkg-triplets"} \
    "$@"
cmake --build --preset release

version="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build/CMakeCache.txt")"
[ -n "$version" ] || { echo "package.sh: no project version in $build/CMakeCache.txt" >&2; exit 1; }

case "$(uname -s)" in
    Darwin) os=macos ;;
    Linux) os=linux ;;
    *) os="$(uname -s | tr '[:upper:]' '[:lower:]')" ;;
esac
case "$(uname -m)" in
    arm64 | aarch64) arch=arm64 ;;
    x86_64 | amd64) arch=x86_64 ;;
    *) arch="$(uname -m)" ;;
esac

name="ghostdrive-$version-$os-$arch"
stage="$build/package"
rm -rf "$stage"
cmake --install "$build" --prefix "$stage/$name" --strip

if [ "$os" = macos ]; then
    app="$(GHOSTDRIVE_BUILD_DIR="$build" GHOSTDRIVE_SWIFT_SCRATCH="$build/swift" \
        GHOSTDRIVE_VERSION="$version" "$root/macos/GhostDrive/make-app.sh" release | tail -n 1)"
    cp -R "$app" "$stage/$name/"
    # Bundle the stripped programs rather than the build tree's, and sign the result again.
    bundled="$stage/$name/$(basename "$app")"
    cp "$stage/$name/bin/ghostdrive" "$stage/$name/bin/ghostdrived" "$bundled/Contents/MacOS/"
    codesign --force --sign - "$bundled"
fi

tarball="$build/$name.tar.gz"
# COPYFILE_DISABLE keeps macOS tar from adding ._ metadata files.
COPYFILE_DISABLE=1 tar -C "$stage" -czf "$tarball" "$name"
if command -v shasum >/dev/null 2>&1; then
    (cd "$build" && shasum -a 256 "$name.tar.gz" >"$name.tar.gz.sha256")
else
    (cd "$build" && sha256sum "$name.tar.gz" >"$name.tar.gz.sha256")
fi
rm -rf "$stage"
echo "$tarball"
