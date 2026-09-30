import pytest

from flrn.commands.test import make_frames, IN_ORDER_SIZES


def test_make_frames_sizes_and_content():
    frames = make_frames()
    assert len(frames) == 50
    assert frames[0].startswith(b"\x00")           # seed byte = index
    assert sorted(set(len(f) for f in frames)) == sorted(IN_ORDER_SIZES)


def test_check_in_order():
    from flrn.commands.test import check_in_order
    assert check_in_order([b"\x00a", b"\x01b"]) is True
    assert check_in_order([b"\x01a", b"\x00b"]) is False   # same size, wrong order


from flrn.commands.doctor import check_config_format


def test_config_format_accepts_2025_12(tmp_path):
    cfg = tmp_path / "config"
    cfg.write_text("[reticulum]\n[interfaces]\n[[Default Interface]]\n"
                   "type = SerialInterface\nport = /dev/ttyUSB0\nspeed = 921600\n")
    ok, detail = check_config_format(cfg)
    assert ok


def test_config_format_rejects_old_format(tmp_path):
    cfg = tmp_path / "config"
    cfg.write_text("[reticulum]\n[[Default Interface]]\ntype = SerialInterface\n"
                   "serial_port = /dev/ttyUSB0\nserial_speed = 921600\n")
    ok, detail = check_config_format(cfg)
    assert not ok and "serial_port" in detail


# ---- Review-fix wave 1: setup/doctor roundtrip + spec checks ----


def test_setup_writes_config_doctor_accepts(tmp_path, monkeypatch):
    """setup's template must satisfy doctor's format check (the
    [interfaces] parent roundtrip)."""
    import flrn.commands.setup as setup_mod
    from flrn.detect import Bridge

    monkeypatch.setattr(setup_mod, "list_ch340_ports",
                        lambda: ["/dev/ttyUSB0"])
    monkeypatch.setattr(
        setup_mod, "probe",
        lambda port: Bridge(port=port, fw="m3", mac="A", peer="B",
                            channel=1, mode="bridge"))
    setup_mod.run_setup(assume_yes=True, config_dir=str(tmp_path))
    from flrn.commands.doctor import check_config_format
    ok, detail = check_config_format(tmp_path / "config")
    assert ok, detail


def test_doctor_rows_shape_headless(tmp_path, monkeypatch):
    """run_doctor returns (name, ok, detail) rows for every check,
    including the spec checks (pyserial/rns/dialout/config-port) —
    verdicts are host-dependent; shapes are not."""
    import flrn.commands.doctor as doctor_mod

    monkeypatch.setattr(doctor_mod, "list_ch340_ports", lambda: [])
    rows = doctor_mod.run_doctor(config_dir=str(tmp_path))
    names = {n for n, _, _ in rows}
    for expected in ("pyserial", "rns", "dialout", "ch340-ports",
                     "topology"):
        assert expected in names, (expected, names)
    for n, ok, d in rows:
        assert isinstance(n, str) and n
        assert isinstance(ok, bool)
        assert isinstance(d, str) and d


def test_config_get_without_port_is_clean_error():
    """`flrn config get <key>` without --port: clean SystemExit, not a
    probe(None) traceback."""
    from flrn.cli import main
    with pytest.raises(SystemExit) as e:
        main(["config", "get", "channel"])
    assert "--port" in str(e.value)


def test_config_set_all_bridges_needs_no_port(monkeypatch):
    """`flrn config set channel 6 --all-bridges` (the gate-3 scripted
    invocation) must proceed WITHOUT --port and reach the config module
    with both scanned bridges."""
    import flrn.cli as cli_mod
    from flrn.detect import Bridge

    bridges = [Bridge(port="/dev/a", fw="m3", mac="A", peer="B",
                      channel=1, mode="bridge"),
               Bridge(port="/dev/b", fw="m3", mac="A", peer="B",
                      channel=1, mode="bridge")]
    monkeypatch.setattr(cli_mod, "scan_bridges", lambda: bridges)
    calls = {}

    def fake_set(bridges_arg, key, val, **kw):
        calls["args"] = (bridges_arg, key, val, kw)
        return {"applied": {}, "reprobe": {}}

    monkeypatch.setattr(cli_mod, "cmd_config_set", fake_set)
    assert cli_mod.main(["config", "set", "channel", "6",
                         "--all-bridges"]) == 0
    assert [b.port for b in calls["args"][0]] == ["/dev/a", "/dev/b"]
    assert calls["args"][1] == "channel" and calls["args"][2] == 6


# ---- Review-fix wave 2: CLI error surface ----


def test_cli_test_n_above_255_rejected():
    """-n > 255 wraps the i & 0xFF seed byte and forces false FAIL
    verdicts — argparse rejects it cleanly (exit 2, no ports opened)."""
    from flrn.cli import main
    with pytest.raises(SystemExit) as e:
        main(["test", "/dev/a", "/dev/b", "-n", "300"])
    assert e.value.code == 2


def test_cli_port_and_all_bridges_mutually_exclusive():
    from flrn.cli import main
    with pytest.raises(SystemExit) as e:
        main(["config", "set", "channel", "6",
              "--port", "/dev/a", "--all-bridges"])
    assert e.value.code == 2


def test_cli_config_set_all_bridges_zero_found(monkeypatch):
    """--all-bridges with a scan that finds ZERO bridges: clean error,
    not a silent {"applied": {}} success."""
    import flrn.cli as cli_mod
    monkeypatch.setattr(cli_mod, "scan_bridges", lambda: [])
    with pytest.raises(SystemExit) as e:
        cli_mod.main(["config", "set", "channel", "6", "--all-bridges"])
    assert "no bridges found" in str(e.value)


def test_cli_keyboard_interrupt_exits_zero(monkeypatch):
    """Ctrl-C (the normal exit for --watch) is silence + exit 0."""
    import flrn.cli as cli_mod

    def interrupted(*a, **k):
        raise KeyboardInterrupt()

    monkeypatch.setattr(cli_mod, "stats_cmd", interrupted)
    assert cli_mod.main(["stats", "/dev/ttyUSB0"]) == 0


def test_cli_oserror_from_open_gets_dialout_hint(monkeypatch, capsys):
    """Missing dialout rights -> open() raises OSError (SerialException
    is one): exit 1, port named, dialout hint printed, no traceback."""
    import flrn.commands.stats as stats_mod

    class DeniedLink:
        def __init__(self, port, baud=921600):
            pass

        def open(self):
            raise OSError(13, "Permission denied")

        def close(self):
            pass

    monkeypatch.setattr(stats_mod, "BridgeLink", DeniedLink)
    from flrn.cli import main
    assert main(["stats", "/dev/ttyUSB0"]) == 1
    err = capsys.readouterr().err
    assert "/dev/ttyUSB0" in err
    assert "dialout" in err and "usermod" in err


def test_cli_generic_oserror_exits_1_without_dialout_hint(monkeypatch,
                                                          capsys):
    """A non-permission OSError still exits 1 cleanly — without the
    (wrong) dialout advice."""
    import flrn.commands.stats as stats_mod

    class DeadLink:
        def __init__(self, port, baud=921600):
            pass

        def open(self):
            raise OSError("no such device")

        def close(self):
            pass

    monkeypatch.setattr(stats_mod, "BridgeLink", DeadLink)
    from flrn.cli import main
    assert main(["stats", "/dev/ttyUSB0"]) == 1
    err = capsys.readouterr().err
    assert "no such device" in err
    assert "dialout" not in err


def test_config_format_rejects_enabled_key(tmp_path):
    """`enabled = yes` is not a real RNS interface key — the real key
    is `interface_enabled`; doctor must not bless such a file."""
    from flrn.commands.doctor import check_config_format
    cfg = tmp_path / "config"
    cfg.write_text("[reticulum]\n[interfaces]\n[[Default Interface]]\n"
                   "type = SerialInterface\nenabled = yes\n"
                   "port = /dev/ttyUSB0\nspeed = 921600\n")
    ok, detail = check_config_format(cfg)
    assert not ok and "interface_enabled" in detail
