"""flrn.detect — CH340 USB enumeration and boot-line probing.

The D1 mini's USB-serial chip is a CH340: VID 0x1A86, PID 0x7523.
Candidates come from sysfs (no hardware open); a candidate is confirmed
by probing the serial port for the bridge's boot line, first at 921600
(bridge firmware) then at 115200 (M1 ping-pong firmware).
"""

from dataclasses import dataclass
import glob
import os

from .link import BridgeLink, LinkError, LinkTimeout, PortBusyError, port_holder

_CH340_VID = "1a86"
_CH340_PID = "7523"
_PROBE_BAUDS = (921600, 115200)
_PROBE_TIMEOUT_S = 2.0


@dataclass
class Bridge:
    """Identity of one probed bridge, from its boot line."""

    port: str
    fw: str            # "m3" / "m2" / "m1" / "unknown"
    mac: str
    peer: str
    channel: int
    mode: str


def fw_version(fw: str) -> int:
    """Numeric milestone from the fw boot field ("m3" -> 3, "m10" ->
    10); anything unparsable (including "unknown" / "") is 0."""
    if not fw.startswith("m"):
        return 0
    try:
        return int(fw[1:])
    except ValueError:
        return 0


def _usb_attr(tty_sysfs: str, name: str) -> str | None:
    """Read a USB-device attribute for a tty (idVendor/idProduct live at
    the tty device's PARENT — the interface's USB device)."""
    path = os.path.join(tty_sysfs, "device", "..", name)
    try:
        with open(path) as f:
            return f.read().strip().lower()
    except OSError:
        return None


def list_ch340_ports() -> list[str]:
    """Enumerate /dev/ttyUSB*|ttyACM* devices whose USB device is a
    CH340 (1a86:7523). Pure sysfs reads — never opens the ports."""
    out: list[str] = []
    for path in sorted(glob.glob("/sys/class/tty/*")):
        if _usb_attr(path, "idVendor") != _CH340_VID:
            continue
        if _usb_attr(path, "idProduct") != _CH340_PID:
            continue
        out.append("/dev/" + os.path.basename(path))
    return out


def probe(port: str) -> Bridge:
    """Probe one port for a bridge: busy pre-check (rnsd holds ports
    without failing exclusive open), then reset + boot-line read at
    921600 (bridge) with one 115200 retry (M1 ping-pong firmware).
    A malformed boot line (e.g. a channel that is not an int) counts
    as no bridge at that baud and falls through to the retry.
    Raises PortBusyError (with .holder_hint) or LinkTimeout."""
    holder = port_holder(port)
    if holder:
        raise PortBusyError(
            f"{port} is held by {holder}",
            holder_hint=(f"rnsd is using {port} — "
                         "systemctl --user stop rnsd or pass --port"),
        )
    last_error: Exception | None = None
    for baud in _PROBE_BAUDS:
        link = BridgeLink(port, baud)
        try:
            link.open()
            try:
                link.reset_pulse()
                boot = link.read_boot_line(timeout=_PROBE_TIMEOUT_S)
            finally:
                link.close()
            return Bridge(
                port=port,
                fw=str(boot.get("fw", "unknown")),
                mac=str(boot.get("mac", "")),
                peer=str(boot.get("peer", "")),
                channel=int(boot.get("channel", 0)),
                mode=str(boot.get("mode", "unknown")),
            )
        except (LinkTimeout, LinkError, OSError, ValueError) as e:
            # ValueError: boot line parsed but a field is malformed —
            # not a bridge at this baud; try the next one.
            last_error = e
            continue
    raise LinkTimeout(
        f"no boot line from {port} at 921600 or 115200 "
        f"({last_error})")
