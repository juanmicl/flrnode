#!/usr/bin/env python3
"""Generate FLRNode golden vectors (RNS wire format: FLAG + escaped
payload + FLAG, no FCS, no type byte). Output: one .txt per case, lines
of hex bytes with comments."""
import pathlib

def escape(b: bytes) -> bytes:
    out = bytearray()
    for x in b:
        if x == 0x7E: out += b"\x7D\x5E"
        elif x == 0x7D: out += b"\x7D\x5D"
        else: out.append(x)
    return bytes(out)

def unescape(b: bytes) -> bytes:
    out = bytearray()
    esc = False
    for x in b:
        if esc:
            out.append({0x5E: 0x7E, 0x5D: 0x7D}.get(x, x))
            esc = False
        elif x == 0x7D:
            esc = True
        else:
            out.append(x)
    return bytes(out)

def hdlc(payload: bytes) -> bytes:
    return b"\x7E" + escape(payload) + b"\x7E"

OUT = pathlib.Path(__file__).parent
cases = {
    "p01_empty_data":   hdlc(b""),
    "p02_stuffing":     hdlc(b"\x7E\x7D\x11\x22"),
    "p04_hello":        hdlc(b"hello flrnode"),
    "ctrl_stats":       hdlc(b'{"cmd":"stats"}\n'),
}
for name, wire in cases.items():
    lines = [f"# {name}"] + [f"{b:02X}" for b in wire]
    (OUT / f"{name}.txt").write_text("\n".join(lines) + "\n")

# self-check: decoded roundtrip == input for every case (and the
# escape map: 7D 5E -> 7E, 7D 5D -> 7D)
for payload in (b"", b"\x7E\x7D\x11\x22", b"hello flrnode",
                b'{"cmd":"stats"}\n'):
    assert unescape(escape(payload)) == payload
assert unescape(b"\x7D\x5E\x7D\x5D") == b"\x7E\x7D"
print("golden vectors written")
