# FLRNode Protocol Specification (v1)

Status: DRAFT (authoritative for implementation)
Language: RFC 2119 (MUST/SHOULD/MAY). Reserved bits MUST be sent as 0 and
MUST be ignored on receive. Forward-compatible by version field.

This document is self-contained. Implement from this spec only.

---

## 1. Layering

```
┌────────────────────────────────────────────────────────┐
│ Host app (RNS SerialInterface, flrn CLI, minicom)      │
├────────────────────────────────────────────────────────┤
│ L3  HDLC serial framing          (§2)                  │
├────────────────────────────────────────────────────────┤
│ L2  Frame/fragment ARQ           (§3-§6)               │
├────────────────────────────────────────────────────────┤
│ L1  Transport: ESP-NOW / FLRC    (§7)                  │
└────────────────────────────────────────────────────────┘
```

Threat model: confidentiality/authenticity are provided by upper layers
(Reticulum E2E crypto; ESP-NOW CCMP at L1). This protocol provides framing
integrity (CRC-16) and delivery reliability only. Do NOT add crypto here.

---

## 2. L3: HDLC serial framing

Serial: 921600 baud, 8N1, no flow control.

A serial frame carries a 1-byte **inner type** followed by a payload:

| Inner type | Name  | Payload                             |
|-----------:|-------|-------------------------------------|
| 0x01       | DATA  | 0-512 bytes (RNS frame / frag)      |
| 0x02       | CTRL  | one JSON line, `\n`-terminated (§8) |

### 2.1 Framing rules

1. Frame on air/wire = `FLAG, [TYPE, PAYLOAD…], FCS_LO, FCS_HI, FLAG`
2. FLAG = 0x7E. Octet stuffing: 0x7E→`7D 5E`, 0x7D→`7D 5D`.
   Stuffing applies to TYPE, PAYLOAD **and FCS bytes**.
3. FCS = CRC-16/XMODEM over the *unstuffed* `TYPE‖PAYLOAD`.
   - poly 0x1021, init 0x0000, refin=false, refout=false, xorout=0x0000
   - validation vector: ASCII `"123456789"` → CRC `0x31C3`
   - byte order on the wire: FCS_LO first, then FCS_HI (little-endian
     order), MSB first *within* each byte per UART. FCS bytes are subject
     to stuffing like any other.
4. Max stuffed frame on serial: 532 B unstuffed + stuffing + 2 FLAG.
5. RX: byte equal to FLAG inside a frame (impossible if TX compliant)
   MUST resync: discard partial frame, `rx_resync++`.
6. Inter-frame silence has no semantic. Back-to-back frames allowed.

---

## 3. L2: addressing & peer keys

L2 delivers `(peer_key, L3 frame)` upward and accepts `(peer_key|null, frame)`.

- **ESP-NOW**: peer_key = 6-byte MAC from the radio header. Broadcast MAC
  (FF:…:FF) is legal for CTRL announcements only; DATA via broadcast SHOULD
  be avoided (no ACK possible).
- **FLRC**: radio has no addressing. Every L2 PDU begins with a 2-byte
  SENDER_ID (config value, random at first boot, persisted). peer_key =
  those 2 bytes. SENDER_ID 0x0000 and 0xFFFF are reserved (0=unset,
  FFFF=broadcast). SENDER_ID is not covered by any checksum at L2: it lives
  inside the L2 PDU (§4), so it is protected by delivery, not by a
  separate checksum.

Peers are learned implicitly. Peer table: 8 entries max, LRU eviction.

---

## 4. L2: PDU format

All multi-byte fields big-endian unless stated. One PDU per L1 transmission.

```
Offset  Size  Field
0       1     B0 = [ver:2][ftype:2][flags:4]
1       2     SEQ        (frame sequence number, per sender)
3       1     B3 = [frag_idx:4][frag_cnt:4]
4       …     PAYLOAD    (0…MTU-4 bytes)
```

- `ver` = 1. Receiver MUST silently drop other versions (`ver_drop++`).
- `ftype`:
  - 0 DATA-FRAG: payload = fragment of an L3 frame
  - 1 ACK: see §5.2
  - 2 NAK: see §5.3
  - 3 CTRL: payload = 1 JSON line (mesh-wide config/status, no `\n` on air)
- `flags` (DATA-FRAG): bit0 = RETRY (set on retransmission; informational),
  bits1-3 reserved.
- `SEQ`: 16-bit, monotonically increasing per sender, wraps mod 65536.
  Sequence 0 is never used (sentinel, §5.4).
- `frag_idx`: 0…14; `frag_cnt`: 1…15 (frag_cnt==0 invalid → drop).
  frag_idx MUST be < frag_cnt. All fragments of one SEQ carry the same
  frag_cnt. Last fragment may be short; others MUST be exactly MTU-4.

MTU per transport (payload budget = MTU − 4 B header):
ESP-NOW: MTU 250 → 246 B/frag. FLRC: MTU 127 → 123 B/frag.

Max reassembled L3 frame = **512 B** ⇒ max fragments actually used:
⌈512/246⌉ = 3 (ESP-NOW), ⌈512/123⌉ = 5 (FLRC). Both < 15 (headroom for
future bigger MTUs). Senders MUST reject L3 frames > 512 B (`tx_too_big++`,
CTRL event `frame_dropped`).

---

## 5. L2: ARQ (window, cumulative ACK, NAK)

### 5.1 Sender state machine (per L3 frame)

```
NEW → FRAGMENTED → IN_FLIGHT ── cumulative ACK ≥ SEQ ──► DONE
                        │  ▲
                        ▼  │
                     RETRY_WAIT (per-fragment timers)
                        │  retries > MAX_RETRIES
                        ▼
                     DROPPED (stats + CTRL event `frame_dropped`)
```

- Window: **W = 8** frames IN_FLIGHT simultaneously (per sender, global).
  Sender MUST NOT emit a DATA-FRAG with SEQ > cumulative_ack + W − 1.
- Per-fragment retransmission timer: `T_retry = 60 ms << retry_count`,
  capped at 500 ms. MAX_RETRIES = 5. Retransmit ONLY fragments not covered
  by the latest ACK/NAK bitmap, with RETRY flag set.
- DROPPED frames do NOT advance SEQ allocation; SEQ is per-frame, gaps are
  legal and expected (see §5.4).

### 5.2 ACK PDU (ftype=1)

```
B0 = ver|1|0000
SEQ = cumulative ACK value N ("every frame ≤ N fully delivered")
B3 = [resv:4][bitmap_hi_bit_map:4]  → 8-bit bitmap occupies B3 fully
PAYLOAD: none
```

Bitmap semantics: bit (7−i) of B3 = 1 ⇒ frame N+1+i (i=0…7) is **fully
reassembled** at receiver. 0 = not yet / unknown. Cumulative ACK N advances
the sender window. Sender advances its `cumulative_ack` to the highest
contiguous 1-bit.

Receiver sends ACK when: a frame completes reassembly, or upon receiving a
duplicate fragment (idempotent re-ACK). No delayed-ACK, no piggybacking in v1.

### 5.3 NAK PDU (ftype=2)

Sent immediately when receiver detects a gap: fragment with frag_idx > 0
arrives for an unknown/partial SEQ inside its window.

```
PAYLOAD (3 bytes per entry, up to 4 entries):
  [seq:2][missing_mask:1]  — bit j set ⇒ fragment j missing
```

Sender responds by retransmitting listed fragments (bypassing timers,
respecting RETRY flag).

### 5.4 Receiver state (per peer)

- `rcv_cum`: highest contiguous fully-delivered SEQ.
- 8 slots for SEQ = rcv_cum+1 … rcv_cum+8, each: `frag_recv[15]` bitmask
  - `frag_cnt` + partial payload buffer.
- Delivery: when a slot completes (all bits set), the L3 frame is passed
  up, `rcv_cum` advances past all contiguous complete slots.
- **Dedup is purely positional**: SEQ ≤ rcv_cum → stale, drop silently
  (`stale++`). Duplicate fragment within a slot → merge idempotently
  (OR the bit), re-ACK. No time-based caches exist in this protocol.
- SEQ > rcv_cum+8 → outside window → drop (`window_overflow++`) and send
  NAK with empty payload (signals the sender it is ahead).
- Wrap-around comparison is modular over 65536; SEQ 0 is skipped by
  senders to keep the "stale ⇔ SEQ ≤ rcv_cum" test unambiguous at boot
  (rcv_cum initializes to 0).

### 5.5 Flow & error accounting (mandatory counters)

`tx_frames tx_frags tx_retries tx_dropped rx_frags rx_delivered
rx_dups rx_stale rx_crc_fail rx_window_overflow nak_sent ack_sent`
All exposed via CTRL (§8). Airtime (ms) counted at L1.

---

## 6. Fragmentation rules

1. L3 frame of length L → `frag_cnt = ⌈L / (MTU−4)⌉`, 1…15.
2. Fragment i payload = bytes [i·(MTU−4), min((i+1)·(MTU−4), L)).
3. Fragments are transmitted back-to-back (burst), idx ascending.
4. Reassembly delivers ONLY on completeness (no partial delivery).
5. If any fragment of a slot is older than `T_slot_gc = 5 s`, the slot is
   discarded (`slot_gc++`). The timer protects receiver memory only;
   correctness comes from sender retries + NAK.

Memory budget (per peer): 8 slots × (15/15 used worst case 512 B + 32 B
meta) ≈ 4.4 KB. Acceptable on ESP8266. Document actuals in code comments.

---

## 7. L1: transports

### 7.1 ESP-NOW

- Channel fixed at boot from config (default 1). All nodes same channel.
- Encryption: CCMP with PMK + per-peer LMK (config). Unencrypted mode
  exists for bench testing but MUST emit a CTRL warning at boot.
- Send: `esp_now_send`; failure callback → immediate retry once, then
  PDU queued for T_retry (counts as retry #1).
- LR mode (ESP32 only): config flag, off by default.

### 7.2 FLRC (SX1280 via RadioLib)

- 2440 MHz default. Rates 260/520/1040 kbps, BW per rate table in
  docs/compliance.md. FEC on. Sync word 0x1424 (private network default).
- LBT: channel scan before every burst; if busy, backoff 1-15 random
  slots of 1 ms; give up after 8 attempts (`lbt_giveup++`), then the PDU
  is requeued at the ARQ layer. This does NOT consume a retry.
- Airtime accounting mandatory (feeds stats + future duty view).

---

## 8. CTRL channel (JSON lines)

Both directions. Requests end with `\n`. Example pairs:

```
→ {"cmd":"stats"}                        ← {"stats":{"tx_frames":123,…}}
→ {"cmd":"get","key":"channel"}          ← {"channel":1}
→ {"cmd":"set","key":"rate","val":520}   ← {"ok":true}
→ (unsolicited)                          ← {"event":"frame_dropped","seq":42,
                                             "reason":"max_retries"}
```

Keys: `channel rate tx_power pmk node_id stats_period`. Sets apply to
flash and take effect on reboot EXCEPT `stats_period` (immediate).
Unknown cmd → `{"err":"unknown_cmd"}`. JSON must be strict (no comments).
On-air CTRL (ftype=3) uses the same schema minus `\n`, max 110 B.

---

## 9. Conformance test matrix (IDs referenced by test/golden/)

| ID  | Scenario                                              | Expectation |
|-----|-------------------------------------------------------|-------------|
| P-01| HDLC roundtrip, empty DATA payload                    | byte-exact vs golden |
| P-02| Stuffing: payload `7E 7D 11 22`                       | wire `7E 01 7D 5E 7D 5D 11 22 FCS 7E` (FCS from generator) |
| P-03| CRC corruption (1 bit flipped in payload)              | frame dropped, `rx_crc_fail++` |
| P-04| Single-fragment frame, happy path                      | delivered, ACK advances |
| P-05| 5-fragment frame, reverse order                        | delivered via NAK + reorder |
| P-06| Duplicate fragment                                     | idempotent, re-ACK, `rx_dups++` |
| P-07| Stale seq (≤ rcv_cum)                                  | silent drop, `rx_stale++` |
| P-08| Gap mid-window                                        | NAK lists exact missing mask |
| P-09| Retry exhaustion (5×)                                 | DROPPED, event emitted |
| P-10| Window overflow (seq > rcv_cum+8)                     | drop + empty NAK |
| P-11| SEQ wrap (65535 → 2)                                  | no stall, positional test correct |
| P-12| Two peers interleaved                                 | independent slots/counters |
| P-13| frag_cnt mismatch across same SEQ                     | newer cnt wins, re-fragment state reset, `meta_conflict++` |
| P-14| L3 frame > 512 B                                      | rejected, `tx_too_big++` |
| P-15| ver=2 PDU                                             | silent drop, `ver_drop++` |
| P-16| LBT busy 8×                                           | requeue, no retry consumed |
| P-17| 1000 frames @10 fps soak                              | 0 loss (cabled), mem stable |
| P-18| CTRL roundtrip all cmds                                | schema-conformant replies |
| P-19| Reset mid-burst                                        | resync ≤ 1 frame loss |
| P-20| 2 h soak with stats_period 5 s                        | no drift/leak (heap delta < 512 B) |

---

## Appendix A: Golden vector generator (commit its output)

`test/golden/generate.py` (stdlib only). Run on any host; commit the
emitted `.txt` files. Tests MUST load these, never hand-type CRCs.

```python
#!/usr/bin/env python3
"""Generate FLRNode golden vectors. Output: one .txt per case, lines of
hex bytes with comments."""
import pathlib

def crc16_xmodem(data: bytes, crc: int = 0x0000) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc

def stuff(b: bytes) -> bytes:
    out = bytearray()
    for x in b:
        if x == 0x7E: out += b"\x7D\x5E"
        elif x == 0x7D: out += b"\x7D\x5D"
        else: out.append(x)
    return bytes(out)

def hdlc(inner_type: int, payload: bytes) -> bytes:
    body = bytes([inner_type]) + payload
    fcs = crc16_xmodem(body)
    body += bytes([fcs & 0xFF, fcs >> 8])   # FCS_LO, FCS_HI
    return b"\x7E" + stuff(body) + b"\x7E"

OUT = pathlib.Path(__file__).parent
cases = {
    "p01_empty_data":   hdlc(0x01, b""),
    "p02_stuffing":     hdlc(0x01, b"\x7E\x7D\x11\x22"),
    "p04_hello":        hdlc(0x01, b"hello flrnode"),
    "ctrl_stats":       hdlc(0x02, b'{"cmd":"stats"}\n'),
}
for name, wire in cases.items():
    lines = [f"# {name}"] + [f"{b:02X}" for b in wire]
    (OUT / f"{name}.txt").write_text("\n".join(lines) + "\n")

# self-check: known CRC test vector
assert crc16_xmodem(b"123456789") == 0x31C3
print("golden vectors written")
```

## Appendix B: Constant summary

| Constant       | Value    | Notes                          |
|----------------|----------|--------------------------------|
| SERIAL_BAUD    | 921600   | 8N1                            |
| MAX_L3_FRAME   | 512 B    | reject above                   |
| W (window)     | 8        | frames                         |
| T_retry base   | 60 ms    | ×2 per retry                   |
| T_retry cap    | 500 ms   |                                |
| MAX_RETRIES    | 5        | then DROPPED                   |
| T_slot_gc      | 5 s      | receiver slot GC               |
| MTU (ESP-NOW)  | 250 B    | 246 B payload/frag             |
| MTU (FLRC)     | 127 B    | 123 B payload/frag             |
| PEERS_MAX      | 8        | LRU                            |
| SEQ_SENTINEL   | 0        | never used                     |

End of specification.
