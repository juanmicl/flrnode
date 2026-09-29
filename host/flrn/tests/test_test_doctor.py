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
