#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Stratum cross-host UDP relay (runs on Windows, no admin required).

File    : tools/udp_relay.py
Purpose : Let WSL (behind its NAT) and a VMware guest (behind VMware NAT)
          exchange RTPS traffic, when neither can reach the other directly
          and the host itself is on Wi-Fi (so VMware bridging is unusable).

Why this design
---------------
Both sides can reach the Windows host, so the host relays between them:

    WSL  --(dest = 192.168.100.2)--> relay --(to 192.168.100.129)--> guest
    guest --(dest = 172.17.208.1)--> relay --(to 172.17.211.143)--> WSL

Those two destinations are Windows interface addresses:
  * 192.168.100.2  = VMware NAT gateway (VMnet8)
  * 172.17.208.1   = WSL NAT gateway (vEthernet (WSL))
They belong to Windows, so binding them does NOT conflict with the sockets
that WSL or the guest already hold on their own addresses.

Consequently each Stratum side must advertise the relay-facing address:
  * WSL side   : advertised_address = 172.17.208.1
  * guest side : advertised_address = 192.168.100.2

Usage (Windows, plain user):
    python tools/udp_relay.py --ports 9060,9061 \
        --wsl 172.17.211.143 --guest 192.168.100.129

Note: source address is rewritten by the relay (it is the host's address).
RTPS locators travel inside the SPDP/SEDP payloads we advertise, so peer
discovery still uses the advertised addresses rather than the packet source.
"""
import argparse
import socket
import sys
import threading
import time

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

WSL_GW = "172.17.208.1"      # Windows side of the WSL NAT
VMWARE_GW = "192.168.100.2"  # Windows side of the VMware NAT

stop_flag = threading.Event()


def relay(bind_ip, bind_port, dst_ip, dst_port, label):
    """Receive on (bind_ip, bind_port) and forward to (dst_ip, dst_port)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind((bind_ip, bind_port))
    except OSError as exc:
        print("[%s] BIND FAILED %s:%d -> %s" % (label, bind_ip, bind_port, exc))
        return
    s.settimeout(0.5)
    print("[%s] listening on %s:%d  ->  forwarding to %s:%d"
          % (label, bind_ip, bind_port, dst_ip, dst_port))

    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    n = 0
    bytes_fwd = 0
    last_report = time.time()
    while not stop_flag.is_set():
        try:
            data, addr = s.recvfrom(65535)
        except socket.timeout:
            if time.time() - last_report >= 30:
                print("[%s] forwarded %d packets / %d bytes" % (label, n, bytes_fwd))
                last_report = time.time()
            continue
        except OSError:
            break
        try:
            tx.sendto(data, (dst_ip, dst_port))
            n += 1
            bytes_fwd += len(data)
        except OSError as exc:
            print("[%s] send failed: %s" % (label, exc))
    s.close()
    tx.close()
    print("[%s] stopped after %d packets" % (label, n))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ports", default="9060,9061",
                    help="comma separated UDP ports to relay")
    ap.add_argument("--wsl", default="172.17.211.143", help="WSL address")
    ap.add_argument("--guest", default="192.168.100.129", help="guest address")
    ap.add_argument("--wsl-gw", default=WSL_GW)
    ap.add_argument("--guest-gw", default=VMWARE_GW)
    ap.add_argument("--one-way", action="store_true",
                    help="only relay guest -> WSL (bind guest-gw). "
                         "Use when WSL can reach the guest directly, "
                         "which is the case in this project.")
    args = ap.parse_args()

    ports = [int(p) for p in args.ports.split(",") if p.strip()]

    print("UDP relay starting")
    print("  WSL   : %s  (relay forwards to it)" % args.wsl)
    print("  guest : %s  (its packets arrive at %s)" % (args.guest, args.wsl_gw))
    print("  ports : %s" % ports)
    print()

    threads = []
    for port in ports:
        if not args.one_way:
            # WSL -> guest : WSL sends to the address the guest advertises
            t1 = threading.Thread(target=relay,
                                  args=(args.guest_gw, port, args.guest, port,
                                        "wsl->guest:%d" % port), daemon=True)
            t1.start()
            threads.append(t1)
        # guest -> WSL : the guest sends to args.wsl_gw, which must be an
        # address of THIS host that lies in the guest's subnet
        # (192.168.100.1 = VMnet8 host interface).
        t2 = threading.Thread(target=relay,
                              args=(args.wsl_gw, port, args.wsl, port,
                                    "guest->wsl:%d" % port), daemon=True)
        t2.start()
        threads.append(t2)

    print("running; press Ctrl+C to stop")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nstopping...")
        stop_flag.set()
        for t in threads:
            t.join(timeout=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
