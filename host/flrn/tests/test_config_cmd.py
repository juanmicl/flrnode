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
