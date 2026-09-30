# Radio & regulatory compliance

Normative for any RF change (AGENTS.md). Band: 2400-2483.5 MHz,
ETSI EN 300 328 / EU decision 2021/1067.

## EIRP

- Max 20 dBm (100 mW) including antenna gain.
- Firmware defaults: ESP-NOW tier TX power is pinned at 17.5 dBm in
  firmware (per transport begin); with a 2 dBi antenna EIRP =
  19.5 dBm, within the 20 dBm envelope. FLRC tier will
  default to 12.5 dBm (SX1280 hardware max) with a 2 dBi antenna.
- M3 bridge firmware: `tx_power` is settable over the CTRL channel.
  ConfigStore clamps to 17 dBm conducted on BOTH the set path and the
  boot-load path (the set reply carries the effective value); the
  loaded value is applied at `esp_now_init`, before the first
  transmit. A stored config therefore never exceeds 17 dBm + 2 dBi =
  19 dBm EIRP, inside the envelope. The 17.5 dBm pin above remains
  the M1 ping-pong firmware default. An antenna-gain key (for
  antennas above 2 dBi) arrives with the FLRC tier in M4.
- If antenna gain > 7.5 dBi is configured, firmware must auto-reduce TX
  power and warn. Configuration surfaces must never offer settings that
  exceed EIRP.

## Link encryption keys

- ESP-NOW CCMP keys come from the config store: `flrn config set pmk
  <32 hex>` persists a 16-byte key applied to the radio at boot (as
  PMK and, in the current single-peer model, as the peer LMK). A
  stored zero key falls back to the compiled-in development
  placeholders, and the boot line warns while that is the case.
- The placeholders are public (in this repository). L1
  confidentiality with default keys is therefore nil; the
  confidentiality guarantee is Reticulum's end-to-end crypto, which
  CCMP only complements.
- Rotation: set the same pmk on both bridges (`--all-bridges`) and
  reboot each once.

## Listen-before-talk

- ESP-NOW tier: transmissions are ordinary IEEE 802.11 frames; the WiFi
  MAC performs CSMA/CCA before every transmission. This inherited
  adaptivity is the EN 300 328 (broadband modulation equipment)
  compliance basis for the ESP-NOW tier. No separate firmware LBT module
  exists on this tier.
- FLRC tier (SX1280, from M4): explicit LBT before every TX: channel scan
  with RSSI threshold, random backoff 1-15 slots of 1 ms, give up after
  8 attempts and requeue without consuming an ARQ retry
  (protocol.md section 7.2).

## Channel plan

- FLRC default 2440 MHz (clear of busy WiFi ch1/ch13 edges).
- ESP-NOW default WiFi channel 1, fixed at boot from config and printed
  on the boot line so operators can verify both nodes agree
  (stored-credential drift shows up here; see TEST-CHECKLIST.md).

## Review rule

Any RF-affecting change (power, rate, channel, LBT behavior) requires a
commit updating this file in the same change set.
