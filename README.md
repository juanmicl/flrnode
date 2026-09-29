# FLRNode

**The encrypted 1 Mbps radio link for makers: $15, 2 km, no license.**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg?style=for-the-badge)](LICENSE)
[![PlatformIO](https://img.shields.io/badge/build-PlatformIO-orange?style=for-the-badge)](firmware/)

[README en español](README.es.md)

## Why

Off-grid options today force a trade-off:

| | Range | Bandwidth |
| --- | --- | --- |
| LoRa 868/915 | several km 🟢 | 0.3-20 kbps 🔴 |
| WiFi | 30-100 m 🔴 | 100+ Mbps 🟢 |

**FLRNode fills the gap in the middle: hundreds of meters to 2 km, at up to 1 Mbps, encrypted, legal in the EU (and anywhere 2.4 GHz ISM is).**

It does this with two transports on one firmware:

- **ESP-NOW tier ($0 extra)**: the ESP32 (or even a $5 ESP8266) already in your drawer. ~100-300 m, millisecond latency.
- **FLRC tier (one $10 module)**: Semtech SX1280's almost-forgotten *Fast Long Range Communication* mode, FSK with turbo FEC designed for drone racing. 260/520/1040 kbps, 0.5-2 km. Almost nobody uses it. We do.

On top of that link runs [Reticulum](https://github.com/markqvist/Reticulum), a cryptography-first mesh networking stack, so you get end-to-end encrypted messaging, file transfer and more, out of the box, with **zero configuration**.

## Quick start

Two WEMOS D1 minis, two hosts (or one host + one Raspberry Pi):

```sh
git clone https://github.com/juanmicl/flrnode && cd flrnode
sh scripts/install.sh            # installs flrn (host CLI) + rns, runs flrn setup
```

Flash both bridges (firmware/; peer MAC comes from the boot line — see
`TEST-CHECKLIST.md`):

```sh
pio run -e d1_mini_bridge -t upload
```

Then:

```sh
flrn scan                                   # find bridges: fw, mode, channel, MAC
flrn stats /dev/ttyUSB0                     # live ARQ counters (--watch to poll)
flrn config set channel 6 --all-bridges     # apply + persist on both bridges
flrn test /dev/ttyUSB0 /dev/ttyUSB1         # over-air link test + verdict
flrn doctor                                 # health checks with fixes
```

Requires: a FLRNode bridge on USB (CH340), Python 3.10+, and `rnsd`
running on each host (`docs/host-setup.md`).
*(M5: the one-line `curl -sSL flrnode.sh | sh` installer — today, run
the script from a checkout.)*

## Status

M1 (ESP-NOW transport) and M2 (Reticulum bridge) are closed with
bench-verified results. M3 (`flrn` CLI, this quick start) is
code-complete with a headless integration suite; M4 brings the FLRC
tier and published benchmarks.

## Supported hardware

| Board | Cost | Transports | Notes |
| --- | --- | --- | --- |
| WEMOS D1 mini (ESP8266) | ~$5 | ESP-NOW | the cheapest encrypted radio link you can build |
| Any ESP32 DevKit | ~$7 | ESP-NOW (+LR mode) | best zero-extra-hardware range |
| LilyGO T3S3 (SX1280) | ~$25 | ESP-NOW + FLRC | all-in-one, OLED, battery |
| ESP32 + bare SX1280 module | ~$17 | ESP-NOW + FLRC | DIY wiring: SPI + BUSY + DIO1 |

## What you build with it

- 🔐 Off-grid encrypted comms: [Sideband](https://github.com/markqvist/Sideband) / [MeshChat](https://github.com/liamcottle/reticulum-meshchat) / [NomadNet](https://github.com/markqvist/NomadNet) over radio
- 🔌 Transparent serial pipe: replace a cable across a farm, venue or building
- 📡 High-speed mesh backbone: link Reticulum nodes that are too close for LoRa's patience and too far for WiFi's legs
- 🤖 Robot/drone telemetry: FLRC was designed for this. ms latency, FEC, 1 Mbps *(roadmap)*

## How it works

```
[Host A]                                [Host B]
  rnsd ── SerialInterface (HDLC)          rnsd
    │ USB serial @ 921600                  ▲
    ▼                                       │
┌─────────────────────────────┐   ┌────────┴──────────┐
│ FLRNode firmware            │   │ FLRNode firmware  │
│  ┌─────────┐  ┌──────────┐  │   │                   │
│  │ HDLC    │◄─┤ Transport│◄─┼───┼─► radio ──────────┤
│  │ framer  │  │ ESP-NOW  │  │ 2.4 GHz ISM          │
│  │ + frag. │  │ or FLRC  │  │   └───────────────────┘
│  └─────────┘  └──────────┘  │
│  LBT CSMA · stats · config  │
└─────────────────────────────┘
```

The core is transport-agnostic: one framing layer, two radio backends, more
to come.

## Benchmarks: measured, not marketing

We will publish real numbers from real hardware, for every rate and
distance, in `docs/benchmarks.md` as the hardware milestones complete.
No simulated numbers, ever.

## Radio & legal compliance

- 2.4 GHz ISM band (2400-2483.5 MHz), **≤ 20 dBm EIRP including antenna**
  per ETSI EN 300 328
- Listen-before-talk CSMA always on (adaptive equipment)
- Firmware refuses configurations that would exceed EIRP limits
- Default channel 2440 MHz (clear of busy WiFi edges)
- Full compliance notes will live in `docs/compliance.md`

**FLRNode is legal to operate without a license across the EU and in any
region with a 2.4 GHz ISM band.** Always check your local rules.

## Roadmap

- [ ] M1: ESP-NOW transport, two-node link
- [ ] M2: Reticulum integration (Sideband/MeshChat work)
- [ ] M3: `flrn` CLI + one-line host installer
- [ ] M4: FLRC transport (SX1280) + published benchmarks
- [ ] M5: Web flasher + v1.0 launch
- [ ] Meshtastic 2.4 GHz interop bridge
- [ ] ESP8266 mini-node preset ($5 end-to-end)

## Comparison

| | FLRNode | Meshtastic | RNode |
| --- | --- | --- | --- |
| Bandwidth | **260k-1 Mbps** | 0.3-20 kbps | 0.3-50 kbps |
| Range (typ.) | 0.1-2 km | 1-10 km | 1-10 km |
| E2E encryption | ✅ (Reticulum) | app-level | ✅ |
| Mesh | ✅ multi-hop | ✅ | ✅ |
| Cheapest node | **$5** | $20 | $30 |
| Works with | Reticulum apps | Meshtastic apps | Reticulum apps |

*We're friends with all of these. Meshtastic and RNode are your pick for
maximum range; FLRNode is your pick when you need megabits.*

## Contributing

Issues, PRs, board definitions, benchmarks and translations welcome.

## Credits

- [Mark Qvist](https://github.com/markqvist): Reticulum, LXMF, RNode
  (the reason any of this works)
- [jgromes](https://github.com/jgromes/RadioLib): RadioLib
- The Meshtastic community, for proving 2.4 GHz mesh is wanted

## License

MIT. See [LICENSE](LICENSE).
