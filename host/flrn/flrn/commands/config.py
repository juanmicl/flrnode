"""flrn.commands.config — get/set bridge settings with the dual-bridge
apply sequence.

A settings change that can desync the radio pair (channel, pmk) must be
applied to BOTH bridges: set A, set B, reset A, reset B, then re-probe
and compare the boot lines. Single-bridge applies of those keys demand
--force; pmk additionally demands --yes. The firmware's error
vocabulary (§8) maps to clear local errors; `unknown_cmd` maps to
OldFirmwareError (pre-M3 bridge).
"""

from ..detect import Bridge, fw_version, probe
from ..link import BridgeLink, LinkError

REFLASH_CMD = "pio run -e d1_mini_bridge -t upload"


class OldFirmwareError(RuntimeError):
    """The bridge predates the M3 CTRL get/set vocabulary."""


class ConfigError(RuntimeError):
    """The bridge refused or broke a config operation."""


def check_fw(bridge: Bridge) -> None:
    """Raise OldFirmwareError (with the reflash command) unless the
    bridge runs m3 or newer."""
    if fw_version(bridge.fw) < 3:
        raise OldFirmwareError(
            f"bridge on {bridge.port} runs fw {bridge.fw}; config needs "
            f"m3 or newer — reflash: {REFLASH_CMD}")


def _err_to_error(reply: dict, port: str, action: str) -> RuntimeError:
    """Map a §8 {"err": ...} reply to a local exception."""
    err = reply.get("err", "unknown_cmd")
    if err == "unknown_cmd":
        return OldFirmwareError(
            f"bridge on {port} predates get/set — reflash: {REFLASH_CMD}")
    if err == "bad_value":
        return ConfigError(
            f"{action} on {port} rejected (bad_value): out of range or "
            "malformed — channel is 1-13, tx_power clamps, pmk needs 32 "
            "hex chars")
    if err == "write_only":
        return ConfigError(f"{action} on {port} refused: write_only key")
    if err == "unsupported_transport":
        return ConfigError(
            f"{action} on {port} unsupported on this transport "
            "(e.g. rate on ESP-NOW)")
    return ConfigError(f"{action} on {port} failed: {err}")


def validate_set_preconditions(bridges: list[Bridge], key: str, val,
                               *, force: bool = False,
                               yes: bool = False) -> None:
    """Gate a set before any wire traffic. Exits (SystemExit, CLI-grade
    message) when the operation needs --force (channel/pmk on ONE
    bridge only desyncs the pair) or --yes (pmk re-keys the link)."""
    if key == "pmk" and not yes:
        raise SystemExit(
            "pmk changes re-key the encrypted link on BOTH bridges and "
            "can lock you out — pass --yes to confirm")
    if len(bridges) == 1 and key in ("channel", "pmk") and not force:
        raise SystemExit(
            f"setting {key} on ONE bridge desyncs the pair — pass both "
            "bridges or --force")
    if key == "channel" and (not isinstance(val, int)
                             or not 1 <= val <= 13):
        raise SystemExit(
            f"channel must be an integer in 1-13 (EU band plan), got "
            f"{val!r}")


def cmd_config_get(bridge: Bridge, key: str) -> dict:
    """CTRL get on one bridge (fw-gated). Returns the reply dict, e.g.
    {"channel": 6}. Raises OldFirmwareError / ConfigError / link errors."""
    check_fw(bridge)
    link = BridgeLink(bridge.port)
    link.open()
    try:
        reply = link.ctrl({"cmd": "get", "key": key})
    finally:
        link.close()
    if "err" in reply:
        raise _err_to_error(reply, bridge.port, f"get {key!r}")
    return reply


def cmd_config_set(bridges: list[Bridge], key: str, val, *,
                   force: bool = False, yes: bool = False,
                   do_reset: bool = True) -> dict:
    """Apply a setting to every bridge in the pinned order — set A, set
    B, then reset A, reset B — and re-probe each, asserting agreement
    (channel equality on the boot lines). Returns
    {"applied": {port: effective}, "reprobe": {port: {...}}}.

    Raises SystemExit (preconditions), OldFirmwareError (fw gate or
    unknown_cmd), ConfigError (any {"err": ...} reply — after resets,
    so a partial apply leaves a clear trail), or link errors."""
    validate_set_preconditions(bridges, key, val, force=force, yes=yes)
    for b in bridges:
        check_fw(b)

    opened: list[BridgeLink] = []
    applied: dict = {}
    try:
        for b in bridges:
            link = BridgeLink(b.port)
            link.open()
            opened.append(link)
            reply = link.ctrl({"cmd": "set", "key": key, "val": val})
            if "err" in reply:
                raise _err_to_error(reply, b.port, f"set {key}={val!r}")
            applied[b.port] = reply.get("effective", True)
    finally:
        # Pinned order: every set first, THEN resets in the same order.
        for link in opened:
            if do_reset:
                try:
                    link.reset_pulse()  # reboots into the new config
                except (LinkError, OSError):
                    pass  # close still matters; probe reports the rest
            link.close()

    reprobe: dict = {}
    for b in bridges:
        nb = probe(b.port)
        reprobe[b.port] = {"fw": nb.fw, "channel": nb.channel,
                           "mode": nb.mode}
    if key == "channel":
        chans = {r["channel"] for r in reprobe.values()}
        if len(chans) != 1:
            raise ConfigError(
                f"bridges disagree on channel after apply: {reprobe} — "
                "fix by applying the same channel to both")
    return {"applied": applied, "reprobe": reprobe}
