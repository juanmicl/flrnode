# STATUS

Living state. Update after each session (AGENTS.md).

## Milestones

- [x] M1 ESP-NOW ping-pong — CLOSED 2026-09-23, amended acceptance (see
      hardware results below)
- [x] M2 HDLC bridge — CLOSED 2026-09-28, amended acceptance (see
      hardware results below)
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

## M2 hardware results (2026-09-28, bench)

Same D1 mini pair (A ponger→bridge E8:DB:84:DC:63:E1, B pinger→bridge
E8:DB:84:DC:65:8C), channel 1, CCMP unicast, TX 17.5 dBm firmware-pinned.
Hosts: single bench PC running two isolated RNS instances (see amendment).

| Gate | Result | Evidence |
|---|---|---|
| G1 native | PASS | 70/70 in 10 suites (post wire-format fix), 6/6 embedded envs |
| G2 bridge bench | PASS (re-run) | 9/9 frames A->B in-order, 3/3 B->A; ARQ stats: 8 retries, 6 NAKs, 0 dropped — the M1 ~0.5% return-path loss recovered as designed |
| G3 Reticulum E2E | PASS | flrpong (scripts/flrpong.py): two E2E encrypted RNS links over the radio; "PING sent / RECV 'ping from X' / PONG (proof) received — ROUND TRIP COMPLETE" on BOTH sides (4.94 s RTT each direction) |

Amendment (owner-approved 2026-09-28): gate 3 ran on a single PC with two
isolated RNS instances (separate config dirs, SerialInterface-only, no
AutoInterface — announces cross the radio only) instead of two Raspberry
Pi hosts. Cause: rpi-imager bug raspberrypi/rpi-imager#1439 (OS
customisations silently not applied on reflash) plus Pi WiFi provisioning
friction (rfkill persistence, wizard errors). The two Pi 3B+ remain the
target for the permanent deployment; nothing in the firmware depends on
the host choice.

Bench-found defects fixed during the session (each invisible without
hardware):

- RNS serial wire format: no FCS, no inner type byte (contrary to
  protocol.md section 2 as written). Protocol amended; firmware fixed in
  725a890 (hdlc + bridge_app + tests + golden regeneration + harness).
- RNS config format (2025-12 images): file must be named `config`
  (not `reticulum.conf`), interfaces under `[interfaces]`, keys
  `port`/`speed` (not `serial_port`/`serial_speed`). conf.example and
  host-setup.md corrected.
- rpi-imager #1439 workaround: cloud-init NoCloud `user-data` provisioning
  (scripts/fix-pi-sd-wifi.sh for card-side WiFi repair; sd provisioning
  notes in host-setup.md).
- RNS announce-handler API: handlers are objects with `aspect_filter`
  and a `received_announce` method — plain functions are silently
  ignored. flrpong implements the object form.

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
- 2026-09-24 (M2 build): full L2 per protocol.md (pdu codec, fragmenter,
  ARQ window/backlog/NAK) + bridge app + host artifacts implemented via
  the 12-task reviewed plan. Native suite 68/68 in 10 suites, six embedded
  envs green. Awaiting TEST-CHECKLIST gates 2 and 3.
- 2026-09-28 (M2 close): gates 1-3 PASS. Wire-format fix wave (725a890:
  RNS serial = flag+escape+flag, no FCS/type — protocol section 2 amended);
  gate 3 on single PC w/ two isolated RNS instances (amendment, rpi-imager
  bug #1439); conf.example + host-setup corrected to RNS 2025-12 format;
  flrpong added to scripts/. M2 CLOSED.
