import pytest
from flrn.link import frame, deframe, parse_boot_line


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


def test_parse_boot_line():
    line = b'{"t":"boot","fw":"m3","mode":"bridge","channel":1,"mac":"A","peer":"B"}\r\n'
    d = parse_boot_line(line)
    assert d["fw"] == "m3" and d["channel"] == 1


def test_parse_boot_line_none_on_garbage():
    assert parse_boot_line(b"\x57\x27\xc6") is None
