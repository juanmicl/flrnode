#!/usr/bin/env python3
"""M2 gate 2: bridge bench harness.

Opens both D1 mini serial ports at 921600, waits for both bridge boot
lines, injects HDLC DATA frames of varied sizes into side A, asserts
the identical payloads emerge HDLC-framed and IN ORDER from side B
(and a reverse-direction sample), then polls {"cmd":"stats"} on both
sides and prints the counters as gate evidence.

Usage:
  bridge_gate2.py <portA> <portB>
Run with the PlatformIO venv python (it has pyserial):
  ~/.local/share/pipx/venvs/platformio/bin/python scripts/bridge_gate2.py \
      /dev/ttyUSB0 /dev/ttyUSB1
"""
import sys, time
import serial

def crc16_xmodem(data: bytes, crc: int = 0) -> int:
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

def hdlc_frame(inner_type: int, payload: bytes) -> bytes:
    body = bytes([inner_type]) + payload
    fcs = crc16_xmodem(body)
    body += bytes([fcs & 0xFF, fcs >> 8])
    return b"\x7E" + stuff(body) + b"\x7E"

class Deframer:
    def __init__(self):
        self.frames = []   # (type, payload) — gate harness: assert only on valid deliveries
        self._acc = bytearray()
        self._esc = False
    def feed(self, bs):
        for b in bs:
            if b == 0x7E:
                if len(self._acc) >= 3:
                    body = bytes(self._acc)
                    fcs = body[-2] | (body[-1] << 8)
                    if crc16_xmodem(body[:-2]) == fcs:
                        self.frames.append((body[0], body[1:-2]))
                self._acc = bytearray(); self._esc = False
            elif b == 0x7D:
                self._esc = True
            else:
                if self._esc:
                    b = {0x5E: 0x7E, 0x5D: 0x7D}.get(b, b)
                    self._esc = False
                self._acc.append(b)

def reset(s):
    s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
    s.reset_input_buffer()

def wait_boot(s, timeout=8):
    end = time.time() + timeout
    while time.time() < end:
        line = s.readline().decode(errors="replace").strip()
        if '"t":"boot"' in line and '"mode":"bridge"' in line:
            return line
    return None

def drain(defr, s):
    n = s.in_waiting
    if n:
        defr.feed(s.read(n))

def run_round(src, dst, dsrc, ddst, frames, label, timeout=45):
    for i, payload in enumerate(frames):
        src.write(hdlc_frame(0x01, payload))
    deadline = time.time() + timeout
    while time.time() < deadline:
        drain(dsrc, src); drain(ddst, dst)
        got = [p for (t, p) in ddst.frames if t == 0x01]
        if len(got) >= len(frames):
            break
        time.sleep(0.05)
    got = [p for (t, p) in ddst.frames if t == 0x01][:len(frames)]
    ok = len(got) == len(frames) and all(
        bytes(a) == bytes(b) for a, b in zip(got, frames))
    print(f"[{label}] {len(got)}/{len(frames)} frames, in-order match: {ok}")
    return ok, got

def main():
    pa, pb = sys.argv[1], sys.argv[2]
    sa = serial.Serial(pa, 921600, timeout=1)
    sb = serial.Serial(pb, 921600, timeout=1)
    reset(sa); reset(sb)
    ba = wait_boot(sa); bb = wait_boot(sb)
    print("bootA:", ba); print("bootB:", bb)
    if not (ba and bb):
        sys.exit("FAIL: missing bridge boot line(s)")
    da, db = Deframer(), Deframer()
    frames = [bytes((i * 7 + 3) & 0xFF for i in range(n))
              for n in (8, 64, 246, 247, 400, 512, 13, 512, 100)]
    ok1, _ = run_round(sa, sb, da, db, frames, "A->B")
    ok2, _ = run_round(sb, sa, db, da, frames[:3], "B->A")
    for name, s, d in (("A", sa, da), ("B", sb, db)):
        d.frames.clear()
        s.write(hdlc_frame(0x02, b'{"cmd":"stats"}'))
        time.sleep(0.5)
        drain(d, s)
        for t, p in d.frames:
            if t == 0x02:
                print(f"stats[{name}]:", p.decode(errors="replace"))
    print("GATE2:", "PASS" if ok1 and ok2 else "FAIL")
    sys.exit(0 if ok1 and ok2 else 1)

if __name__ == "__main__":
    main()
