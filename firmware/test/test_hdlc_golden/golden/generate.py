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
