"""flrn.link — RNS serial framing and CTRL protocol client.

Wire format (M2-amended, normative per docs/protocol.md §2): frames are
FLAG (0x7E) + escaped payload + FLAG, escaping 0x7D with ESC_MASK 0x20
(0x7E -> 7D 5E, 0x7D -> 7D 5D). NO FCS, NO type byte.

CTRL JSON lines ride the same stream as framed payloads; per §8 they
are COMPACT JSON (no whitespace), printable ASCII, at most 95 bytes,
with one optional trailing newline. pyserial is imported only when a
real port is opened, so hardware-less imports and tests stay clean.
"""

import errno
import json
import subprocess
import time
from typing import Any

FLAG = 0x7E
ESC = 0x7D
ESC_MASK_PAIRS = {0x5E: 0x7E, 0x5D: 0x7D}
CTRL_MAX_BYTES = 95  # §8: the bridge's CTRL heuristic bound
_BOOT_LINE_MAX_BUF = 8 * 1024  # read_boot_line accumulation cap


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
    (flag-opens / flag-closes delimit accumulation — sound because the
    bridge emits every frame with its own leading AND trailing flag)."""
    frames: list[bytes] = []
    acc = bytearray()
    in_frame = False
    esc = False
    for b in data:
        if b == FLAG:
            if in_frame and acc:
                frames.append(bytes(acc))
                in_frame = False        # closed a full frame
            elif not in_frame:
                in_frame = True         # opened
            # else: FLAG while open with empty acc = orphan flag (e.g.
            # a read that began mid-frame); stay open — self-resync.
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


def _is_busy_error(e: OSError) -> bool:
    """True when an open failure means another holder on the port:
    pyserial's exclusive lock surfaces as EBUSY or a 'resource busy' /
    'exclusively lock' message depending on version. Plain permission
    failures (EACCES + 'Permission denied') deliberately do NOT match —
    those stay OSError so the CLI can show the dialout hint."""
    if e.errno == errno.EBUSY:
        return True
    text = str(e).lower()
    return ("device or resource busy" in text
            or "exclusively lock" in text)


class BridgeLink:
    """One attached bridge over its serial port.

    `port` is a device path string, or an already-open serial-like
    object (duck-typed read/write/flush) for tests.
    """

    def __init__(self, port: str | Any, baud: int = 921600):
        self._port = port
        self._baud = baud
        self._ser = None

    def open(self) -> None:
        """Open the serial port exclusively (a second holder is mapped
        to PortBusyError); an injected fake port is used as-is.
        Context: any. Blocking: holder check (fuser/lsof, <= 4 s) plus
        the open itself. Failure: PortBusyError or the underlying
        OSError (e.g. permission — surfaced with the dialout hint by
        the CLI)."""
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
            try:
                self._ser = serial.Serial(self._port, self._baud,
                                          timeout=1, exclusive=True)
            except OSError as e:
                # Race: a holder appeared between the pre-check and the
                # open (pyserial's exclusive lock fails with EBUSY / a
                # 'resource busy' message).
                if _is_busy_error(e):
                    raise PortBusyError(
                        f"{self._port} is held by another process",
                        holder_hint=(f"rnsd may be using {self._port} — "
                                     "systemctl --user stop rnsd or "
                                     "pass --port"),
                    ) from e
                raise
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
        line is printed raw, not HDLC-framed). The accumulation buffer
        is capped (oldest bytes dropped) so a garbage-spewing port
        cannot turn the timeout window into an O(n^2) re-split.
        Raises LinkTimeout."""
        deadline = time.monotonic() + timeout
        buf = b""
        while time.monotonic() < deadline:
            buf += self._read_some()
            if len(buf) > _BOOT_LINE_MAX_BUF:
                buf = buf[-1024:]  # one line's worth; boot lines ~150 B
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
        validated BEFORE writing) and await one JSON reply. Unsolicited
        {"event": ...} payloads (§8) are skipped, not returned, and the
        input buffer is drained first so a stale reply from a previous
        timed-out command cannot satisfy this one. CTRL-only: never use
        on a link whose boot line is still expected."""
        raw = json.dumps(request, separators=(",", ":")).encode()
        if len(raw) > CTRL_MAX_BYTES:
            raise LinkError(
                f"CTRL command is {len(raw)} bytes; the wire bound is "
                f"{CTRL_MAX_BYTES} (§8) — shorten the request")
        drain = getattr(self._ser, "reset_input_buffer", None)
        if drain is not None:
            drain()
        self._ser.write(frame(raw))
        self._ser.flush()
        deadline = time.monotonic() + timeout
        buf = b""
        while time.monotonic() < deadline:
            buf += self._read_some()
            for payload in deframe(buf):
                if payload.startswith(b'{"'):
                    d = parse_boot_line(payload)
                    if d is not None and "event" not in d:
                        return d
            # Bound the buffer: DATA-only streams must not grow it
            # unboundedly; 1 KiB exceeds the ~380 B worst-case stats
            # reply with headroom — do not shrink below that.
            buf = buf[-1024:]
        raise LinkTimeout(
            "no CTRL reply — rnsd contention or firmware without "
            "get/set support (pre-M3 replies unknown_cmd, not silence)")
