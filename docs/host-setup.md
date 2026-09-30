# Host setup: rnsd on Raspberry Pi 3B+ (M2)

One-time provisioning per Pi (two boards: flrnode-1, flrnode-2).

## Image the SD card

1. Raspberry Pi Imager → Raspberry Pi OS **Lite** (64-bit).
2. OS customisation (the gear): hostname `flrnode-1` (second Pi:
   `flrnode-2`), enable SSH (password auth), create the user you like,
   configure Wi-Fi or use Ethernet (preferred on the 3B+).
3. Flash, boot, find the Pi on your LAN (`ping flrnode-1.local`).

## Install RNS and access

    sudo usermod -aG dialout $USER   # log out/in after this
    sudo apt install -y python3-pip pipx
    pipx install rns
    pipx ensurepath   # log out/in once more so rnsd is on $PATH

## Configure the SerialInterface

    mkdir -p ~/.reticulum
    cp host/reticulum.conf.example ~/.reticulum/config

NOTE: current RNS (2025-12 images) reads a file named `config` (no
extension) inside the config directory — not `reticulum.conf`.

Edit `port` to the by-id path of the attached D1 mini:

    ls /dev/serial/by-id/

Speed stays 921600 8N1 — the firmware boot line also prints at this
rate; `cat` style terminal noise is normal.

## Run rnsd (acceptance)

Foreground for the acceptance run:

    rnsd -v

Keep it running in tmux or screen (a systemd unit is not required for
M2):

    ~/.local/bin/rnsd   # in tmux/screen, one per Pi

## Bring-up note

Two fresh nodes exchange announce bursts the moment both interfaces
are up — the bridge's TX backlog exists exactly for that; bursts are
normal, not a fault.
