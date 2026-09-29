from flrn.detect import fw_version


def test_fw_version_numeric():
    assert fw_version("m1") == 1
    assert fw_version("m2") == 2
    assert fw_version("m3") == 3
    assert fw_version("m10") == 10      # numeric parse, not lexicographic
    assert fw_version("unknown") == 0
    assert fw_version("") == 0
