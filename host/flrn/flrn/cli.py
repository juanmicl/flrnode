"""flrn.cli — argparse wiring and human/JSON rendering for the commands.

Exit codes: 0 success; 1 operation failed (timeout, config error,
old firmware); 2 port busy (holder hint printed). SystemExit from
precondition validation propagates (argparse-grade message + code 1).
"""

import argparse
import json
import sys

from .commands.config import (cmd_config_get, cmd_config_set,
                              ConfigError, OldFirmwareError)
from .commands.doctor import run_doctor
from .commands.scan import scan_bridges
from .commands.setup import run_setup
from .commands.stats import stats as stats_cmd
from .commands.test import run_link_test
from .detect import probe
from .link import LinkError, LinkTimeout, PortBusyError

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_BUSY = 2


def _bridges_from_args(args) -> list:
    """Bridges named by --port, or every CH340 bridge for
    --all-bridges (probe failures on --all-bridges are skipped)."""
    if getattr(args, "all_bridges", False):
        return scan_bridges()
    if not args.port:
        raise SystemExit("pass --port /dev/ttyUSB0 or --all-bridges")
    return [probe(args.port)]


def _render_scan(bridges, as_json: bool) -> None:
    if as_json:
        print(json.dumps([b.__dict__ for b in bridges]))
        return
    print(f"{'PORT':<16} {'FW':<4} {'MODE':<8} {'CH':>3} "
          f"{'MAC':<13} {'PEER':<13}")
    for b in bridges:
        print(f"{b.port:<16} {b.fw:<4} {b.mode:<8} {b.channel:>3} "
              f"{b.mac:<13} {b.peer:<13}")
    if not bridges:
        print("no bridges found (see: flrn doctor)")


def _render_stats(d, as_json: bool) -> None:
    if as_json:
        print(json.dumps(d))
        return
    width = max(len(k) for k in d)
    for k, v in d.items():
        print(f"{k:<{width}} : {v}")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        prog="flrn", description="FLRNode bridge command line")
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("scan", help="find and identify bridges")
    sp.add_argument("--json", action="store_true")

    st = sub.add_parser("stats", help="show a bridge's ARQ counters")
    st.add_argument("port")
    st.add_argument("--json", action="store_true")
    st.add_argument("--watch", action="store_true")

    cf = sub.add_parser("config", help="get/set bridge settings")
    cf.add_argument("action", choices=["get", "set"])
    cf.add_argument("key")
    cf.add_argument("val", nargs="?")
    cf.add_argument("--port")
    cf.add_argument("--all-bridges", action="store_true")
    cf.add_argument("--force", action="store_true")
    cf.add_argument("--yes", action="store_true")

    tp = sub.add_parser("test", help="over-air link test between two bridges")
    tp.add_argument("portA")
    tp.add_argument("portB")
    tp.add_argument("-n", type=int, default=50)
    tp.add_argument("--json", action="store_true")

    dp = sub.add_parser("doctor", help="health checks with fixes")
    dp.add_argument("--json", action="store_true")
    dp.add_argument("--config-dir", default="~/.reticulum")

    up = sub.add_parser("setup", help="write the RNS config for a bridge")
    up.add_argument("--yes-setup", action="store_true")
    up.add_argument("--config-dir", default="~/.reticulum")

    args = p.parse_args(argv)
    try:
        if args.cmd == "scan":
            _render_scan(scan_bridges(), args.json)
        elif args.cmd == "stats":
            _render_stats(stats_cmd(args.port, watch=args.watch),
                          args.json)
        elif args.cmd == "config":
            if args.action == "get":
                bridge = probe(args.port)
                print(json.dumps(
                    cmd_config_get(bridge, args.key)))
            else:
                if args.val is None:
                    raise SystemExit("config set needs a value")
                val = _coerce(args.val)
                bridges = _bridges_from_args(args)
                result = cmd_config_set(
                    bridges, args.key, val, force=args.force,
                    yes=args.yes)
                print(json.dumps(result))
        elif args.cmd == "test":
            r = run_link_test(args.portA, args.portB, n=args.n)
            if args.json:
                print(json.dumps(r))
            else:
                verdict = "PASS" if r["in_order"] else "FAIL"
                print(f"sent {r['sent']}  received {r['received']}  "
                      f"in-order: {verdict}")
                print(f"stats A tx_retries={r['stats_a']['tx_retries']} "
                      f"nak={r['stats_a']['nak_sent']} "
                      f"ack={r['stats_a']['ack_sent']}")
                print(f"stats B rx_dups={r['stats_b']['rx_dups']} "
                      f"rx_stale={r['stats_b']['rx_stale']} "
                      f"rx_delivered={r['stats_b']['rx_delivered']}")
            return EXIT_OK if r["in_order"] else EXIT_FAIL
        elif args.cmd == "doctor":
            rows = run_doctor(args.config_dir)
            if args.json:
                print(json.dumps(
                    [{"check": n, "ok": ok, "detail": d}
                     for n, ok, d in rows]))
            else:
                for n, ok, d in rows:
                    print(f"[{'OK  ' if ok else 'FAIL'}] {n}: {d}")
            return EXIT_OK if all(ok for _, ok, _ in rows) else EXIT_FAIL
        elif args.cmd == "setup":
            run_setup(assume_yes=args.yes_setup,
                      config_dir=args.config_dir)
        return EXIT_OK
    except PortBusyError as e:
        print(f"BUSY: {e}\n{e.holder_hint}", file=sys.stderr)
        return EXIT_BUSY
    except (LinkTimeout, LinkError, ConfigError, OldFirmwareError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return EXIT_FAIL


def _coerce(val: str):
    """CLI strings -> JSON-ish values: int when it looks like one."""
    try:
        return int(val)
    except ValueError:
        return val


if __name__ == "__main__":
    sys.exit(main())
