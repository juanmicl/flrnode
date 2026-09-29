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

command -v python3 >/dev/null || { echo "python3 required"; exit 1; }
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)' \
  || { echo "python3 >= 3.10 required"; exit 1; }

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PKG_DIR="$REPO_ROOT/host/flrn"

# --- pipx (preferred installer) --------------------------------------
if command -v pipx >/dev/null 2>&1; then
  PIPX=$(command -v pipx)
else
  echo "pipx not found — installing (user site)"
  run python3 -m pip install --user pipx \
    || { echo "pip install failed — install pipx manually: https://pipx.pypa.io"; exit 1; }
  PIPX="$HOME/.local/bin/pipx"
  [ -x "$PIPX" ] || PIPX=$(command -v pipx)
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
# Same resolution path the installer used (pipx exposes apps in its bin
# dir — the directory pipx itself lives in for user installs).
FLRN_BIN=$(dirname "$PIPX")
if [ "$DRY_RUN" = "1" ]; then
  echo "+ $FLRN_BIN/flrn setup   (skipped: --dry-run)"
  exit 0
fi
if [ -x "$FLRN_BIN/flrn" ]; then
  exec "$FLRN_BIN/flrn" setup
elif command -v flrn >/dev/null 2>&1; then
  exec flrn setup
fi
echo "flrn installed but not on PATH — run: $PIPX ensurepath"
echo "(then re-login) and: flrn setup"
exit 1
