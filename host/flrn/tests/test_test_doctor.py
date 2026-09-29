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
