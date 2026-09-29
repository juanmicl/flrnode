"""flrn.commands.test — over-air link test between two bridges.

The gate-2 essence: 50 frames of varied sizes (seed byte = frame index)
into side A, collect from side B, verify in-order delivery via the seed
bytes, then pull both bridges' ARQ counters as evidence.
"""

import time

from ..link import BridgeLink, deframe, frame
from .stats import parse_stats_reply

# Varied sizes spanning the ESP-NOW MTU (246/247 straddle fragmentation;
# 512 is the L3 max).
IN_ORDER_SIZES = (8, 64, 246, 247, 400, 512, 13, 100)


def make_frames(n: int = 50) -> list[bytes]:
    """n test frames; first byte = frame index (the in-order witness),
    rest a deterministic pattern; lengths cycle IN_ORDER_SIZES."""
    frames = []
    for i in range(n):
        size = IN_ORDER_SIZES[i % len(IN_ORDER_SIZES)]
        body = bytes((j * 7 + 3) & 0xFF for j in range(size - 1))
        frames.append(bytes([i & 0xFF]) + body)
    return frames


def check_in_order(received: list[bytes]) -> bool:
    """True when the received seed bytes are strictly ascending (order
    preserved; lost frames allowed). Nothing received -> False."""
    if not received:
        return False
    seeds = [f[0] for f in received]
    return all(a < b for a, b in zip(seeds, seeds[1:]))


def _pull_stats(port: str) -> dict:
    """Fetch + parse one bridge's 14-key stats (short timeout)."""
    link = BridgeLink(port)
    link.open()
    try:
        return parse_stats_reply(link.ctrl({"cmd": "stats"}, timeout=5.0))
    finally:
        link.close()


def run_link_test(port_a: str, port_b: str, n: int = 50,
                  timeout: float = 20.0) -> dict:
    """Run the over-air test A -> B. Returns {"sent", "received",
    "in_order", "stats_a", "stats_b"}. Raises PortBusyError /
    LinkTimeout / LinkError like the rest of the package.

    Same-package note: raw DATA writes/reads use the links' `_ser`
    handle (BridgeLink.ctrl is CTRL-only by design)."""
    frames = [frame(f) for f in make_frames(n)]
    la = BridgeLink(port_a)
    lb = BridgeLink(port_b)
    received: list[bytes] = []
    try:
        la.open()
        lb.open()
        for w in frames:
            la._ser.write(w)
        la._ser.flush()
        deadline = time.monotonic() + timeout
        buf = b""
        while time.monotonic() < deadline:
            avail = getattr(lb._ser, "in_waiting", 0)
            chunk = lb._ser.read(avail if avail else 1)
            if chunk:
                buf += chunk
                got = deframe(buf)
                if len(got) >= n:
                    received = got
                    break
    finally:
        la.close()
        lb.close()
    received = deframe(buf)[:n] if received == [] else received[:n]
    return {"sent": n, "received": len(received),
            "in_order": check_in_order(received),
            "stats_a": _pull_stats(port_a),
            "stats_b": _pull_stats(port_b)}
