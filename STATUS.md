# STATUS

Living state. Update after each session (AGENTS.md).

## Milestones

- [x] M1 ESP-NOW ping-pong — CLOSED 2026-09-23, amended acceptance (see
      hardware results below)
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

## M1 hardware results (2026-09-23, bench)

2x WEMOS D1 mini (A ponger E8:DB:84:DC:63:E1, B pinger
E8:DB:84:DC:65:8C), stock PCB antennas, channel 1 (site scan: no APs on
1; nearby APs on 10/11), CCMP unicast, TX 17.5 dBm firmware-pinned
(EIRP 19.5 dBm). Runs 1-2 at ~0.3 m on the desk, run 3 ~1 m apart and
perpendicular. Full logs: firmware/logs/.

| Run | Forward delivery | Round-trip unique | Loss | RTT p50/p95/max | send_fail |
|---|---|---|---|---|---|
| 1 | 1000/1000 (highest_seq 1000) | 994/1000 | 6 | 0/3/42 ms | 0 |
| 2 | 1000/1000 (highest_seq 1000) | 996/1000 | 4 | 0/3/- ms | 0 |
| 3 | 1000/1000 (highest_seq 1000) | 995/1000 | 5 | 0/3/48 ms | 0 |

The ponger received every PING in all runs (15-17 MAC-retransmit
duplicates per run, absorbed correctly). The residual ~0.5% loss lives
exclusively in the unrecovered echo (return) path.

Amendment (owner-approved 2026-09-23): the original "0 packet loss"
criterion assumed a protected return path; M1's one-shot echo has none
by design. Amended acceptance: zero send failures AND 100% forward
delivery AND >= 99% round-trip delivery — met in all three runs. The
remaining ~0.5% is exactly the M2 ARQ work (protocol.md sections 5-6).

Bench findings fixed during the session:

- esp_now_send returned -3 on ESP8266: missing esp_now_set_self_role.
  Fixed in 654bb63 and validated by three full runs.
- PLATFORMIO_BUILD_FLAGS string literals need escaped quotes (\");
  TEST-CHECKLIST.md corrected.

## Session log

- 2026-09-23: market study (internal), README/AGENTS/protocol cleanup,
  M1 design spec (independently reviewed, v2), implementation plan. All plan
  tasks implemented; every task went through the review gate: native
  suite 21/21 across 6 suites, embedded gate 4/4 [SUCCESS], EIRP pinned
  at 17.5 dBm. Awaiting the hardware acceptance run
  (TEST-CHECKLIST.md); M1 closes when its results are pasted here.
- 2026-09-23 (later): hardware acceptance session on the D1 mini pair.
  Bench-found and fixed: missing esp_now_set_self_role (esp_now_send -3),
  build-flag quoting. Three acceptance runs recorded above; M1 closed
  with the owner-approved amended acceptance.
