"""FakeBridge — simulates the firmware side of the flrnode serial
protocol over a pty pair, for headless integration tests.

Mirrors the reviewed firmware exactly:
- boot line emitted RAW (unframed, Serial.println style) on every
  slave-open transition (host open() -> boot line, like a real D1 mini
  reset); short_boot=True emits {"t":"boot"} only (defaults path).
- CTRL classifier: printable ASCII 0x20-0x7E, 2..95 B, '{"' prefix ->
  CTRL; everything else framed -> DATA.
- CTRL replies per §8: get -> {"<key>":<value>}; set -> {"ok":true,
  "effective":<n>} on numeric / bare {"ok":true} on pmk; errors
  unknown_key / unsupported_transport / write_only / bad_value /
  unknown_cmd (fw != m3 answers unknown_cmd to every get/set).
- stats reply: the 14-key schema in canonical order (stats_omit drops
  keys to test the "-" fill).
- DATA frames (flag/escape/flag, NO FCS) are forwarded to the paired
  bridge (FakeBridgePair wires them) — drop_after stops forwarding
  after k payloads, scramble forwards in swapped pairs.
"""

import json
import os
import pty
import select
import threading
import time

FLAG = 0x7E
ESC = 0x7D
ESC_MASK_PAIRS = {0x5E: 0x7E, 0x5D: 0x7D}

STATS_KEYS = (
    "tx_frames", "tx_frags", "tx_retries", "tx_dropped", "tx_too_big",
    "tx_window_overflow", "meta_conflict", "rx_frags", "rx_delivered",
    "rx_dups", "rx_stale", "rx_window_overflow", "nak_sent", "ack_sent",
)

DEFAULT_MAC = "AABBCCDDEEFF"
DEFAULT_PEER = "112233445566"


def _escape(payload: bytes) -> bytes:
    out = bytearray()
    for b in payload:
        if b == FLAG:
            out += b"\x7D\x5E"
        elif b == ESC:
            out += b"\x7D\x5D"
        else:
            out.append(b)
    return bytes(out)


class FakeBridge:
    """One simulated bridge on a pty master fd. Test-only."""

    def __init__(self, master_fd: int, fw: str = "m3", **opts):
        self.master = master_fd
        self.fw = fw
        self.opts = opts
        self.channel = opts.get("channel", 1)
        self.tx_power = opts.get("tx_power", 17)
        self.stats_period = opts.get("stats_period", 5)
        self.node_id = opts.get("node_id", DEFAULT_MAC)
        self.on_data = None            # wired by FakeBridgePair
        self.connected = False
        self.resets_seen = 0
        self._running = False
        self._thread = None
        self._acc = bytearray()
        self._in_frame = False
        self._esc = False
        self._forwarded = 0

    # -- lifecycle --------------------------------------------------------
    def start(self):
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._running = False
        if self._thread:
            self._thread.join(timeout=2)

    # -- wire helpers -----------------------------------------------------
    def _send_framed(self, payload: bytes):
        try:
            os.write(self.master, b"\x7E" + _escape(payload) + b"\x7E")
        except OSError:
            pass  # slave closed mid-reply; the next open re-syncs

    def _emit_boot(self):
        self.resets_seen += 1
        if self.opts.get("silent"):
            return                     # never answers: probe must time out
        if self.opts.get("short_boot"):
            line = b'{"t":"boot"}'
        elif self.fw == "m1":
            line = (b'{"t":"boot","fw":"m1","channel":'
                    + str(self.channel).encode()
                    + b',"mac":"' + DEFAULT_MAC.encode() + b'"}')
        else:
            # disagree_channel: a mis-provisioned bridge whose boot line
            # reports a DIFFERENT channel than the applied one
            ch = self.opts.get("disagree_channel", self.channel)
            # boot_channel: raw JSON value for "channel", inserted
            # verbatim — lets tests inject a malformed (non-integer)
            # channel like b'"six"'.
            ch_raw = self.opts.get("boot_channel") or str(ch).encode()
            line = (b'{"t":"boot","fw":"' + self.fw.encode()
                    + b'","mode":"bridge","channel":'
                    + ch_raw
                    + b',"mac":"' + DEFAULT_MAC.encode()
                    + b'","peer":"' + DEFAULT_PEER.encode()
                    + b'","warn":"dev keys"}')
        try:
            os.write(self.master, line + b"\r\n")
        except OSError:
            pass

    # -- main loop --------------------------------------------------------
    def _run(self):
        # A pty master never becomes readable just because the slave
        # opened (no DTR reset to piggyback on), so the bridge emits its
        # boot line every 400 ms while the host is quiet — exactly like
        # a real bridge would after a reset. Raw boot lines outside
        # frames are ignored by the host's flag-discipline deframer, and
        # emission stops once host data flows (streams stay clean).
        last_boot = 0.0
        last_host = time.monotonic()
        while self._running:
            r, _, _ = select.select([self.master], [], [], 0.05)
            now = time.monotonic()
            if r:
                try:
                    data = os.read(self.master, 4096)
                except OSError:
                    pass  # slave closed; keep the periodic boot going
                else:
                    if data:
                        last_host = now
                        self._feed(data)
            if now - last_host > 0.4 and now - last_boot > 0.4:
                self._emit_boot()
                last_boot = now

    # -- protocol ---------------------------------------------------------
    def _feed(self, data: bytes):
        for b in data:
            if b == FLAG:
                if self._in_frame and self._acc:
                    self._handle(bytes(self._acc))
                    self._in_frame = False   # closed a full frame
                elif not self._in_frame:
                    self._in_frame = True    # opened
                # else: orphan flag while open with empty acc — re-open
                self._acc = bytearray()
                self._esc = False
            elif self._in_frame:
                if b == ESC:
                    self._esc = True
                else:
                    if self._esc:
                        self._acc.append(ESC_MASK_PAIRS.get(b, b))
                        self._esc = False
                    else:
                        self._acc.append(b)
        # leftover partial frames stay in _acc

    def _handle(self, payload: bytes):
        # CTRL classifier (§8/M2): printable, small, {"-prefixed; ONE
        # trailing \n is exempt from the printable scan (§8 requests may
        # arrive \n-terminated; handleCtrl_ trims it) — \n\n is NOT
        # exempt and falls through to DATA.
        plen = len(payload)
        if plen and payload[plen - 1:plen] == b"\n":
            plen -= 1
        body = payload[:plen]
        if (2 <= len(payload) < 96 and body[0:1] == b"{"
                and body[1:2] == b'"'
                and all(0x20 <= c <= 0x7E for c in body)):
            self._ctrl(payload)
            return
        self._forward_data(payload)

    def _forward_data(self, payload: bytes):
        drop_after = self.opts.get("drop_after")
        if drop_after is not None and self._forwarded >= drop_after:
            return
        if self.opts.get("scramble") and self._forwarded % 2 == 1:
            payload = bytes(reversed(payload))
        self._forwarded += 1
        if self.on_data:
            self.on_data(payload)

    def _forward_from_air(self, payload: bytes):
        """A DATA payload that arrived over the air on the PEER bridge:
        deliver it to this bridge's host, framed."""
        self._send_framed(payload)

    # -- CTRL -------------------------------------------------------------
    def _ctrl(self, payload: bytes):
        if self.opts.get("silent"):
            return  # fully silent: no boot line AND no CTRL replies
        if self.opts.get("emit_event"):
            # unsolicited §8 event ahead of the reply — pins the host's
            # event-skip discipline in BridgeLink.ctrl
            self._send_framed(b'{"event":"air_rx","len":16}')
        try:
            req = json.loads(payload.decode())
        except ValueError:
            self._send_framed(b'{"err":"unknown_cmd"}')
            return
        if self.fw != "m3":
            self._send_framed(b'{"err":"unknown_cmd"}')
            return
        cmd = req.get("cmd")
        if cmd == "stats":
            stats = {k: 0 for k in STATS_KEYS
                     if k not in self.opts.get("stats_omit", ())}
            line = json.dumps({"stats": stats}, separators=(",", ":"))
            self._send_framed(line.encode())
            return
        key = req.get("key")
        if cmd == "get":
            self._get(key)
        elif cmd == "set":
            self._set(key, req.get("val"))
        else:
            self._send_framed(b'{"err":"unknown_cmd"}')

    def _get(self, key):
        replies = {
            "channel": {"channel": self.channel},
            "tx_power": {"tx_power": self.tx_power},
            "stats_period": {"stats_period": self.stats_period},
            "node_id": {"node_id": self.node_id},
            "rate": {"err": "unsupported_transport"},
            "pmk": {"err": "write_only"},
        }
        reply = replies.get(key, {"err": "unknown_key"})
        self._send_framed(json.dumps(reply, separators=(",", ":")).encode())

    @staticmethod
    def _parse_num(val):
        """Mirror the firmware's parseNum: a JSON int, or a string whose
        digits strtol would FULLY consume ("6" ok; "abc"/"6x" -> None =
        bad_value). Booleans are not numbers here."""
        if isinstance(val, bool):
            return None
        if isinstance(val, int):
            return val
        if isinstance(val, str):
            s = val.strip()
            digits = s[1:] if s[:1] in "+-" else s
            if digits.isdigit():
                return int(s)
        return None

    def _set(self, key, val):
        if key == "channel":
            n = self._parse_num(val)
            if n is None or not 1 <= n <= 13:
                reply = {"err": "bad_value"}
            else:
                self.channel = n
                reply = {"ok": True, "effective": n}
        elif key == "tx_power":
            n = self._parse_num(val)
            if n is None:
                reply = {"err": "bad_value"}
            else:
                v = max(0, min(17, n))
                self.tx_power = v
                reply = {"ok": True, "effective": v}
        elif key == "stats_period":
            n = self._parse_num(val)
            if n is None:
                reply = {"err": "bad_value"}
            else:
                if n <= 0 or n > 3600:
                    n = 5
                self.stats_period = n
                reply = {"ok": True, "effective": n}
        elif key == "pmk":
            hexdigits = set("0123456789abcdefABCDEF")
            if (isinstance(val, str) and len(val) == 32
                    and all(c in hexdigits for c in val)):
                reply = {"ok": True}
            else:
                reply = {"err": "bad_value"}
        elif key == "rate":
            reply = {"err": "unsupported_transport"}
        else:
            reply = {"err": "unknown_key"}
        self._send_framed(json.dumps(reply, separators=(",", ":")).encode())


class FakeBridgePair:
    """Two wired FakeBridges (DATA forwarded between them) + their
    pty slave paths (what flrn opens)."""

    def __init__(self, fw: str = "m3", **opts):
        self.fw = fw
        self.opts = opts
        self.bridges: list[FakeBridge] = []
        self.slave_paths: list[str] = []
        self._masters: list[int] = []
        self._slaves: list[int] = []

    def start(self) -> list[str]:
        shared = {k: v for k, v in self.opts.items()
                  if k != "disagree_channel"}
        for i in range(2):
            m, s = pty.openpty()
            self._masters.append(m)
            self._slaves.append(s)
            self.slave_paths.append(os.ttyname(s))
            opts = dict(shared)
            if i == 1 and "disagree_channel" in self.opts:
                # per-bridge mis-provisioning: only side B reports it
                opts["disagree_channel"] = self.opts["disagree_channel"]
            self.bridges.append(FakeBridge(m, fw=self.fw, **opts))
        self.bridges[0].on_data = lambda d: self.bridges[1]._forward_from_air(d)
        self.bridges[1].on_data = lambda d: self.bridges[0]._forward_from_air(d)
        for b in self.bridges:
            b.start()
        return self.slave_paths

    def stop(self):
        for b in self.bridges:
            b.stop()
        for fd in self._masters + self._slaves:
            try:
                os.close(fd)
            except OSError:
                pass
