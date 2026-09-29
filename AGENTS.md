# AGENTS.md: FLRNode

You are an AI coding agent working on FLRNode. This file is your single source
of truth. Read it fully before touching code. Human handles all hardware;
you handle code, tests, docs, and automation.

## Mission

FLRNode turns cheap ESP32/ESP8266 boards (+ optional SX1280 modules) into
high-speed, encrypted radio links for the Reticulum Network Stack (RNS).

Tagline: "The 1 Mbps encrypted radio link for makers: $15, 2 km, no license."

Two transports, one core:

- **ESP-NOW tier** (today, zero extra hardware): any ESP32/ESP8266, ~100-300 m
- **FLRC tier** (SX1280 via SPI/RadioLib): 260/520/1040 kbps, FEC, 0.5-2 km

Killer apps: Sideband / MeshChat / NomadNet over RNS (end-to-end crypto),
transparent serial pipe, future Meshtastic 2.4G bridge.

## North-star metrics (every decision serves these)

1. A non-expert goes from "board in drawer" to "encrypted chat working" in
   under 10 minutes (web flasher + one-line host installer).
2. Measured, published benchmarks (nobody else measures; we do).
3. Weekly releases, every issue answered < 24h.

## Repo layout (target)

```
flrnode/
├── firmware/            # PlatformIO workspace (Arduino framework)
│   ├── src/             # app entry, tasks
│   ├── src/core/        # transport-agnostic: HDLC, buffers, stats, LBT
│   ├── src/transport/   # espnow_transport.cpp / flrc_transport.cpp
│   ├── test/            # native unit tests (pio test -e native)
│   └── platformio.ini   # envs: d1_mini, esp32_dev, t3s3, native
├── host/
│   ├── flrn/            # Python CLI: config, stats, link test, installer
│   └── reticulum.conf.example
├── flasher/             # web flasher (esptool-js) — static site
├── docs/                # wiring.md, compliance.md, benchmarks.md (ES/EN)
├── scripts/             # hackrf_verify.py, link_test.sh, release.sh
├── STATUS.md            # living state — UPDATE THIS after each session
└── TEST-CHECKLIST.md    # hardware steps the human must run (you write these)
```

## Architecture (immutable decisions)

```
[USB serial 921600 8N1] ⇄ [HDLC framer] ⇄ [Transport interface]
                                                ├── EspNowTransport
                                                └── FlrcTransport (SX1280)
```

- **Core is transport-agnostic.** Never leak transport specifics above the
  `Transport` interface (pure virtual: `begin`, `send(frame)`, `onFrame(cb)`,
  `stats()`).
- **HDLC framing** exactly as RNS `SerialInterface` expects:
  flag 0x7E, escape 0x7D (0x7E→0x7D 0x5E, 0x7D→0x7D 0x5D), CRC-16/XMODEM
  appended before flag of next frame. Frame limit = transport MTU.
- **MTU chain**: RNS default MTU 500 B → firmware fragments frames >
  transport MTU. ESP-NOW MTU = 250 B; FLRC MTU = 127 B. Reassembly +
  dedup on RX.
- **Fragmentation & ARQ**: `docs/protocol.md` is the normative spec (PDU
  format, window/ACK/NAK state machine, constants, conformance matrix
  P-01..P-20). Implement from that spec only; do not re-derive formats
  here. Golden vectors in test/golden/.
- **LBT before every TX** (RSSI threshold + random backoff 1-15 slots).
  This is a regulatory requirement (see compliance).
- **Stats counters** on every layer (tx/rx frames/bytes, airtime ms, RSSI,
  retransmits) exposed over serial as JSON lines + to `flrn`.

## Regulatory compliance (NON-NEGOTIABLE)

Band: 2400-2483.5 MHz, ETSI EN 300 328 / EU decision 2021/1067.

- Max EIRP **20 dBm (100 mW) including antenna gain**. Default TX power
  12.5 dBm (SX1280 hardware max) with 2 dBi antenna = OK.
- Config UI must NEVER offer settings that exceed EIRP. If user sets antenna
  gain > 7.5 dBi, firmware must auto-reduce TX power and warn.
- LBT/CSMA kept always-on (adaptive equipment requirement).
- Default frequency 2440 MHz (avoids WiFi ch1/ch13 edges).
- `docs/compliance.md` is normative: update it with any RF change.
- Never disable encryption defaults; never implement replay of foreign
  protocols.

## Board matrix & wiring (verify against docs/wiring.md)

| Board | MCU | Transport | Notes |
| --- | --- | --- | --- |
| WEMOS D1 mini | ESP8266 | ESP-NOW | 5 € tier. No LR mode. espnow.h core API |
| ESP32 DevKit | ESP32 | ESP-NOW | LR mode available (802.11 LR, ESP32-to-ESP32 only) |
| LilyGO T3S3 | ESP32-S3 | FLRC | pins: SCK 5, MISO 3, MOSI 6, NSS 7, RST 8, BUSY 36, DIO1 9 |
| Generic + bare SX1280 | ESP32 | FLRC | SPI + BUSY + DIO1; see wiring.md |

ESP8266 quirks: single radio; fix WiFi channel before esp_now_init();
encryption via esp_now_set_kok (PMK/LMK same as ESP32).

## Code conventions

- C++17, PlatformIO. One .h/.cpp per module under src/core or src/transport.
- No dynamic allocation in steady state (pre-allocated ring buffers,
  static pools). No String class in core. millis()-based scheduling,
  no delay() in tasks.
- Every public function has a doc comment stating: thread/task context,
  max blocking time, failure mode.
- Unit tests (native env) mandatory for: HDLC roundtrip, escaping edge
  cases, fragmentation/reassembly, dedup window. Golden vectors live in
  test/golden/. CI must run `pio test -e native` + flrn lint.
- Commits: conventional (feat:/fix:/docs:/test:), present tense, one
  logical change. No secrets, no personal data, no .env files.

## Host side

- `flrn` = Python 3.10+, zero deps beyond stdlib + pyserial (keep it
  installable with `pipx install flrn`). Commands: `flrn scan`,
  `flrn config`, `flrn stats`, `flrn test`, `flrn doctor`,
  `flrn setup`.
- Host installer (`curl -sSL flrnode.sh | sh`): installs rns (`pip install
  rns`), writes a reticulum.conf with the SerialInterface block for the
  detected port, starts rnsd, prints the Sideband next step.
- SerialInterface block must set port speed 921600.

## Web flasher

Static site in /flasher using esptool-js (browser WebSerial). Board
auto-detect via USB VID/PID, firmware selection, progress UI, works on
Chrome/Edge over WebSerial. No build step required to serve (plain
static files). Add boards to the manifest, not to the code.

## Milestones (pick the lowest incomplete; update STATUS.md)

- [x] M1 ESP-NOW ping-pong — closed 2026-09-23 on the D1 mini pair with
      amended acceptance (0 send failures, 100% forward delivery, >= 99%
      round-trip; measured runs in STATUS.md). RSSI printing and the
      esp32 env hardware run move to M2 entry.
- [ ] M2 HDLC bridge: rnsd on two hosts exchanges announces; `rnstatus`
      shows interface; `rnspong`/Sideband chat works.
- [ ] M3 flrn CLI complete + host installer + doctor.
- [ ] M4 FLRC transport (needs SX1280 hardware): fragmentation/ARQ,
      benchmarks 260/520/1040 kbps at 10/50/100/500/1000 m.
- [ ] M5 Web flasher + launch assets (README ES/EN, GIF, benchmark table).

When a milestone needs physical hardware, write the exact steps in
TEST-CHECKLIST.md (flash commands, what to observe, what numbers to
write down and where) and stop. The human runs it and pastes results
into STATUS.md.

## Benchmarks format (docs/benchmarks.md)

Table per rate: distance, LOS/urban, RSSI avg/min, throughput UDP,
latency p50/p95, frame loss %. Every row = one real measurement session,
dated, with board + antenna noted. No simulated numbers, ever.

## Language & docs

Code/comments/issues in English. README bilingual (README.md EN with
ES link, README.es.md). Docs are a product: screenshots, wiring photos,
and GIFs are features. Keep both languages in sync: update both or
neither.

## Hard don'ts

- Don't vendor RadioLib; declare it as a lib_deps dependency.
- Don't touch vendor/, flasher deps, or lockfiles unless the task demands it.
- Don't increase TX power defaults or remove LBT to "make tests pass".
- Don't fake benchmark rows or mark hardware milestones complete
  without human-run checklist results.
- Don't refactor across layers in the same commit as a feature.
