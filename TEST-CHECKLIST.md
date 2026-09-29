# TEST-CHECKLIST: M3 acceptance (flrn CLI + config + persistence)

The human runs this on the bench and pastes results into STATUS.md.
Time: ~30 min (plus one clean-OS installer run). M3 closes only when
the gate results are pasted.

## Phase 0: prerequisites

- The M2 bridge firmware still flashes and boots (M2 gate 2 already
  proved the pair talks).
- flrn installed on the bench host from this checkout:
  `pipx install --force host/flrn` (or `sh scripts/install.sh`).
- Both D1 minis (A = E8:DB:84:DC:63:E1, B = E8:DB:84:DC:65:8C), each
  flashed with the bridge firmware pointing at the OTHER board:

      Plug A only:
      PLATFORMIO_BUILD_FLAGS='-DFLR_PEER_MAC=\"E8DB84DC658C\"' pio run -e d1_mini_bridge -t upload
      Plug B only:
      PLATFORMIO_BUILD_FLAGS='-DFLR_PEER_MAC=\"E8DB84DC63E1\"' pio run -e d1_mini_bridge -t upload

  (escaped quotes mandatory — SCons strips bare quotes; both plugs
  back in for the gates)

## Gate 1: suites green (pre-handoff, re-run to confirm)

    cd firmware && pio test -e native          # 89/89 in 11 suites
    pio run -e d1_mini_pinger -e d1_mini_ponger -e d1_mini_bridge \
        -e esp32_dev_pinger -e esp32_dev_ponger -e esp32_dev_bridge
                                             # 6x SUCCESS
    cd ../host/flrn && python -m pytest      # 43 passed

## Gate 2: flrn smoke over the air

1. `flrn scan` — BOTH bridges listed, fw `m3`, mode `bridge`,
   channel 1, correct MACs.
2. `flrn stats /dev/ttyUSB0` — the 14-key table prints (all counters
   visible).
3. `flrn config set stats_period 2 --all-bridges` — reply carries
   `"effective":2` on both ports; NO reset drop (immediate effect).
4. `flrn config set tx_power 30 --all-bridges` — reply carries
   `"effective":17` on both (EIRP clamp; 17, not 17.5 — the config
   store caps at integer 17 dBm). Then `flrn config get tx_power
   --port /dev/ttyUSB0` → `{"tx_power":17}`.
5. **Reboot BOTH bridges** (unplug/replug). Boot lines show the
   PERSISTED values and NO `"cfg":"default"` field (tx_power 17 now
   survives in EEPROM). `flrn config get tx_power --port …` still
   returns 17 — persistence proven.
6. `flrn config set channel 6 --all-bridges` — `"effective":6` both;
   power-cycle both; boot lines show `"channel":6`;
   `flrn test /dev/ttyUSB0 /dev/ttyUSB1` → PASS on the new channel.
7. **PMK rotation**: `flrn config set pmk
   00112233445566778899AABBCCDDEEFF --all-bridges --yes` — bare
   `{"ok":true}` (never echoes the key). The link re-keys:
   `flrn test …` → PASS. **Reset ONE bridge** (either) — link returns:
   `flrn test …` → PASS again (PMK survived in EEPROM on the reset
   side — persistence discriminator).
8. `flrn setup` — writes/validates `~/.reticulum/config` (an existing
    config is kept). Do this before the doctor step: rnsd is stopped
    during these gates, so the topology row is OK only via a valid
    config dir.
9. `flrn doctor` — every row OK (ports attached, fw m3, config valid,
    topology healthy).
10. Roll back: `flrn config set channel 1 --all-bridges` (leave the
    bench on channel 1 for the next session).

## Gate 3: installer clean-environment run

In a fresh Debian 12 container or VM (podman: `podman run -it --rm
--device=/dev/ttyUSB0 debian:12`):

    apt update && apt install -y python3 python3-pip git usbutils psmisc
    git clone https://github.com/juanmicl/flrnode && cd flrnode
    sh scripts/install.sh            # twice — second run must say
                                     # "rns already installed"

Owner-only (not the bench gate): publish the package with
`cd host/flrn && python -m build && python -m twine upload dist/*`,
then `sh scripts/install.sh --from-pypi` in another clean env.

## Troubleshooting

- `OldFirmwareError` / "predates get/set": the bridge runs pre-M3
  firmware — reflash Phase 0.
- "BUSY … rnsd is using": stop rnsd (`systemctl --user stop rnsd`) or
  pass the port explicitly; flrn never fights rnsd.
- Channel change lost after reboot: boot line shows `"cfg":"default"`
  → the EEPROM image was corrupt at boot; re-run the set.
- Pair DESYNCED after a partial set: re-run the SAME set on both
  bridges (the error names what applied where).
- flrn test FAIL after a channel move: both boot lines must show the
  SAME channel; mismatch = stored-credential drift → power-cycle both.

## Verdict rule

M3 passes only with Gate 2 all-PASS (incl. the PMK reset
discriminator) + Gate 3 installer run recorded in STATUS.md by the
human (AGENTS hard rule: no simulated results).
