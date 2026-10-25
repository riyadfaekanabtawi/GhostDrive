#!/bin/sh
# Installs GhostDrive from this release folder for the current user only: the programs into
# ~/.local/bin, the menu entry into ~/.local/share/applications and the icon into
# ~/.local/share/icons (under $XDG_DATA_HOME instead of ~/.local/share when it is set).
# It never uses sudo and writes nothing outside the home directory.
set -eu

if [ "$(id -u)" -eq 0 ]; then
    echo "install.sh: run this as your own user, not as root; it installs into your home directory" >&2
    exit 1
fi
[ -n "${HOME:-}" ] || { echo "install.sh: HOME is not set" >&2; exit 1; }

here="$(cd "$(dirname "$0")" && pwd)"
bindir="$HOME/.local/bin"
datadir="${XDG_DATA_HOME:-$HOME/.local/share}"
apps="$datadir/applications"
icons="$datadir/icons/hicolor/scalable/apps"
desktop=org.ghostdrive.GhostDrive.desktop
icon=org.ghostdrive.GhostDrive.svg

mkdir -p "$bindir" "$apps" "$icons"
for prog in ghostdrive ghostdrived ghostdrive-rendezvous ghostdrive-app; do
    # A temporary name and a rename, so a running ghostdrived keeps its old binary intact.
    cp "$here/bin/$prog" "$bindir/.$prog.new"
    chmod 755 "$bindir/.$prog.new"
    mv -f "$bindir/.$prog.new" "$bindir/$prog"
done
cp "$here/share/icons/hicolor/scalable/apps/$icon" "$icons/$icon"
# The full path in Exec, since ~/.local/bin is not on every desktop session's PATH.
sed "s|^Exec=ghostdrive-app|Exec=$bindir/ghostdrive-app|" \
    "$here/share/applications/$desktop" >"$apps/$desktop"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$apps" >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1 && [ -f "$datadir/icons/hicolor/index.theme" ]; then
    gtk-update-icon-cache -q -t "$datadir/icons/hicolor" >/dev/null 2>&1 || true
fi

echo "Installed GhostDrive into $bindir and $datadir."
echo "Open GhostDrive from the application menu, or run $bindir/ghostdrive-app."
case ":${PATH:-}:" in
    *":$bindir:"*) ;;
    *) echo "Add $bindir to PATH to run ghostdrive from a terminal." ;;
esac
