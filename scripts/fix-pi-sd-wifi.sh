#!/bin/bash
# FLRNode: card-side WiFi repair for a Raspberry Pi OS (cloud-init) SD.
#
# Root cause it fixes (bench-proven, 2026-09): Raspberry Pi OS 2025-12
# images can persist an rfkill soft-block on wlan ("device not available"
# to NetworkManager), and first-boot WiFi needs the regulatory domain set
# before NM starts. Fixes are applied directly to the mounted SD — no
# keyboard/HDMI on the Pi needed.
#
# Usage (from the bench PC, with the SD's rootfs mounted):
#   sudo bash scripts/fix-pi-sd-wifi.sh <rootfs-mount> <SSID> <PSK>
# Example:
#   sudo bash scripts/fix-pi-sd-wifi.sh /media/$USER/rootfs "MyWifi" "pass"
#
# What it does:
#   1. Deletes persisted rfkill wlan block state (unblocks the radio)
#   2. Writes a NetworkManager profile for the given SSID (autoconnect,
#      priority 10) — use your 2.4 GHz SSID for maximum robustness
#   3. Installs a oneshot unit setting the wireless regdomain to ES
#      before NetworkManager starts (fixes 5 GHz passive-scan channels)
set -euo pipefail

ROOT=${1:?usage: fix-pi-sd-wifi.sh <rootfs-mount> <SSID> <PSK>}
SSID=${2:?missing SSID}
PSK=${3:?missing PSK}
NMPROF="$ROOT/etc/NetworkManager/system-connections"
[ -d "$NMPROF" ] || { echo "ERROR: $NMPROF no existe — ¿está montada la partición rootfs en $ROOT?" >&2; exit 1; }

echo "== 1. Eliminando estado rfkill del WiFi (bloqueo persistido):"
rm -fv "$ROOT/var/lib/systemd/rfkill/"platform-*:wlan 2>/dev/null || true

echo "== 2. Perfil NetworkManager para '$SSID' (autoconnect, prioridad 10):"
cat > "$NMPROF/flr-wifi-sd.nmconnection" <<EOF
[connection]
id=flr-wifi-sd
type=wifi
autoconnect=true
autoconnect-priority=10
[wifi]
mode=infrastructure
ssid=$SSID
[wifi-security]
key-mgmt=wpa-psk
psk=$PSK
[ipv4]
method=auto
EOF
chown root:root "$NMPROF/flr-wifi-sd.nmconnection"
chmod 600 "$NMPROF/flr-wifi-sd.nmconnection"
echo "   $(ls -la "$NMPROF/flr-wifi-sd.nmconnection")"

echo "== 3. Unidad regdom (iw reg set ES antes de NetworkManager):"
cat > "$ROOT/etc/systemd/system/flr-regdom.service" <<'EOF'
[Unit]
Description=Set wireless regulatory domain (FLRNode provisioning)
Before=NetworkManager.service
[Service]
Type=oneshot
ExecStart=/usr/sbin/iw reg set ES
RemainAfterExit=yes
[Install]
WantedBy=network-pre.target
EOF
mkdir -p "$ROOT/etc/systemd/system/network-pre.target.wants"
ln -sf /etc/systemd/system/flr-regdom.service \
      "$ROOT/etc/systemd/system/network-pre.target.wants/flr-regdom.service"
echo "   $(ls -la "$ROOT/etc/systemd/system/network-pre.target.wants/flr-regdom.service")"

echo "== Sincronizando..."
sync
echo
echo "VERIFICACIÓN:"
echo "   rfkill wlan restante: $(ls "$ROOT/var/lib/systemd/rfkill/" 2>/dev/null | grep -c wlan || true) (debe ser 0)"
echo "   perfiles NM: $(ls "$NMPROF")"
echo "LISTO — expulsa la SD, arranca la Pi y espera ~90 s antes de escanear"
