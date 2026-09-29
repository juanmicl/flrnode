"""flrn.commands.setup — the installer engine (docs/host-setup.md).

Detects the bridge, writes the 2025-12 RNS config block atomically
(temp + rename), and prints the rnsd next steps. Idempotent-ish: an
existing config is kept unless --yes-setup confirms a rewrite.
"""

import os
import shutil
from pathlib import Path

from ..detect import list_ch340_ports, probe
from ..link import LinkError, LinkTimeout, PortBusyError

CONFIG_TEMPLATE = """[reticulum]
  enable_transport = yes
  share_instance = yes

[[Default Interface]]
  type = SerialInterface
  interface_enabled = yes
  port = {port}
  speed = 921600
  databits = 8
  parity = none
  stopbits = 1
"""


def _atomic_write(path: Path, text: str) -> None:
    """Temp file in the same directory + os.replace (atomic on POSIX)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(text)
    os.replace(tmp, path)


def run_setup(assume_yes: bool = False,
              config_dir: str = "~/.reticulum") -> None:
    """Provision this host for one bridge: detect it, write
    <config_dir>/config (atomic; kept unless --yes-setup), print the
    rnsd next steps. Exits non-zero via SystemExit on blockers."""
    ports = list_ch340_ports()
    if not ports:
        raise SystemExit(
            "no CH340 bridge found — plug in the D1 mini (USB) and "
            "retry; if it is held by rnsd: systemctl --user stop rnsd")
    target = ports[0]
    try:
        bridge = probe(target)
    except (PortBusyError, LinkError, LinkTimeout) as e:
        raise SystemExit(f"bridge on {target} not usable: {e}")
    print(f"bridge: {target} fw {bridge.fw} mode {bridge.mode} "
          f"ch {bridge.channel} mac {bridge.mac}")

    if shutil.which("rnsd") is None:
        print("HINT: rnsd not on PATH — install with: pipx install rns "
              "&& pipx ensurepath (then re-login)")
    cfg_path = Path(config_dir).expanduser() / "config"
    if cfg_path.exists() and not assume_yes:
        print(f"keeping existing {cfg_path} (pass --yes-setup to "
              "overwrite)")
    else:
        _atomic_write(cfg_path, CONFIG_TEMPLATE.format(port=target))
        print(f"wrote {cfg_path} (port {target}, 921600)")
    print("next: start rnsd on this host (rnsd -v) and repeat on the "
          "second Pi/bridge; then rnstatus / rnspong (docs/host-setup.md)")
