"""flrn.commands.stats — fetch and validate the bridge stats reply.

The reply schema is pinned in docs/protocol.md §8: {"stats": {14 keys}}
in the firmware's order. Parsing is defensive: missing keys render as
"-", a reply without a stats object is a hard ValueError.
"""

import json
import time

from ..link import BridgeLink, LinkError

# Canonical order = firmware emit order (§8).
STATS_KEYS = (
    "tx_frames", "tx_frags", "tx_retries", "tx_dropped", "tx_too_big",
    "tx_window_overflow", "meta_conflict", "rx_frags", "rx_delivered",
    "rx_dups", "rx_stale", "rx_window_overflow", "nak_sent", "ack_sent",
)


def parse_stats_reply(reply: dict) -> dict:
    """Extract the 14-key stats dict from a CTRL reply. Missing keys are
    filled with "-" (render as missing); a reply without a stats object
    raises ValueError."""
    if not isinstance(reply, dict) or not isinstance(reply.get("stats"), dict):
        raise ValueError(
            f"malformed stats reply: expected {{'stats': {{…14 keys…}}}}, "
            f"got {reply!r}")
    stats = reply["stats"]
    return {key: stats.get(key, "-") for key in STATS_KEYS}


def _stats_period(link: BridgeLink) -> float:
    """Live stats_period from the bridge (CTRL get); default 5 s."""
    try:
        d = link.ctrl({"cmd": "get", "key": "stats_period"})
        return float(d.get("stats_period", 5))
    except (LinkError, KeyError, TypeError, ValueError):
        return 5.0


def stats(port: str, watch: bool = False) -> dict:
    """Fetch the bridge's counters (and print them once per period when
    watch=True, until interrupted). PortBusyError propagates loudly with
    its holder hint; LinkTimeout on a silent bridge. Returns the 14-key
    dict (single fetch when watch=False)."""
    link = BridgeLink(port)
    link.open()
    try:
        while True:
            d = parse_stats_reply(link.ctrl({"cmd": "stats"}))
            if not watch:
                return d
            print(json.dumps(d))  # one machine-parseable line per poll
            period = _stats_period(link)
            time.sleep(max(period, 1.0))
    finally:
        link.close()
