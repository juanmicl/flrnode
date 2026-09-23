# STATUS

Living state. Update after each session (AGENTS.md).

## Milestones

- [ ] M1 ESP-NOW ping-pong (code complete; awaiting hardware run)
- [ ] M2 HDLC bridge to rnsd
- [ ] M3 flrn CLI + host installer
- [ ] M4 FLRC transport + benchmarks
- [ ] M5 Web flasher + launch

## M1 amendments

These amendments stand on their own as the binding M1 scope decisions.

- Acceptance runs unicast + CCMP on the D1 mini pair. Broadcast was
  dropped: no MAC ACK made 0%-loss environment-dependent, and unencrypted
  broadcast deviated from the encryption-default rule.
- RSSI is not available on ESP8266, and the pinned ESP32 core (2.x)
  callback does not expose it either; ESP32 prints RSSI once the project
  moves to core 3.x (transport branch ready, ifdef'd). "rssi" omitted on
  d1_mini for M1.
- ESP-NOW LBT basis: inherited 802.11 MAC CSMA/CCA (docs/compliance.md).
  The blanket "LBT before every TX" wording in AGENTS.md applies to the
  FLRC tier from M4.
- esp32_dev envs are build-only until a DevKit exists.

## Session log

- 2026-09-23: market study (internal), README/AGENTS/protocol cleanup,
  M1 design spec (independently reviewed, v2), implementation plan. All plan
  tasks implemented; every task went through the review gate: native
  suite 21/21 across 6 suites, embedded gate 4/4 [SUCCESS], EIRP pinned
  at 17.5 dBm. Awaiting the hardware acceptance run
  (TEST-CHECKLIST.md); M1 closes when its results are pasted here.
