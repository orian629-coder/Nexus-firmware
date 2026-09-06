#!/usr/bin/env python3
"""Minimal, dependency-free TCP forwarder: port 80 -> 127.0.0.1:8080.

The setup web UI (and the captive-portal page) live on the speaker's HTTP server at :8080, but a
phone that opens http://10.42.0.1 (or a captive-portal probe) hits port 80 — where nothing listens
unless the nftables redirect fires. That redirect proved fragile across NetworkManager versions, so
this forwarder guarantees port 80 always reaches the UI. It's started/stopped with the hotspot
(scripts/hotspot.sh). Runs as root (needed to bind the privileged port 80).
"""
import socket
import sys
import threading

LISTEN_PORT = 80
TARGET_HOST = "127.0.0.1"
TARGET_PORT = 8080


def pipe(src, dst):
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            dst.sendall(data)
    except OSError:
        pass
    finally:
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def handle(client):
    try:
        upstream = socket.create_connection((TARGET_HOST, TARGET_PORT), timeout=5)
    except OSError:
        client.close()
        return
    threading.Thread(target=pipe, args=(client, upstream), daemon=True).start()
    threading.Thread(target=pipe, args=(upstream, client), daemon=True).start()


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind(("0.0.0.0", LISTEN_PORT))
    except OSError as e:
        print(f"[port80] cannot bind :{LISTEN_PORT}: {e}", file=sys.stderr)
        sys.exit(1)
    srv.listen(64)
    print(f"[port80] forwarding :{LISTEN_PORT} -> {TARGET_HOST}:{TARGET_PORT}", flush=True)
    while True:
        try:
            client, _ = srv.accept()
        except OSError:
            break
        handle(client)


if __name__ == "__main__":
    main()
