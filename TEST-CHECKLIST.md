# TEST-CHECKLIST: M2 acceptance (bridge + Reticulum)

Gates run bench-first (no hosts), then end-to-end with the Pis. M2
closes only when gate 3 results are pasted into STATUS.md.

## Phase 0: prerequisites

- The M1 boards, still labeled A and B, MACs A=E8DB84DC63E1,
  B=E8DB84DC658C (provisioned in M1; unchanged).
- `pio` on the bench PC + dialout (M1 one-time setup, already done).

## Phase 1: flash bridge firmware (one board at a time)

1. Plug A only:
   `PLATFORMIO_BUILD_FLAGS='-DFLR_PEER_MAC=\"E8DB84DC658C\"' pio run -e d1_mini_bridge -t upload`
2. Unplug A. Plug B only:
   `PLATFORMIO_BUILD_FLAGS='-DFLR_PEER_MAC=\"E8DB84DC63E1\"' pio run -e d1_mini_bridge -t upload`
3. Plug A back in (both connected).
4. Boot lines arrive at 921600 — watch with the gate 2 harness below
   (a plain `pio device monitor` also works; escaped quotes in the
   build flag are mandatory, see M1's checklist note).

## Phase 2: gate 2 — bridge on the bench (no hosts)

1. `~/.local/share/pipx/venvs/platformio/bin/python scripts/bridge_gate2.py /dev/ttyUSB0 /dev/ttyUSB1`
2. Expect `GATE2: PASS`: 9 frames A->B in order, 3 frames B->A, and
   both bridges' stats counters printed (ack/nak/retry activity under
   air loss is expected and is the point).
3. Paste the full output into STATUS.md.

## Phase 3: gate 3 — Reticulum end to end (AGENTS wording)

1. Provision both Pis per docs/host-setup.md.
2. Move each D1 mini from the PC to its Pi.
3. `rnsd -v` on both (tmux/screen fine).
4. `rnstatus` on either Pi: the SerialInterface must be up; announces
   from both nodes must cross.
5. `rnspong`: announce a destination on Pi 1, send a message from
   Pi 2, receive the reply (round trip through the radio).
6. Optional evidence: pull `{"cmd":"stats"}` from a bridge via its Pi
   (any pyserial one-liner at 921600) and paste it too.
7. Paste results into STATUS.md.

## Troubleshooting

- No boot line: check 921600 (not 115200 — that was M1's ping-pong).
- rnstatus shows no interface: reticulum.conf `serial_port` by-id path
  wrong; check `ls /dev/serial/by-id/` on the Pi.
- Announce bursts at bring-up are normal (TX backlog exists for them).
- USB cables at 921600: short and good quality.

## Verdict rule

M2 passes only with gate 2 PASS recorded plus gate 3 results pasted by
the human into STATUS.md (AGENTS hard rule: no simulated results).
