import pytest
from flrn.commands.config import check_fw, OldFirmwareError


def test_check_fw_passes_m3():
    from flrn.detect import Bridge
    b = Bridge(port="/dev/x", fw="m3", mac="A", peer="B", channel=1, mode="bridge")
    check_fw(b)  # no raise


def test_check_fw_rejects_m2_with_reflash_hint():
    from flrn.detect import Bridge
    b = Bridge(port="/dev/x", fw="m2", mac="A", peer="B", channel=1, mode="bridge")
    with pytest.raises(OldFirmwareError) as e:
        check_fw(b)
    assert "pio run -e d1_mini_bridge -t upload" in str(e.value)


def test_set_channel_requires_force_with_one_bridge():
    # the dual sequence helper: single bridge + channel/pmk -> force required
    from flrn.commands.config import validate_set_preconditions
    from flrn.detect import Bridge
    b = [Bridge(port="/dev/x", fw="m3", mac="A", peer="B", channel=1, mode="bridge")]
    with pytest.raises(SystemExit):
        validate_set_preconditions(b, "channel", 6, force=False)
    validate_set_preconditions(b, "channel", 6, force=True)  # ok


def test_pmk_requires_yes():
    from flrn.commands.config import validate_set_preconditions
    from flrn.detect import Bridge
    b = [Bridge(port="/dev/x", fw="m3", mac="A", peer="B", channel=1, mode="bridge")]
    with pytest.raises(SystemExit):
        validate_set_preconditions(b, "pmk", "0011..", force=True, yes=False)


def test_partial_apply_reports_desync(monkeypatch):
    # A applies, B rejects -> ConfigError naming the DESYNCED state and
    # the applied port; resets still ran on both (reset-then-raise).
    import flrn.commands.config as cfgmod
    from flrn.detect import Bridge

    class FakeLink:
        def __init__(self, replies):
            self.replies = list(replies)
            self.resets = 0
            self.closed = False

        def open(self):
            pass

        def ctrl(self, request):
            r = self.replies.pop(0)
            if isinstance(r, Exception):
                raise r
            return r

        def reset_pulse(self):
            self.resets += 1

        def close(self):
            self.closed = True

    a = Bridge(port="/dev/a", fw="m3", mac="A", peer="B", channel=1, mode="bridge")
    b = Bridge(port="/dev/b", fw="m3", mac="A", peer="B", channel=1, mode="bridge")
    replies = {"/dev/a": [{"ok": True, "effective": 6}],
               "/dev/b": [{"err": "bad_value"}]}
    made = []

    def factory(port, baud=921600):
        fl = FakeLink(replies[port])
        made.append(fl)
        return fl

    monkeypatch.setattr(cfgmod, "BridgeLink", factory)
    with pytest.raises(cfgmod.ConfigError) as e:
        cfgmod.cmd_config_set([a, b], "channel", 6, force=True)
    msg = str(e.value)
    assert "DESYNCED" in msg
    assert "/dev/a" in msg and "/dev/b" in msg
    assert "re-run the same set on both bridges" in msg
    assert made[0].resets == 1 and made[1].resets == 1   # reset-then-raise
    assert made[0].closed and made[1].closed
