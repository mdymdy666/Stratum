#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Stratum cross-host UDP relay (v3: one bound RX socket + one unbound TX socket).

File    : tools/udp_relay.py
Purpose : Bridge WSL and a VMware guest that cannot reach each other directly.

Evolution (both earlier versions failed, for different reasons)
--------------------------------------------------------------
v1: one socket per direction, each bound to a different host address.
    Worked for guest->WSL, but was one-way: after the relay rewrote the
    source address, WSL learned the guest's locator as the relay host, and
    its SEDP went into the wrong socket.  Symptom: spdp=200 peers=1 sedp_pub=0.

v2: one socket per port, direction decided by packet source (correct idea),
    BUT it also reused that *bound* socket for sending.  Windows then refused
    to route from 192.168.100.1 (VMnet8) into the WSL subnet:
        send to wsl failed: [WinError 10051] network unreachable

v3 (this file): keep v2's single-RX-socket-per-port with source-based direction,
    and send through a SEPARATE, UNBOUND socket so the OS picks a valid
    source address for each destination.  This is what made v1 work by luck.

Topology (measured):
    WSL  -> guest : direct, no relay needed
    guest -> WSL  : not reachable; both sides send to the relay address
                    (192.168.100.1 = host's VMnet8 interface, same subnet as
                    the guest and routable from WSL).

Usage (Windows, plain user, no admin needed):

    python -u tools/udp_relay.py --ports 14900,9360,9361 ^
        --wsl 172.17.211.143 --guest 192.168.100.129 --bind 192.168.100.1
"""
import argparse
import socket
import sys
import threading
import time

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

stop_flag = threading.Event()


def relay_port(bind_ip, port, wsl_ip, guest_ip):
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        rx.bind((bind_ip, port))
    except OSError as exc:
        print("[port %d] BIND FAILED %s:%d -> %s" % (port, bind_ip, port, exc))
        return
    rx.settimeout(0.5)

    # 【关键】发送用**独立且不绑定**的 socket：让系统按目标选择合法源地址。
    # 若复用上面这个绑定了 192.168.100.1 的 socket，Windows 会拒绝把它路由到
    # WSL 网段（WinError 10051 network unreachable）—— v2 就是栽在这里。
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    print("[port %d] rx on %s:%d -> guest=%s / wsl=%s"
          % (port, bind_ip, port, guest_ip, wsl_ip))

    n_to_wsl = 0
    n_to_guest = 0
    last = time.time()
    while not stop_flag.is_set():
        try:
            data, (src_ip, _src_port) = rx.recvfrom(65535)
        except socket.timeout:
            if time.time() - last >= 60:
                print("[port %d] to_wsl=%d to_guest=%d" % (port, n_to_wsl, n_to_guest))
                last = time.time()
            continue
        except OSError:
            break

        if src_ip == guest_ip:
            try:
                tx.sendto(data, (wsl_ip, port))
                n_to_wsl += 1
                if n_to_wsl <= 3:
                    print("[port %d] guest->wsl (%d bytes)" % (port, len(data)))
            except OSError as exc:
                print("[port %d] send to wsl failed: %s" % (port, exc))
        else:
            try:
                tx.sendto(data, (guest_ip, port))
                n_to_guest += 1
                if n_to_guest <= 3:
                    print("[port %d] ->guest (%d bytes, src %s)"
                          % (port, len(data), src_ip))
            except OSError as exc:
                print("[port %d] send to guest failed: %s" % (port, exc))

    rx.close()
    tx.close()
    print("[port %d] stopped: to_wsl=%d to_guest=%d" % (port, n_to_wsl, n_to_guest))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ports", default="14900,9360,9361",
                    help="UDP ports to relay (SPDP + metatraffic + user)")
    ap.add_argument("--wsl", default="172.17.211.143")
    ap.add_argument("--guest", default="192.168.100.129")
    ap.add_argument("--bind", default="192.168.100.1",
                    help="host address reachable by BOTH sides (RX)")
    args = ap.parse_args()

    ports = [int(p) for p in args.ports.split(",") if p.strip()]
    print("UDP relay v3 (RX bound to %s, TX unbound so OS picks a valid source)"
          % args.bind)
    print("  wsl   : %s" % args.wsl)
    print("  guest : %s" % args.guest)
    print("  ports : %s" % ports)
    print()

    threads = []
    for p in ports:
        th = threading.Thread(target=relay_port,
                              args=(args.bind, p, args.wsl, args.guest),
                              daemon=True)
        th.start()
        threads.append(th)

    print("running; Ctrl+C to stop")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nstopping...")
        stop_flag.set()
        for th in threads:
            th.join(timeout=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
