"""flrn.commands.doctor — health checks with actionable hints.

Every FAIL line carries its fix (the error taxonomy's hints surface
verbatim: PortBusyError.holder_hint, LinkTimeout's dual hypotheses,
OldFirmwareError's reflash command). Dual-instance tolerance: rnsd
holding a port, OR two config dirs each with a SerialInterface, are
both healthy topologies.
"""

from pathlib import Path
import getpass
import grp
import os
import pwd
import re
import shutil

from ..detect import fw_version, list_ch340_ports, probe
from ..link import LinkError, LinkTimeout, PortBusyError, port_holder
from .config import REFLASH_CMD


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

    # Host prerequisites -------------------------------------------------
    try:
        import serial  # noqa: F401
        checks.append(("pyserial", True, "importable"))
    except ImportError:
        checks.append(("pyserial", False,
                       "not importable — pip install pyserial "
                       "(or pipx install flrn, which pulls it in)"))
    if shutil.which("rnsd") is None:
        checks.append(("rns", False,
                       "rnsd not on PATH — pipx install rns && pipx "
                       "ensurepath (then re-login)"))
    else:
        checks.append(("rns", True,
                       f"rnsd at {shutil.which('rnsd')}"))
    user = getpass.getuser()
    try:
        grp_entry = grp.getgrnam("dialout")
        in_dialout = (user in grp_entry.gr_mem or
                      pwd.getpwuid(os.getuid()).pw_gid == grp_entry.gr_gid)
    except KeyError:
        in_dialout = False
    if in_dialout:
        checks.append(("dialout", True, f"{user} is a member"))
    else:
        checks.append(("dialout", False,
                       f"{user} is not in the dialout group — "
                       "sudo usermod -aG dialout "
                       f"{user} (then log out/in)"))

    # Bridges ------------------------------------------------------------
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
                               f"reflash: {REFLASH_CMD}"))
        except PortBusyError as e:
            checks.append((f"port {p}", False, e.holder_hint or str(e)))
        except (LinkTimeout, LinkError) as e:
            checks.append((f"port {p}", False, f"unresponsive: {e}"))

    for d in _config_dirs(config_dir):
        cfg = d / "config"
        ok, detail = check_config_format(cfg)
        name = f"config {d.name}"
        checks.append((name, ok, f"{cfg}: {detail}"))

    # Configured port vs attached reality (catches wrong-id/wrong-ttyUSB)
    primary_cfg = Path(config_dir).expanduser() / "config"
    configured = None
    try:
        m = re.search(r"^port\s*=\s*(\S+)",
                      primary_cfg.read_text(), re.MULTILINE)
        configured = m.group(1) if m else None
    except OSError:
        pass
    if configured:
        attached = list_ch340_ports()
        if configured in attached:
            checks.append(("config-port", True,
                           f"{configured} is attached"))
        else:
            checks.append(("config-port", False,
                           f"config points at {configured} but attached "
                           f"CH340 ports are: "
                           f"{', '.join(attached) or 'none'} — fix the "
                           "port line or replug the bridge"))

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
