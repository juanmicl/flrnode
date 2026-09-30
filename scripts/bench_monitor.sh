#!/usr/bin/env bash
# Bench monitor: prefixes two serial ports and tees to logs/.
# Usage: scripts/bench_monitor.sh <ponger_port> <pinger_port> [baud]
# Opening a port asserts DTR and resets a D1 mini, so pass the PONGER
# port first: boards restart in the required order. Default baud is
# 921600 (bridge firmware); 115200 was the M1 ping-pong rate.
set -euo pipefail
PONGER=${1:?usage: bench_monitor.sh <ponger_port> <pinger_port> [baud]}
PINGER=${2:?usage: bench_monitor.sh <ponger_port> <pinger_port> [baud]}
BAUD=${3:-921600}
for p in "$PONGER" "$PINGER"; do
  [ -e "$p" ] || { echo "no such port: $p" >&2; exit 1; }
  stty -F "$p" "$BAUD" raw -echo -echoe -echok || true
done
mkdir -p logs
trap 'kill 0' EXIT INT TERM
( awk -v P=PONGER '{ printf "[%s] %s\n", P, $0; fflush(); }' < "$PONGER" \
    | tee logs/ponger.log ) &
sleep 2  # stagger the pinger reset: hardens against the boot race
( awk -v P=PINGER '{ printf "[%s] %s\n", P, $0; fflush(); }' < "$PINGER" \
    | tee logs/pinger.log ) &
wait
