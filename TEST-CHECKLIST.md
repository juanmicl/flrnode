# TEST-CHECKLIST: M1 acceptance (D1 mini pair)

The human runs this and pastes results into STATUS.md. Time: ~20 min.
Boards: two WEMOS D1 mini (label them A and B). Both stay USB-connected
to the PC for the whole run (USB powers them and gives us the logs).

## One-time setup

1. `pipx install platformio` (or `pip install --user platformio`).
2. Serial permissions: `sudo usermod -aG dialout $USER`, then log out
   and back in.
3. `chmod +x scripts/bench_monitor.sh`.

## Phase 1: provision peer MACs (~10 min, once)

1. Plug board A, flash the ponger:
   `cd firmware && pio run -e d1_mini_ponger -t upload`
2. Open the monitor: `pio device monitor -b 115200`. The boot line shows
   `"mac":"XXXXXXXXXXXX"` and `"peer":"UNSET-read-mac-above"`.
   Write down A's MAC. Close the monitor (Ctrl+] or Ctrl+T then Ctrl+X).
3. Unplug A. Plug board B. Flash the pinger:
   `pio run -e d1_mini_pinger -t upload`. Note B's MAC. Close monitor.
4. Rebuild each role pointing at the OTHER board's MAC and reflash.
   For A (ponger, pointing at B):
   `export PLATFORMIO_BUILD_FLAGS='-DFLR_PEER_MAC="<B-MAC>"'`
   `pio run -e d1_mini_ponger -t upload`
   For B (pinger, pointing at A): replug A, then on B:
   `export PLATFORMIO_BUILD_FLAGS='-DFLR_PEER_MAC="<A-MAC>"'`
   `pio run -e d1_mini_pinger -t upload`
   Then `unset PLATFORMIO_BUILD_FLAGS`.
   Use exactly 12 hex digits with no separators (AABBCCDDEEFF form); the
   firmware's MAC parser does not accept AA:BB:CC:DD:EE:FF.

## Phase 2: acceptance run (~3 min)

1. Place both boards 1-3 m apart on the desk, antennas vertical if
   external, orientation noted.
2. From the repo root: `scripts/bench_monitor.sh /dev/ttyUSB0 /dev/ttyUSB1`
   (ponger port first; see note). The script prefixes every line with
   [PONGER]/[PINGER] and tees to logs/. If the port names are swapped,
   pass them in the right order — the script does not auto-detect.
   NOTE: opening a serial port asserts DTR and RESETS a D1 mini, so the
   ponger resets first and the pinger second: the required power-on
   order happens automatically. Watch for both boot lines.
3. Wait ~110 s. The pinger prints:
   `{"t":"m1_result",...,"verdict":"PASS"}` (or FAIL).
   The ponger prints `{"t":"pong_final",...}` a few seconds later.
4. Cross-check: ponger `"rx"` == pinger `"rx"` == 1000, and
   `"loss":0` for PASS. If FAIL, keep both logs and see below.

## Phase 3: record results

Paste into STATUS.md (session log):
- pinger m1_result line + ponger pong_final line
- distance (m), antenna (stock PCB / external), orientation, channel,
  TX power (17.5 dBm, firmware-pinned; note antenna gain -> EIRP),
  date, board names A/B, and the full logs/ paths if anything failed.

## Troubleshooting

- rx == 0 on both: compare `"channel"` in both boot lines; check that
  each peer MAC equals the OTHER board's actual mac; re-flash if unsure.
- Occasional loss on a busy desk: re-run with a phone WiFi analyzer to
  pick a quieter channel, rebuild both with
  `-DFLR_CHANNEL=<n>` in PLATFORMIO_BUILD_FLAGS.
- Permission denied opening ports: dialout group (see one-time setup).

## Verdict rule

M1 passes only with `"verdict":"PASS"` from a real hardware run recorded
in STATUS.md (AGENTS.md hard rule: no simulated hardware results).
