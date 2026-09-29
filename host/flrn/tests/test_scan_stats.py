from flrn.commands.stats import parse_stats_reply


def test_parse_stats_reply_all_14_keys():
    reply = {"stats": {k: 0 for k in [
        "tx_frames", "tx_frags", "tx_retries", "tx_dropped", "tx_too_big",
        "tx_window_overflow", "meta_conflict", "rx_frags", "rx_delivered",
        "rx_dups", "rx_stale", "rx_window_overflow", "nak_sent", "ack_sent"]}}
    parsed = parse_stats_reply(reply)
    assert len(parsed) == 14 and parsed["ack_sent"] == 0


def test_parse_stats_reply_rejects_wrong_shape():
    import pytest
    with pytest.raises(ValueError):
        parse_stats_reply({"nope": 1})
