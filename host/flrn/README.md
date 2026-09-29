# flrn — FLRNode host CLI

Command-line companion for [FLRNode](https://github.com/juanmicl/flrnode)
bridges: encrypted radio links that carry [Reticulum](https://github.com/markqvist/Reticulum)
traffic over cheap ESP32/ESP8266 hardware.

## Install

From a checkout of the FLRNode repository:

    sh scripts/install.sh        # flrn (pipx) + rns, then runs flrn setup

Or manually:

    pipx install ./host/flrn     # from the repo root
    pipx install rns             # the Reticulum daemon

Requires Python 3.10+ and a FLRNode bridge on USB (CH340, 921600 8N1).

## Commands

| Command | What it does |
| --- | --- |
| `flrn scan` | find CH340 bridges on USB, show fw / mode / channel / MAC |
| `flrn stats PORT` | live ARQ counters (14-key schema, `--watch` to poll) |
| `flrn config get KEY --port P` | read a setting (channel, tx_power, …) |
| `flrn config set KEY VAL --all-bridges` | apply to both bridges (clamped, persisted) |
| `flrn test PORT_A PORT_B` | over-air link test: 50 frames, in-order verdict + ARQ counters |
| `flrn doctor` | health checks with actionable fixes (ports, fw skew, rnsd, config) |
| `flrn setup` | write the RNS SerialInterface config for an attached bridge |

`--json` on scan/stats/test/doctor emits one machine-parseable JSON
document. Exit codes: 0 ok, 1 failure, 2 port busy.

## More

Hardware, wiring, compliance and benchmarks:
[the FLRNode repository](https://github.com/juanmicl/flrnode).
