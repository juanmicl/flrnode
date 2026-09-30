#!/bin/sh
# FLRNode host installer — thin wrapper; the real configuration logic
# lives in `flrn setup`. POSIX sh, idempotent, safe to re-run.
#
# Usage: sh scripts/install.sh [--dry-run] [--from-pypi]
#   --dry-run     print every command, change nothing
#   --from-pypi   install flrn from PyPI instead of the local checkout
#                 (works once the owner has published the package)
#
# Installs: pipx (if missing), the flrn CLI (this repo's host/flrn),
# and rns (the Reticulum daemon the bridges feed).
set -eu

DRY_RUN=0
FROM_PYPI=0
for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    --from-pypi) FROM_PYPI=1 ;;
    *) echo "unknown option: $arg (supported: --dry-run, --from-pypi)"; exit 1 ;;
  esac
done

run() {
  echo "+ $*"
  [ "$DRY_RUN" = "1" ] || "$@"
}

# run_root: like run(), but through sudo when not already root (needed
# for the distro package managers in the pipx bootstrap).
run_root() {
  if [ "$(id -u)" = "0" ]; then
    run "$@"
  else
    echo "+ $SUDO $*"
    [ "$DRY_RUN" = "1" ] || "$SUDO" "$@"
  fi
}

# --- root guard -------------------------------------------------------
# pipx apps and the RNS config would land in /root, where the regular
# user's shell and rnsd never see them — run as a normal user.
if [ "$(id -u)" = "0" ] && [ "${FLRN_ALLOW_ROOT:-}" != "1" ]; then
  echo "refusing to run as root: pipx and configs would land in /root;"
  echo "run as a regular user, or set FLRN_ALLOW_ROOT=1 to override"
  exit 1
fi

command -v python3 >/dev/null || { echo "python3 required"; exit 1; }
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)' \
  || { echo "python3 >= 3.10 required"; exit 1; }

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PKG_DIR="$REPO_ROOT/host/flrn"

# --- pipx (preferred installer) --------------------------------------
# PEP 668 (Debian >= 12, Ubuntu >= 23.04, Pi OS Bookworm) makes a plain
# `pip install --user` abort with externally-managed-environment, so
# cascade: distro package (apt-get, then dnf), then pip --user, then
# pip --user --break-system-packages. Every attempt falls through on
# failure (if/||) — only when ALL fail do we bail with the manual hint.
if command -v pipx >/dev/null 2>&1; then
  PIPX=$(command -v pipx)
else
  echo "pipx not found — installing (distro package first, then pip)"
  SUDO=""
  [ "$(id -u)" = 0 ] || SUDO=sudo
  ok=0
  if command -v apt-get >/dev/null 2>&1; then
    if run_root apt-get install -y pipx; then
      ok=1
    elif run_root apt-get update -qq && run_root apt-get install -y pipx; then
      ok=1
    fi
  fi
  if [ "$ok" = 0 ] && command -v dnf >/dev/null 2>&1; then
    if run_root dnf install -y pipx; then
      ok=1
    fi
  fi
  if [ "$ok" = 0 ]; then
    if run python3 -m pip install --user pipx \
       || run python3 -m pip install --user --break-system-packages pipx; then
      ok=1
    fi
  fi
  if [ "$ok" != 1 ]; then
    echo "pipx install failed — install pipx manually: https://pipx.pypa.io"
    exit 1
  fi
  PIPX=$(command -v pipx 2>/dev/null || true)
  [ -n "$PIPX" ] || PIPX="$HOME/.local/bin/pipx"
  if [ "$DRY_RUN" != "1" ] && [ ! -x "$PIPX" ]; then
    echo "pipx bootstrap finished but $PIPX is not executable —" \
         "check PATH and install manually: https://pipx.pypa.io"
    exit 1
  fi
fi

# --- flrn -------------------------------------------------------------
if [ "$FROM_PYPI" = "1" ]; then
  run "$PIPX" install --force flrn
elif [ -f "$PKG_DIR/pyproject.toml" ]; then
  echo "installing flrn from the local checkout: $PKG_DIR"
  run "$PIPX" install --force "$PKG_DIR"
else
  echo "flrn is not on PyPI yet and no local checkout found next to"
  echo "this script — run it from a flrnode clone, or use --from-pypi"
  echo "once the package is published."
  exit 1
fi

# --- rns (Reticulum daemon) -------------------------------------------
# pipx list --short emits "name version" columns (verified on pipx 1.x);
# take column 1. The long format indents package names and would never
# match a ^rns$ grep.
if "$PIPX" list --short 2>/dev/null | awk '{print $1}' | grep -qx 'rns'; then
  echo "rns already installed"
else
  run "$PIPX" install rns
fi

run "$PIPX" ensurepath || true

# --- hand off to the configuration engine ------------------------------
# pipx exposes apps in its bin dir — for user installs that is
# ~/.local/bin, which is NOT always the dir pipx itself lives in
# (distro packages put pipx in /usr/bin), so probe both before
# declaring not-on-PATH.
FLRN_BIN=$(dirname "$PIPX")
if [ "$DRY_RUN" = "1" ]; then
  echo "+ $FLRN_BIN/flrn setup   (skipped: --dry-run)"
  exit 0
fi
if [ -x "$FLRN_BIN/flrn" ]; then
  exec "$FLRN_BIN/flrn" setup
elif [ -x "$HOME/.local/bin/flrn" ]; then
  exec "$HOME/.local/bin/flrn" setup
elif command -v flrn >/dev/null 2>&1; then
  exec flrn setup
fi
echo "flrn installed but not on PATH — run: pipx ensurepath"
echo "(then re-login) and: flrn setup"
exit 1
