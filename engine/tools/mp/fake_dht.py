#!/usr/bin/env python3
"""A one-node stand-in for the BitTorrent DHT on 127.0.0.1, for live NAT-simulator runs of the
party rendezvous (gpu/shim/party/party_rdv.h) without touching the internet.

  python tools/mp/fake_dht.py [--port 46881] [--seconds 600]

Then start both game instances with BB_PARTY_RENDEZVOUS=1 (opt-in) and
BB_PARTY_DHT_BOOTSTRAP=127.0.0.1:46881 (and BB_NET_SIM with
nat=...). It answers get_peers (token, no further nodes, stored values) and announce_peer (stores
the sender's address with the announced port), as a BEP 5 node would. Loopback only.
"""
import argparse
import socket
import time


def bdecode(data, i=0):
    c = data[i:i + 1]
    if c == b"i":
        e = data.index(b"e", i)
        return int(data[i + 1:e]), e + 1
    if c == b"l":
        i += 1
        out = []
        while data[i:i + 1] != b"e":
            v, i = bdecode(data, i)
            out.append(v)
        return out, i + 1
    if c == b"d":
        i += 1
        out = {}
        while data[i:i + 1] != b"e":
            k, i = bdecode(data, i)
            v, i = bdecode(data, i)
            out[k] = v
        return out, i + 1
    colon = data.index(b":", i)
    n = int(data[i:colon])
    return data[colon + 1:colon + 1 + n], colon + 1 + n


def bencode(v):
    if isinstance(v, int):
        return b"i%de" % v
    if isinstance(v, bytes):
        return b"%d:%s" % (len(v), v)
    if isinstance(v, list):
        return b"l" + b"".join(bencode(x) for x in v) + b"e"
    if isinstance(v, dict):
        return b"d" + b"".join(bencode(k) + bencode(v[k]) for k in sorted(v)) + b"e"
    raise TypeError(type(v))


class Node:
    def __init__(self, node_id=b"F" * 20):
        self.id = node_id
        self.store = {}  # info_hash -> [compact peer]

    def token(self, addr):
        return b"tk" + socket.inet_aton(addr[0])

    def handle(self, data, addr):
        """The reply bytes for a datagram from addr (ip, port), or None."""
        try:
            msg, end = bdecode(data)
        except (ValueError, IndexError):
            return None
        if end != len(data) or not isinstance(msg, dict) or msg.get(b"y") != b"q":
            return None
        t, q, a = msg.get(b"t", b""), msg.get(b"q"), msg.get(b"a", {})
        r = {b"id": self.id}
        if q == b"get_peers":
            ih = a.get(b"info_hash", b"")
            r[b"token"] = self.token(addr)
            r[b"nodes"] = b""
            if ih in self.store:
                r[b"values"] = list(self.store[ih])
        elif q == b"announce_peer":
            if a.get(b"token") != self.token(addr):
                return bencode({b"t": t, b"y": b"e", b"e": [203, b"bad token"]})
            peer = socket.inet_aton(addr[0]) + int(a[b"port"]).to_bytes(2, "big")
            peers = self.store.setdefault(a[b"info_hash"], [])
            if peer not in peers:
                peers.append(peer)
        return bencode({b"t": t, b"y": b"r", b"r": r})


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=46881)
    ap.add_argument("--seconds", type=float, default=600)
    a = ap.parse_args()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", a.port))
    s.settimeout(0.5)
    node = Node()
    end = time.time() + a.seconds
    print(f"fake DHT node on 127.0.0.1:{a.port}", flush=True)
    while time.time() < end:
        try:
            data, addr = s.recvfrom(2048)
        except socket.timeout:
            continue
        except ConnectionResetError:
            continue
        out = node.handle(data, addr)
        if out:
            s.sendto(out, addr)
            q = bdecode(data)[0].get(b"q", b"?").decode(errors="replace")
            print(f"{time.strftime('%H:%M:%S')} {q} from {addr[0]}:{addr[1]}", flush=True)


if __name__ == "__main__":
    main()
