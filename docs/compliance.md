# Radio & regulatory compliance

Normative for any RF change (AGENTS.md). Band: 2400-2483.5 MHz,
ETSI EN 300 328 / EU decision 2021/1067.

## EIRP

- Max 20 dBm (100 mW) including antenna gain.
- Firmware defaults: ESP-NOW at the radio's default power; FLRC tier will
  default to 12.5 dBm (SX1280 hardware max) with a 2 dBi antenna.
- If antenna gain > 7.5 dBi is configured, firmware must auto-reduce TX
  power and warn. Configuration surfaces must never offer settings that
  exceed EIRP.

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
