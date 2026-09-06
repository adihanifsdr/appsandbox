#!/bin/sh
# Pasang Nestbox ke menu aplikasi pengguna (tanpa root): ikon dari
# docs/brand, entri ke ~/.local/share/applications, peluncur ke ~/.local/bin.
# Servernya sendiri tetap dinyalakan lewat pkexec saat menu diklik.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BRAND="$HERE/../../../docs/brand"
APPS="$HOME/.local/share/applications"
mkdir -p "$APPS" "$HOME/.local/bin"

for s in 32 64 128 256 512; do
    d="$HOME/.local/share/icons/hicolor/${s}x${s}/apps"
    mkdir -p "$d" && cp "$BRAND/nestbox-app-icon-$s.png" "$d/nestbox.png"
done
ln -sfn "$HERE/nestbox-app" "$HOME/.local/bin/nestbox-app"

cat > "$APPS/nestbox.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Nestbox
GenericName=Mesin virtual
Comment=VM dan replica KVM di PC ini, lewat panel Nestbox
Exec=$HERE/nestbox-app
Icon=nestbox
Terminal=false
Categories=System;
Keywords=vm;kvm;qemu;virtual;replica;sandbox;
StartupNotify=true
StartupWMClass=Nestbox
DESKTOP

update-desktop-database "$APPS" 2>/dev/null || true
gtk-update-icon-cache -f -t "$HOME/.local/share/icons/hicolor" 2>/dev/null || true
echo "OK: Nestbox ada di menu aplikasi (Exec=$HERE/nestbox-app)"
