"""flrn.link — RNS serial framing and CTRL protocol client.

Wire format (M2-amended, normative per docs/protocol.md §2): frames are
FLAG (0x7E) + escaped payload + FLAG, escaping 0x7D with ESC_MASK 0x20
(0x7E -> 7D 5E, 0x7D -> 7D 5D). NO FCS, NO type byte.

CTRL JSON lines ride the same stream as framed payloads; per §8 they
are COMPACT JSON (no whitespace), printable ASCII, at most 95 bytes,
with one optional trailing newline. pyserial is imported only when a
real port is opened, so hardware-less imports and tests stay clean.
"""

import json
import subprocess
import time

FLAG = 0x7E
ESC = 0x7D
ESC_MASK_PAIRS = {0x5E: 0x7E, 0x5D: 0x7D}
CTRL_MAX_BYTES = 95  # §8: the bridge's CTRL heuristic bound


class LinkError(RuntimeError):
    """Permanent link/protocol problem (bad request, port failure)."""


class LinkTimeout(RuntimeError):
    """The bridge did not answer within the allotted time."""


class PortBusyError(RuntimeError):
    """The serial port is held by another process (usually rnsd)."""

    def __init__(self, message: str, holder_hint: str = ""):
        super().__init__(message)
        self.holder_hint = holder_hint


def frame(payload: bytes) -> bytes:
    """RNS wire frame: FLAG + escaped payload + FLAG."""
    out = bytearray([FLAG])
    for b in payload:
        if b == FLAG:
            out += b"\x7D\x5E"
        elif b == ESC:
            out += b"\x7D\x5D"
        else:
            out.append(b)
    out.append(FLAG)
    return bytes(out)


def deframe(data: bytes) -> list[bytes]:
    """Complete frames in a byte stream; junk outside frames is ignored
    (flag-opens / flag-closes delimit accumulation, like RNS's own
    host-side parser)."""
    frames: list[bytes] = []
    acc = bytearray()
    in_frame = False
    esc = False
    for b in data:
        if b == FLAG:
            if in_frame and acc:
                frames.append(bytes(acc))
            in_frame = not in_frame
            acc = bytearray()
            esc = False
        elif in_frame:
            if b == ESC:
                esc = True
            else:
                if esc:
                    acc.append(ESC_MASK_PAIRS.get(b, b))
                    esc = False
                else:
                    acc.append(b)
    return frames


def parse_boot_line(data: bytes) -> dict | None:
    """Parse one bridge boot line (raw JSON, CRLF-terminated) into a
    dict; None on anything that is not a JSON object."""
    try:
        d = json.loads(data.decode("utf-8", errors="replace").strip())
    except (ValueError, UnicodeDecodeError):
        return None
    return d if isinstance(d, dict) else None


def port_holder(port: str) -> str | None:
    """Best-effort check for another process holding the port: fuser
    first, then lsof -t. Returns the holder PID string, a tool marker
    when the PID is unavailable, or None when free / undeterminable."""
    try:
        fuser = subprocess.run(["fuser", "-s", port],
                               capture_output=True, timeout=2)
        if fuser.returncode == 0:
            pid = _lsof_pid(port)
            return pid if pid else "fuser"
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
        pass
    return _lsof_pid(port)


def _lsof_pid(port: str) -> str | None:
    try:
        r = subprocess.run(["lsof", "-t", port],
                           capture_output=True, text=True, timeout=2)
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
        return None
    if r.returncode == 0 and r.stdout.strip():
        return r.stdout.strip().splitlines()[0]
    return None


class BridgeLink:
    """One attached bridge over its serial port.

    `port` is a device path string, or an already-open serial-like
    object (duck-typed read/write/flush) for tests.
    """

    def __init__(self, port: str, baud: int = 921600):
        self._port = port
        self._baud = baud
        self._ser = None

    def open(self) -> None:
        if self._ser is not None:
            return
        if isinstance(self._port, str):
            holder = port_holder(self._port)
            if holder:
                raise PortBusyError(
                    f"{self._port} is held by {holder}",
                    holder_hint=(f"rnsd is using {self._port} — "
                                 "systemctl --user stop rnsd or pass "
                                 "--port"),
                )
            import serial  # deferred: hardware-less imports stay clean
            self._ser = serial.Serial(self._port, self._baud, timeout=1)
        else:
            self._ser = self._port  # injected fake / pre-opened port

    def close(self) -> None:
        if self._ser is not None:
            try:
                self._ser.close()
            finally:
                self._ser = None

    def reset_pulse(self) -> None:
        """DTR/RTS pulse that resets an attached D1 mini (open asserts
        DTR); framing after this is RNS-format."""
        self._ser.dtr = False
        self._ser.rts = True
        time.sleep(0.1)
        self._ser.rts = False
        self._ser.flush()

    def _read_some(self) -> bytes:
        n = getattr(self._ser, "in_waiting", 0)
        if n:
            return self._ser.read(n)
        return self._ser.read(1)  # blocks up to the port's timeout

    def read_boot_line(self, timeout: float = 4.0) -> dict:
        """Collect raw bytes until a JSON boot line arrives (the boot
        line is printed raw, not HDLC-framed). Raises LinkTimeout."""
        deadline = time.monotonic() + timeout
        buf = b""
        while time.monotonic() < deadline:
            buf += self._read_some()
            for line in buf.split(b"\n"):
                d = parse_boot_line(line)
                if d is not None:
                    return d
            if b"\n" in buf:
                buf = buf.rsplit(b"\n", 1)[1]
        raise LinkTimeout(
            "no boot line — is rnsd holding the port, or is this the "
            "115200 ping-pong firmware instead of the 921600 bridge?")

    def ctrl(self, request: dict, timeout: float = 3.0) -> dict:
        """Send one CTRL command (§8: COMPACT JSON, <= 95 bytes,
        validated BEFORE writing) and await one JSON reply."""
        raw = json.dumps(request, separators=(",", ":")).encode()
        if len(raw) > CTRL_MAX_BYTES:
            raise LinkError(
                f"CTRL command is {len(raw)} bytes; the wire bound is "
                f"{CTRL_MAX_BYTES} (§8) — shorten the request")
        self._ser.write(frame(raw))
        self._ser.flush()
        deadline = time.monotonic() + timeout
        buf = b""
        while time.monotonic() < deadline:
            buf += self._read_some()
            for payload in deframe(buf):
                if payload.startswith(b'{"'):
                    d = parse_boot_line(payload)
                    if d is not None:
                        return d
            buf = buf[-1024:]
        raise LinkTimeout(
            "no CTRL reply — rnsd contention or firmware without "
            "get/set support (pre-M3 replies unknown_cmd, not silence)")
