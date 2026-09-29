import json

import pytest
from tests.fakebridge import FakeBridgePair


@pytest.fixture(autouse=True)
def _no_rnsd(monkeypatch):
    """The FakeBridge world has no rnsd and no DTR-reset: on a pty,
    fuser/lsof report the test process itself as the holder (master and
    slave are one object) and the slave rejects TIOCMSET (ENOTTY), so
    the busy pre-check is pinned to free (both module bindings — detect
    imports port_holder by name) and reset_pulse is a no-op (the boot
    line comes from the fake's periodic emitter instead)."""
    import flrn.detect
    import flrn.link
    monkeypatch.setattr(flrn.link, "port_holder", lambda port: None)
    monkeypatch.setattr(flrn.detect, "port_holder", lambda port: None)
    monkeypatch.setattr(flrn.link.BridgeLink, "reset_pulse",
                        lambda self: None)


def test_scan_stats_roundtrip():
    pair = FakeBridgePair()
    ports = pair.start()
    from flrn.commands.scan import probe
    a = probe(ports[0])
    assert a.fw == "m3" and a.mode == "bridge"
    from flrn.commands.stats import stats
    s = stats(ports[0])
    assert s["ack_sent"] == 0
    pair.stop()


def test_config_get_set_and_old_firmware():
    pair = FakeBridgePair(fw="m2")   # old firmware mode
    ports = pair.start()
    from flrn.commands.config import cmd_config_get, OldFirmwareError
    with pytest.raises(OldFirmwareError):
        cmd_config_get.__wrapped__(ports[0]) if hasattr(cmd_config_get, "__wrapped__") else _get_via_probe(ports[0])
    pair.stop()


def _get_via_probe(port):
    from flrn.detect import probe
    from flrn.commands.config import check_fw
    check_fw(probe(port))


def test_link_test_two_fake_bridges():
    pair = FakeBridgePair()
    ports = pair.start()
    from flrn.commands.test import run_link_test
    result = run_link_test(ports[0], ports[1], n=10)
    assert result["in_order"] is True and result["received"] == 10
    pair.stop()


# ---- Ledger coverage (Tasks 6-9 reviews) ----


def test_probe_m1_boot_and_short_boot_defaults():
    pair = FakeBridgePair(fw="m1")
    ports = pair.start()
    from flrn.commands.scan import probe
    b = probe(ports[0])
    assert b.fw == "m1" and b.mode == "unknown"   # m1 boot has no mode
    pair.stop()

    short = FakeBridgePair(short_boot=True)
    ports = short.start()
    from flrn.detect import Bridge
    b2 = probe(ports[0])
    assert b2 == Bridge(port=ports[0], fw="unknown", mac="", peer="",
                        channel=0, mode="unknown")
    short.stop()


def test_probe_silent_bridge_times_out():
    pair = FakeBridgePair(silent=True)
    ports = pair.start()
    from flrn.commands.scan import probe
    from flrn.link import LinkTimeout
    with pytest.raises(LinkTimeout):
        probe(ports[0])
    pair.stop()


def test_probe_busy_carries_holder_hint(monkeypatch):
    import flrn.detect
    from flrn.commands.scan import probe
    from flrn.link import PortBusyError
    monkeypatch.setattr(flrn.detect, "port_holder",
                        lambda port: "4242")
    with pytest.raises(PortBusyError) as e:
        probe("/dev/ttyUSB0")
    assert "rnsd" in e.value.holder_hint


def test_stats_closes_port_and_refills_boot_line():
    pair = FakeBridgePair()
    ports = pair.start()
    from flrn.commands.scan import probe
    from flrn.commands.stats import stats
    stats(ports[0])                      # open + close cycle one
    b = probe(ports[0])                  # second open gets a fresh boot
    assert b.fw == "m3"
    pair.stop()


def test_stats_missing_key_fills_dash():
    pair = FakeBridgePair(stats_omit=("ack_sent", "nak_sent"))
    ports = pair.start()
    from flrn.commands.stats import stats
    s = stats(ports[0])
    assert s["ack_sent"] == "-" and s["nak_sent"] == "-"
    assert s["tx_frames"] == 0
    pair.stop()


def test_stats_watch_max_polls_seam(capsys):
    pair = FakeBridgePair(stats_period=1)
    ports = pair.start()
    from flrn.commands.stats import stats
    d = stats(ports[0], watch=True, max_polls=2)
    assert d["tx_frames"] == 0
    lines = capsys.readouterr().out.strip().splitlines()
    assert len(lines) == 2
    assert all(json.loads(line)["tx_frames"] == 0 for line in lines)
    pair.stop()


def test_config_set_dual_apply_and_reprobe():
    pair = FakeBridgePair()
    ports = pair.start()
    from flrn.commands.config import cmd_config_set
    from flrn.commands.scan import probe
    bridges = [probe(ports[0]), probe(ports[1])]
    result = cmd_config_set(bridges, "channel", 6, force=True)
    assert result["applied"] == {ports[0]: 6, ports[1]: 6}
    assert all(r["channel"] == 6 for r in result["reprobe"].values())
    pair.stop()


def test_config_set_effective_clamp_and_bad_value():
    pair = FakeBridgePair()
    ports = pair.start()
    from flrn.commands.config import cmd_config_set, ConfigError
    from flrn.commands.scan import probe
    bridges = [probe(ports[0]), probe(ports[1])]
    r = cmd_config_set(bridges, "tx_power", 30, force=True)
    assert r["applied"] == {ports[0]: 17, ports[1]: 17}   # clamped echo
    with pytest.raises(ConfigError) as e:
        cmd_config_set(bridges, "channel", 14, force=True)
    assert "bad_value" in str(e.value)
    pair.stop()


def test_config_set_channel_disagreement_raises():
    pair = FakeBridgePair(disagree_channel=7)   # B boots reporting 7
    ports = pair.start()
    from flrn.commands.config import cmd_config_set, ConfigError
    from flrn.commands.scan import probe
    bridges = [probe(ports[0]), probe(ports[1])]
    with pytest.raises(ConfigError) as e:
        cmd_config_set(bridges, "channel", 6, force=True)
    assert "disagree" in str(e.value)
    pair.stop()


def test_cli_exit_codes_busy_and_ok(monkeypatch, capsys):
    import flrn.cli as cli_mod
    import flrn.link
    pair = FakeBridgePair()
    ports = pair.start()
    monkeypatch.setattr(flrn.link, "port_holder",
                        lambda port: "4242")       # rnsd holds it
    assert cli_mod.main(["stats", ports[0]]) == 2  # busy class
    monkeypatch.setattr(flrn.link, "port_holder", lambda port: None)
    assert cli_mod.main(["stats", ports[0]]) == 0  # happy path
    pair.stop()


def test_run_link_test_partial_capture_and_scramble():
    partial = FakeBridgePair(drop_after=3)
    ports = partial.start()
    from flrn.commands.test import run_link_test
    r = run_link_test(ports[0], ports[1], n=10, timeout=3)
    assert r["received"] == 3 and r["in_order"] is True
    partial.stop()

    scrambled = FakeBridgePair(scramble=True)
    ports = scrambled.start()
    r = run_link_test(ports[0], ports[1], n=10, timeout=3)
    assert r["in_order"] is False
    scrambled.stop()


# ---- Review-fix wave 1 ----


def test_scan_skips_silent_without_abort():
    """One good bridge + one silent port: scan returns exactly ONE
    Bridge and does not abort on the dead candidate."""
    pair = FakeBridgePair(silent=True)
    silent_port = pair.start()[0]
    from flrn.commands.scan import scan_bridges
    from flrn.detect import Bridge
    good = FakeBridgePair()
    good_port = good.start()[0]
    try:
        found = scan_bridges(ports=[good_port, silent_port])
        assert len(found) == 1
        assert isinstance(found[0], Bridge) and found[0].fw == "m3"
    finally:
        pair.stop()
        good.stop()


def test_ctrl_newline_tolerance_and_double_newline_data():
    """§8 one-trailing-\\n form is answered as CTRL; \\n\\n falls through
    to DATA (crosses to the peer)."""
    import time
    from flrn.link import BridgeLink, deframe, frame
    pair = FakeBridgePair()
    ports = pair.start()
    la = BridgeLink(ports[0])
    lb = BridgeLink(ports[1])
    la.open()
    lb.open()

    def framed_reply_within():
        buf = b""
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            n = getattr(la._ser, "in_waiting", 0)
            if n:
                buf += la._ser.read(n)
            if deframe(buf):
                return deframe(buf)
            time.sleep(0.02)
        return []

    la._ser.write(frame(b'{"cmd":"get","key":"channel"}\n'))
    assert framed_reply_within() == [b'{"channel":1}']

    def peer_data_within():
        buf = b""
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            n = getattr(lb._ser, "in_waiting", 0)
            if n:
                buf += lb._ser.read(n)
            if deframe(buf):
                return deframe(buf)
            time.sleep(0.02)
        return []

    la._ser.write(frame(b'{"cmd":"get","key":"channel"}\n\n'))
    assert peer_data_within() == [b'{"cmd":"get","key":"channel"}\n\n']
    la.close()
    lb.close()
    pair.stop()


def test_fake_set_strict_numerics_and_pmk_hex():
    """The fake mirrors parseNum (whole-token) and isxdigit×32: malformed
    numerics and non-hex pmk get bad_value; numeric strings parse."""
    pair = FakeBridgePair()
    ports = pair.start()
    from flrn.link import BridgeLink, deframe, frame
    import time
    link = BridgeLink(ports[0])
    link.open()

    def ctrl(raw_req):
        link._ser.write(frame(raw_req))
        buf = b""
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            n = getattr(link._ser, "in_waiting", 0)
            if n:
                buf += link._ser.read(n)
            frames = deframe(buf)
            if frames:
                return frames[0]
            time.sleep(0.02)
        return None

    assert ctrl(b'{"cmd":"set","key":"channel","val":"abc"}') == \
        b'{"err":"bad_value"}'
    assert ctrl(b'{"cmd":"set","key":"channel","val":"6x"}') == \
        b'{"err":"bad_value"}'
    assert ctrl(b'{"cmd":"set","key":"channel","val":"6"}') == \
        b'{"ok":true,"effective":6}'          # numeric string parses
    assert ctrl(b'{"cmd":"set","key":"pmk","val":'
                b'"zz112233445566778899AABBCCDDEEFF"}') == \
        b'{"err":"bad_value"}'                 # non-hex charset
    assert ctrl(b'{"cmd":"set","key":"pmk","val":'
                b'"00112233445566778899AABBCCDDEEFF"}') == \
        b'{"ok":true}'
    link.close()
    pair.stop()


def test_cli_exit_code_fail_class():
    """main() exit code 1: the fail class (silent bridge -> LinkTimeout)."""
    import flrn.cli as cli_mod
    pair = FakeBridgePair(silent=True)
    port = pair.start()[0]
    try:
        assert cli_mod.main(["stats", port]) == 1
    finally:
        pair.stop()
