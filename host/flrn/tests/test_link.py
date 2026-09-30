import pytest
from flrn.link import (BridgeLink, LinkError, LinkTimeout, PortBusyError,
                       deframe, frame, parse_boot_line)


def test_frame_roundtrip_plain():
    assert deframe(frame(b"hello")) == [b"hello"]


def test_frame_escapes_flag_and_esc():
    payload = bytes([0x7E, 0x7D, 0x11])
    wire = frame(payload)
    assert wire[0] == 0x7E and wire[-1] == 0x7E
    assert 0x7E not in wire[1:-1]
    assert deframe(wire) == [payload]


def test_deframe_splits_back_to_back():
    assert deframe(frame(b"aa") + frame(b"bb")) == [b"aa", b"bb"]


def test_deframe_ignores_junk_between():
    stream = b"\x55\x00" + frame(b"x") + b"\xff" + frame(b"y")
    assert deframe(stream) == [b"x", b"y"]


def test_deframe_resyncs_on_orphan_flag():
    # A read that began mid-frame sees an orphan closing flag first; the
    # parser must re-open on it, not invert phase and swallow the frame.
    assert deframe(b"\x7e" + frame(b"p")) == [b"p"]


def test_parse_boot_line():
    line = b'{"t":"boot","fw":"m3","mode":"bridge","channel":1,"mac":"A","peer":"B"}\r\n'
    d = parse_boot_line(line)
    assert d["fw"] == "m3" and d["channel"] == 1


def test_parse_boot_line_none_on_garbage():
    assert parse_boot_line(b"\x57\x27\xc6") is None


# ---- §8 serial-form obligations, pinned against a fake port ----


class FakeSer:
    """Duck-typed serial port: logs writes, replays queued bytes."""

    def __init__(self):
        self.written = b""
        self.reply = b""

    def write(self, b):
        self.written += b
        return len(b)

    def read(self, n):
        d, self.reply = self.reply[:n], self.reply[n:]
        return d

    def flush(self):
        pass

    def close(self):
        pass


def test_ctrl_writes_compact_framed_bytes():
    fake = FakeSer()
    fake.reply = frame(b'{"channel":6}')
    link = BridgeLink(fake)
    link.open()
    assert link.ctrl({"cmd": "get", "key": "channel"}) == {"channel": 6}
    assert b" " not in fake.written                      # no space bytes
    assert b'", "' not in fake.written
    assert b'": "' not in fake.written
    assert deframe(fake.written) == [b'{"cmd":"get","key":"channel"}']


def test_ctrl_oversize_rejected_before_write():
    fake = FakeSer()
    link = BridgeLink(fake)
    link.open()
    with pytest.raises(LinkError):
        link.ctrl({"cmd": "set", "key": "pmk", "val": "00" * 48})
    assert fake.written == b""          # nothing reached the port


# ---- Review-fix wave 2: event skip, drain-on-ctrl, busy open ----


def test_ctrl_skips_unsolicited_event_payloads():
    """§8: {"event": ...} frames are unsolicited — ctrl must keep
    looping and return the ACTUAL reply, not the event."""
    fake = FakeSer()
    fake.reply = (frame(b'{"event":"air_rx","len":16}')
                  + frame(b'{"channel":6}'))
    link = BridgeLink(fake)
    link.open()
    assert link.ctrl({"cmd": "get", "key": "channel"}) == {"channel": 6}


def test_ctrl_times_out_when_only_events_arrive():
    fake = FakeSer()
    fake.reply = frame(b'{"event":"air_rx"}')
    link = BridgeLink(fake)
    link.open()
    with pytest.raises(LinkTimeout):
        link.ctrl({"cmd": "get", "key": "channel"}, timeout=0.2)


def test_ctrl_drains_stale_reply_from_previous_timeout():
    """ctrl drains the input buffer first: a reply left over from a
    previous timed-out command must not satisfy the next one."""
    fake = FakeSer()
    fake.reply = frame(b'{"channel":99}')   # stale reply
    drained = []

    def drain():
        fake.reply = b""
        drained.append(1)

    fake.reset_input_buffer = drain
    link = BridgeLink(fake)
    link.open()
    with pytest.raises(LinkTimeout):
        link.ctrl({"cmd": "get", "key": "channel"}, timeout=0.2)
    assert drained == [1]               # the drain actually ran


def _install_fake_serial(monkeypatch, errno_value, message):
    """Patch the deferred `import serial` in BridgeLink.open with a
    module whose Serial(port, baud, **kw) raises SerialException."""
    import sys
    import types
    import flrn.link as link_mod

    class SerialException(OSError):
        pass

    def _open(*args, **kwargs):
        raise SerialException(errno_value, message)

    fake = types.ModuleType("serial")
    fake.Serial = _open
    monkeypatch.setattr(link_mod, "port_holder", lambda port: None)
    monkeypatch.setitem(sys.modules, "serial", fake)


def test_open_maps_busy_serial_error_to_port_busy(monkeypatch):
    """exclusive=True open failing 'busy' maps to PortBusyError (this
    covers the race the port_holder pre-check cannot see)."""
    import errno
    _install_fake_serial(monkeypatch, errno.EBUSY,
                         "Device or resource busy")
    link = BridgeLink("/dev/ttyUSB9")
    with pytest.raises(PortBusyError):
        link.open()


def test_open_permission_error_stays_oserror(monkeypatch):
    """EACCES + 'Permission denied' is NOT busy: it must stay OSError
    (no PortBusyError) so the CLI can surface the dialout hint."""
    import errno
    _install_fake_serial(monkeypatch, errno.EACCES,
                         "could not open port: Permission denied")
    link = BridgeLink("/dev/ttyUSB9")
    with pytest.raises(OSError) as e:
        link.open()
    assert not isinstance(e.value, PortBusyError)
