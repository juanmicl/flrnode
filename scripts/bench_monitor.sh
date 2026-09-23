#!/usr/bin/env bash
# Bench monitor: prefixes two serial ports and tees to logs/.
# Usage: scripts/bench_monitor.sh <ponger_port> <pinger_port>
# Opening a port asserts DTR and resets a D1 mini, so pass the PONGER
# port first: boards restart in the required order.
set -euo pipefail
PONGER=${1:?usage: bench_monitor.sh <ponger_port> <pinger_port>}
PINGER=${2:?usage: bench_monitor.sh <ponger_port> <pinger_port>}
for p in "$PONGER" "$PINGER"; do
  [ -e "$p" ] || { echo "no such port: $p" >&2; exit 1; }
  stty -F "$p" 115200 raw -echo -echoe -echok || true
done
mkdir -p logs
trap 'kill 0' EXIT INT TERM
( awk -v P=PONGER '{ printf "[%s] %s\n", P, $0; fflush(); }' < "$PONGER" \
    | tee logs/ponger.log ) &
( awk -v P=PINGER '{ printf "[%s] %s\n", P, $0; fflush(); }' < "$PINGER" \
    | tee logs/pinger.log ) &
wait
