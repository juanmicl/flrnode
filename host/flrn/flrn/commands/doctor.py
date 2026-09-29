"""flrn.commands.doctor — health checks with actionable hints.

Every FAIL line carries its fix (the error taxonomy's hints surface
verbatim: PortBusyError.holder_hint, LinkTimeout's dual hypotheses,
OldFirmwareError's reflash command). Dual-instance tolerance: rnsd
holding a port, OR two config dirs each with a SerialInterface, are
both healthy topologies.
"""

from pathlib import Path

from ..detect import fw_version, list_ch340_ports, probe
from ..link import LinkError, LinkTimeout, PortBusyError, port_holder

REFLASH_HINT = "reflash: pio run -e d1_mini_bridge -t upload"


def check_config_format(path) -> tuple[bool, str]:
    """Validate one RNS config file's SerialInterface shape: the
    2025-12 format uses port/speed under an [interfaces] parent; the
    pre-2025-12 keys (serial_port/serial_speed) are rejected."""
    try:
        text = Path(path).read_text()
    except OSError as e:
        return False, f"unreadable: {e}"
    if "serial_port" in text or "serial_speed" in text:
        return False, ("pre-2025-12 keys found (serial_port/"
                       "serial_speed) — migrate to port/speed under "
                       "[interfaces]")
    needed = ("[interfaces]", "SerialInterface", "port = ", "speed = ")
    missing = [k for k in needed if k not in text]
    if missing:
        return False, ("missing SerialInterface bits: "
                       + ", ".join(missing))
    return True, "2025-12 format (SerialInterface, port/speed)"


def _config_dirs(config_dir: str) -> list[Path]:
    """The primary config dir plus any sibling ~/.reticulum* dirs
    (dual-instance setups keep one SerialInterface per dir)."""
    primary = Path(config_dir).expanduser()
    home = primary.parent
    dirs = {primary}
    for p in home.glob(primary.name + "*"):
        if p.is_dir():
            dirs.add(p)
    return sorted(dirs)


def run_doctor(config_dir: str = "~/.reticulum") -> list[tuple[str, bool, str]]:
    """Run every check; returns (name, ok, detail) rows — ok=False rows
    always carry the fix in detail."""
    checks: list[tuple[str, bool, str]] = []

    ports = list_ch340_ports()
    checks.append((
        "ch340-ports", bool(ports),
        f"found {len(ports)}: {', '.join(ports)}" if ports else
        "no CH340 (1a86:7523) visible — check USB cable and power"))

    rnsd_holding = False
    for p in ports:
        holder = port_holder(p)
        if holder:
            rnsd_holding = True
            checks.append((f"port {p}", True,
                           f"in use by {holder} (rnsd attached — healthy)"))
            continue
        try:
            b = probe(p)
            v = fw_version(b.fw)
            if v >= 3:
                checks.append((f"port {p}", True,
                               f"fw {b.fw} mode {b.mode} ch {b.channel} "
                               f"mac {b.mac}"))
            else:
                checks.append((f"port {p}", False,
                               f"fw {b.fw} predates config — "
                               + REFLASH_HINT))
        except PortBusyError as e:
            checks.append((f"port {p}", False, e.holder_hint or str(e)))
        except (LinkTimeout, LinkError) as e:
            checks.append((f"port {p}", False, f"unresponsive: {e}"))

    for d in _config_dirs(config_dir):
        cfg = d / "config"
        ok, detail = check_config_format(cfg)
        name = f"config {d.name}"
        checks.append((name, ok, f"{cfg}: {detail}"))

    serial_dirs = [d for d in _config_dirs(config_dir)
                   if (d / "config").exists()
                   and check_config_format(d / "config")[0]]
    if rnsd_holding or serial_dirs:
        checks.append(("topology", True,
                       "rnsd attached" if rnsd_holding else
                       f"{len(serial_dirs)} config dir(s) with a "
                       "SerialInterface — healthy (single or dual "
                       "instance)"))
    else:
        checks.append(("topology", False,
                       "no rnsd on the ports and no valid config — "
                       "run flrn setup"))
    return checks
