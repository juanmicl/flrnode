#!/usr/bin/env python3
"""flrpong v2: minimal non-interactive rnspong replacement for the FLRNode
M2 gate-3 bench. Uses the PROVE_ALL pattern from Reticulum's own Echo
example (the installed API): each instance announces a SINGLE/IN
destination with PROVE_ALL, discovers the peer over the radio, sends a
ping, and treats the returned proof as the pong (round-trip evidence).
Usage: flrpong.py --config <rns-config-dir> [--name LABEL] [--duration S]
"""
import argparse
import sys
import time
import RNS

APP_NAME = "flrpong"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", required=True)
    ap.add_argument("--name", default=None)
    ap.add_argument("--duration", type=float, default=55.0)
    args = ap.parse_args()
    name = args.name or args.config.rstrip("/").split("/")[-1]

    reticulum = RNS.Reticulum(args.config)
    identity = RNS.Identity()
    dest = RNS.Destination(identity, RNS.Destination.IN,
                           RNS.Destination.SINGLE, APP_NAME, "bench")
    dest.set_proof_strategy(RNS.Destination.PROVE_ALL)
    me = dest.hash
    pinged = set()
    sent_receipts = []

    def log(msg):
        print(f"[{time.strftime('%H:%M:%S')}] [{name}] {msg}", flush=True)

    # --- IN side: receive pings, proof goes back automatically ---
    def packet_received(message, packet):
        text = message.decode("utf-8", errors="replace")
        rssi = getattr(packet, "rssi", None)
        log(f"RECV '{text}' (RSSI {rssi} dBm) — proof sent automatically")

    dest.set_packet_callback(packet_received)
    log(f"running, destination {RNS.prettyhexrep(me)}")

    # --- OUT side: on peer announce, ping and wait for the proof ---
    def announce_handler(destination_hash, announced_identity, app_data):
        if destination_hash == me:
            return
        if destination_hash in pinged:
            return
        pinged.add(destination_hash)
        peer = RNS.prettyhexrep(destination_hash)
        log(f"ANNOUNCE seen from {peer} — sending ping")

        out_dest = RNS.Destination(announced_identity,
                                   RNS.Destination.OUT,
                                   RNS.Destination.SINGLE,
                                   APP_NAME, "bench")
        packet = RNS.Packet(out_dest, f"ping from {name}".encode())
        packet.send()
        sent_receipts.append((packet.receipt, peer, time.time()))
        log(f"PING sent -> {peer}, waiting for proof")

    class BenchAnnounceHandler:
        def __init__(self, cb):
            self.aspect_filter = None  # fire on every announce
            self.cb = cb
        def received_announce(self, destination_hash, announced_identity, app_data):
            self.cb(destination_hash, announced_identity, app_data)

    RNS.Transport.register_announce_handler(BenchAnnounceHandler(announce_handler))

    # --- main loop: announce + check proof receipts ---
    deadline = time.time() + args.duration
    n = 0
    reported = set()
    while time.time() < deadline:
        dest.announce()
        n += 1
        if n % 6 == 1:
            log(f"announced ({n})")
        for receipt, peer, t0 in sent_receipts:
            status = getattr(receipt, "status", None)
            delivered = getattr(RNS.PacketReceipt, "DELIVERED", 3)
            if peer not in reported and status == delivered:
                reported.add(peer)
                log(f"PONG (proof) received from {peer} "
                    f"after {time.time()-t0:.2f}s — ROUND TRIP COMPLETE")
        time.sleep(5)

    log(f"duration elapsed: {len(reported)} peer(s) round-tripped, exiting")
    sys.exit(0 if reported else 1)  # zero peers = the bench failed


if __name__ == "__main__":
    main()
