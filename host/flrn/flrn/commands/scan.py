"""flrn.commands.scan — enumerate CH340 ports and probe each one.

Composition only: enumeration from detect.list_ch340_ports, identity
from detect.probe. Held/unresponsive ports are SKIPPED (a scan must not
fight rnsd or abort on one dead port); table rendering lives in cli.py.
"""

from ..detect import Bridge, list_ch340_ports, probe
from ..link import LinkError, LinkTimeout, PortBusyError


def scan_bridges(ports: list[str] | None = None) -> list[Bridge]:
    """Probe CH340 ports (or an explicit candidate list) and return the
    bridges that answered. Held or unresponsive ports are omitted, not
    errors. Context: any. Blocking: probe-bound (2 s per port).
    Failure: none raised — failures are omissions."""
    candidates = ports if ports is not None else list_ch340_ports()
    found: list[Bridge] = []
    for port in candidates:
        try:
            found.append(probe(port))
        except (PortBusyError, LinkTimeout, LinkError, OSError):
            continue  # held / wrong firmware / dead — omit, keep scanning
    return found
