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
