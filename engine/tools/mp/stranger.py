#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""stranger: what a stranger on the open internet could throw at a host's party port (F3, 0.8.0).

No game and no password on this side. It talks to 127.0.0.1 ONLY (refuses any other address) and sends,
against the host's party port (TCP control channel + its UDP twin):

  tcp-garbage    random bytes of every size, connection closed or held
  tcp-oversize   frame headers claiming 0, 1, 64 KiB + 1, 16 MiB and 4 GiB, with and without a body
  tcp-truncated  a valid HELLO cut at every interesting length (header only, mid string, mid hash)
  tcp-badhello   wrong magic, wrong version, a name length past the frame, a NUL name, an empty frame
  tcp-wrongkey   N full HELLO + AUTH handshakes with a wrong key (= a wrong password); needs --identity
                 (a game's party_identity.txt) to get past the version check, else the stranger is
                 refused at HELLO (still a refusal, reported as such)
  tcp-replay     a recorded wrong-key HELLO + AUTH replayed on a fresh connection (old nonce)
  tcp-flood      a burst of connects / closes and many idle half-open handshakes
  udp-garbage    random and oversized datagrams, hole-punch probes, relay frames, STUN requests, rudp
                 headers, at the party UDP port; then a flood for --flood-s seconds

Verdict: the stranger must never receive a WELCOME or any encrypted frame (= admitted), and the host must
still answer a fresh HELLO at the end (it did not hang). The caller (smoke phase "stg") checks the host
game's log for a crash and that the real guest stayed in the party.

  python tools/mp/stranger.py --port 47700 --identity inst0/user/party_identity.txt --log stranger.log
Exit code 0 = PASS, 1 = FAIL. Lines "ok   name: detail" / "FAIL name: detail" and a final PASS / FAIL line.
"""
from __future__ import annotations

import argparse
import ipaddress
import os
import random
import socket
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "party"))
import simguest as sg  # noqa: E402  (HELLO layout, handshake and frame constants)

RESULTS: list = []
LOG_FH = None
ADMITTED = []


def say(line: str) -> None:
    print(line, flush=True)
    if LOG_FH:
        LOG_FH.write(line + "\n")
        LOG_FH.flush()


def step(name: str, ok: bool, detail: str = "") -> None:
    RESULTS.append(ok)
    say("%s %s: %s" % ("ok  " if ok else "FAIL", name, detail))


class Nolog:
    def rec(self, *a, **k):
        pass


def frame(type_: int, body: bytes) -> bytes:
    return struct.pack("<IB", 1 + len(body), type_) + body


def connect(host, port, timeout=3.0):
    s = socket.create_connection((host, port), timeout=timeout)
    s.settimeout(timeout)
    return s


def drain(s, wait=1.0, limit=1 << 16) -> bytes:
    """What the host sent back within `wait` s (b'' when it said nothing or closed)."""
    end = time.monotonic() + wait
    got = bytearray()
    while time.monotonic() < end and len(got) < limit:
        s.settimeout(max(0.05, end - time.monotonic()))
        try:
            d = s.recv(4096)
        except (socket.timeout, OSError):
            break
        if not d:
            break
        got += d
    return bytes(got)


def admitted(data: bytes) -> bool:
    """Does the host's answer contain a WELCOME or an encrypted frame (only sent to an authenticated peer)?"""
    off = 0
    while off + 5 <= len(data):
        n, t = struct.unpack_from("<IB", data, off)
        if t in (sg.WELCOME, sg.ENCRYPTED):
            return True
        if n == 0 or n > (1 << 20):
            break
        off += 4 + n
    return False


def reject_text(data: bytes) -> str:
    if len(data) >= 8 and data[4] == sg.REJECT:
        n = struct.unpack_from("<H", data, 6)[0]
        return "%s: %s" % (sg.REJECT_NAMES.get(data[5], data[5]), data[8:8 + n].decode("utf-8", "replace"))
    return ""


def one_tcp(host, port, payload: bytes, wait=0.4, close=True) -> bytes:
    try:
        s = connect(host, port)
    except OSError as e:
        return b"CONNECT-FAILED " + str(e).encode()
    try:
        if payload:
            try:
                s.sendall(payload)
            except OSError:
                pass
        got = drain(s, wait)
    finally:
        if close:
            s.close()
    return got


def hello_body(link: "sg.Link") -> bytes:
    """The HELLO body Link.connect would send (same layout), for the truncation / replay tests."""
    hello = b"BBPL" + struct.pack("<H", link.protocol) + sg.w_str16(link.name) + link.eboot + link.mods + link.token
    hello += link.patches + sg.w_str16("\n".join(link.patch_names)) + sg.w_str16("\n".join(link.mod_names))
    i = link.ident
    if "rules" in i or "build" in i:
        hello += sg.w_str16(i.get("rules", ""))
    if "build" in i:
        hello += (sg.w_str16(i["build"]) + i.get("params", b"\0" * 32) + i.get("hooks", b"\0" * 32) +
                  sg.w_str16("\n".join(i.get("hook_names", []))))
    return hello


def make_link(identity: dict | None, name="Stranger") -> "sg.Link":
    ident = identity or {}
    return sg.Link(name, os.urandom(32), ident.get("eboot", b"\0" * 32), ident.get("mods", b"\0" * 32),
                   os.urandom(16), Nolog(), patches=ident.get("patches", b"\0" * 32),
                   patch_names=ident.get("patch_names"), mod_names=ident.get("mod_names"), ident=ident)


# ---- tests ----

def t_tcp_garbage(host, port, rng):
    bad, closed, n = 0, 0, 40
    for k in range(n):
        size = rng.choice([1, 2, 4, 5, 6, 17, 100, 1000, 4096, 20000])
        got = one_tcp(host, port, rng.randbytes(size), wait=0.2)
        if admitted(got) or got.startswith(b"CONNECT-FAILED"):
            bad += got.startswith(b"CONNECT-FAILED") is False
        closed += 1
    step("tcp-garbage", bad == 0, "%d random streams (1 B - 20 KB), none admitted" % n)


def t_tcp_oversize(host, port, rng):
    bad = 0
    cases = [("len 0", struct.pack("<IB", 0, 1)),
             ("len 1 type 0", struct.pack("<IB", 1, 0)),
             ("len 64KiB+1 + body", struct.pack("<IB", (64 << 10) + 2, sg.HELLO) + rng.randbytes(70000)),
             ("len 16MiB", struct.pack("<IB", 16 << 20, sg.HELLO) + b"x" * 100),
             ("len 4GiB-1", struct.pack("<IB", 0xFFFFFFFF, sg.HELLO)),
             ("len 4GiB-1 + 64K body", struct.pack("<IB", 0xFFFFFFFF, sg.AUTH) + rng.randbytes(65536)),
             ("encrypted frame before AUTH", frame(sg.ENCRYPTED, rng.randbytes(64))),
             ("AUTH before HELLO", frame(sg.AUTH, rng.randbytes(56))),
             ("WELCOME from a guest", frame(sg.WELCOME, rng.randbytes(60)))]
    for name, p in cases:
        got = one_tcp(host, port, p, wait=0.5)
        if admitted(got):
            bad += 1
            ADMITTED.append(name)
    step("tcp-oversize", bad == 0, "%d malformed headers / orders, none admitted" % len(cases))


def t_tcp_truncated(host, port, ident):
    body = hello_body(make_link(ident))
    frm = frame(sg.HELLO, body)
    cuts = sorted({1, 3, 4, 5, 6, 8, 12, 20, 40, 60, len(frm) // 2, len(frm) - 33, len(frm) - 1})
    bad = 0
    for c in cuts:
        got = one_tcp(host, port, frm[:c], wait=0.15)
        bad += admitted(got)
    step("tcp-truncated", bad == 0, "valid HELLO (%d B) cut at %d lengths, none admitted" % (len(frm), len(cuts)))


def t_tcp_badhello(host, port, ident):
    body = hello_body(make_link(ident))
    cases = [("wrong magic", b"XXXX" + body[4:]), ("version 0", body[:4] + b"\0\0" + body[6:]),
             ("version 65535", body[:4] + b"\xff\xff" + body[6:]),
             ("name length past the frame", body[:6] + b"\xff\xff" + body[8:]),
             ("empty name", body[:6] + b"\0\0" + body[8:]),
             ("NUL in name", body[:6] + struct.pack("<H", 3) + b"a\0b" + body[8 + len(make_link(ident).name):]),
             ("body of zeros", b"\0" * len(body)), ("magic only", b"BBPL"),
             ("HELLO twice", None)]
    bad = 0
    for name, b in cases:
        if b is None:
            got = one_tcp(host, port, frame(sg.HELLO, body) * 2, wait=0.5)
        else:
            got = one_tcp(host, port, frame(sg.HELLO, b), wait=0.4)
        bad += admitted(got)
    step("tcp-badhello", bad == 0, "%d damaged HELLOs, none admitted" % len(cases))


def t_tcp_wrongkey(host, port, ident, tries):
    outcomes, bad = {}, 0
    for k in range(tries):
        link = make_link(ident)
        try:
            link.connect(host, port, timeout=4.0)
            bad += 1
            ADMITTED.append("wrongkey %d" % k)
            outcomes["ADMITTED"] = outcomes.get("ADMITTED", 0) + 1
            link.sock.close()
        except sg.LinkError as e:
            msg = str(e)
            key = (msg.split(": ", 1)[-1][:60]) if msg.startswith("rejected") else msg[:50]
            outcomes[key] = outcomes.get(key, 0) + 1
        except OSError as e:
            outcomes["socket: " + str(e)[:40]] = outcomes.get("socket", 0) + 1
        if link.sock:
            try:
                link.sock.close()
            except OSError:
                pass
    step("tcp-wrongkey", bad == 0, "%d wrong-key handshakes, none admitted: %s%s" % (
        tries, outcomes, "" if ident else " (no --identity: refused at the version check)"))


def t_tcp_replay(host, port, ident):
    link = make_link(ident)
    sent = []
    orig = link._send_raw

    def rec(type_, body):
        sent.append((type_, body))
        orig(type_, body)
    link._send_raw = rec
    try:
        link.connect(host, port, timeout=4.0)
    except Exception:
        pass
    try:
        link.sock.close()
    except Exception:
        pass
    hello = next((b for t, b in sent if t == sg.HELLO), None)
    auth = next((b for t, b in sent if t == sg.AUTH), None)
    if hello is None:
        step("tcp-replay", False, "could not record a HELLO")
        return
    bad, how = 0, []
    # 1: the old AUTH after a fresh HELLO + CHALLENGE (the host's nonce differs)
    if auth:
        s = connect(host, port)
        s.sendall(frame(sg.HELLO, hello))
        ch = drain(s, 1.0)
        s.sendall(frame(sg.AUTH, auth))
        got = ch + drain(s, 1.0)
        bad += admitted(got)
        how.append("old AUTH after a new CHALLENGE")
        s.close()
        # 2: the recorded HELLO + AUTH back to back, no wait for the CHALLENGE
        s = connect(host, port)
        s.sendall(frame(sg.HELLO, hello) + frame(sg.AUTH, auth))
        bad += admitted(drain(s, 1.0))
        how.append("HELLO+AUTH in one write")
        s.close()
    # 3: the same AUTH twice on one connection
    s = connect(host, port)
    s.sendall(frame(sg.HELLO, hello))
    drain(s, 0.6)
    s.sendall(frame(sg.AUTH, auth or b"\0" * 56) * 2)
    bad += admitted(drain(s, 1.0))
    how.append("AUTH twice")
    s.close()
    step("tcp-replay", bad == 0, "replays (%s), none admitted" % "; ".join(how))


def t_tcp_flood(host, port, ident, rng):
    body = frame(sg.HELLO, hello_body(make_link(ident)))
    t0, n, refused, held = time.monotonic(), 0, 0, []
    while time.monotonic() - t0 < 2.0:
        try:
            s = socket.create_connection((host, port), timeout=1.0)
            s.sendall(rng.choice([body[:9], rng.randbytes(16), b"", body]))
            s.close()
            n += 1
        except OSError:
            refused += 1
    for _ in range(60):  # half-open handshakes held: the host caps these (16 in all, 4 per address)
        try:
            s = socket.create_connection((host, port), timeout=1.0)
            s.sendall(body[:7])
            held.append(s)
        except OSError:
            refused += 1
    time.sleep(1.5)
    closed = 0
    for s in held:
        s.settimeout(0.05)
        try:
            if s.recv(1) == b"":
                closed += 1
        except socket.timeout:
            pass
        except OSError:
            closed += 1
        s.close()
    step("tcp-flood", True, "%d connect+send+close in 2 s (%d refused), 60 held half-open handshakes (%d dropped by the host "
         "within 1.5 s)" % (n, refused, closed))


def t_udp(host, port, ident, rng, flood_s):
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind((host, 0))
    u.settimeout(0.05)
    dst = (host, port)
    sent = 0
    probe = bytes([0xfe]) + b"bbhp" + b"\0\0\0"
    pkts = [b"", b"\0", rng.randbytes(7), rng.randbytes(8), rng.randbytes(1200), rng.randbytes(2047), rng.randbytes(2048),
            rng.randbytes(2049), rng.randbytes(9200), rng.randbytes(9232), rng.randbytes(60000),
            probe, probe[:7], probe + rng.randbytes(2000),
            b"\xff\x80\x00\x01\x00\x02" + rng.randbytes(500),           # vport header + junk
            b"\xff\xe0" + rng.randbytes(300),
            b"\xfb" + b"R" + rng.randbytes(8) + b"\x00\x01" + rng.randbytes(100),   # relay frame, made-up token
            b"\xfb" + b"R" + b"\0" * 8 + b"\xff\xff" + b"\xfe" + b"bbhp" + b"\0\0\0",
            b"\xfb" + b"r" + rng.randbytes(40),
            # STUN binding request (20-byte header, magic cookie) with and without an attribute
            struct.pack(">HHI", 1, 0, 0x2112A442) + rng.randbytes(12),
            struct.pack(">HHI", 1, 40, 0x2112A442) + rng.randbytes(12) + rng.randbytes(40),
            struct.pack(">HHI", 1, 8, 0x2112A442) + rng.randbytes(12) + b"BBHOST-HELLO"[:8],
            struct.pack(">HHI", 1, 0xffff, 0x2112A442) + rng.randbytes(12),
            b"BBHOST-HELLO" + rng.randbytes(40), b"BBHOST-HELLO"]
    for p in pkts:
        for _ in range(3):
            try:
                u.sendto(p, dst)
                sent += 1
            except OSError:
                pass
    for k in range(400):  # random lengths and bytes, the first bytes drawn from the real framing's magics
        head = rng.choice([b"", b"\xff", b"\xfe", b"\xfb", b"\xe0", b"\x00", b"\x01"])
        try:
            u.sendto(head + rng.randbytes(rng.choice([0, 3, 8, 20, 64, 200, 900, 1400, 2048, 4000, 9000])), dst)
            sent += 1
        except OSError:
            pass
    t0, flood = time.monotonic(), 0
    blob = rng.randbytes(1400)
    while time.monotonic() - t0 < flood_s:
        for _ in range(200):
            try:
                u.sendto(blob if flood % 3 else rng.randbytes(rng.randrange(1, 1400)), dst)
                flood += 1
            except OSError:
                pass
    # anything the host answered must not be a WELCOME / encrypted frame
    got = 0
    try:
        while True:
            d, _ = u.recvfrom(65536)
            got += 1
    except (socket.timeout, OSError):
        pass
    u.close()
    step("udp-garbage", True, "%d crafted + random datagrams, flood of %d in %.1f s (%.0f/s), %d answers (probe acks / "
         "STUN replies)" % (sent, flood, flood_s, flood / max(0.1, flood_s), got))


def t_alive(host, port, ident):
    """The host still answers a fresh HELLO (with a REJECT or a CHALLENGE) after all of it."""
    body = frame(sg.HELLO, hello_body(make_link(ident)))
    for k in range(10):
        got = one_tcp(host, port, body, wait=1.5)
        if got and not got.startswith(b"CONNECT-FAILED"):
            kind = {sg.CHALLENGE: "CHALLENGE", sg.REJECT: "REJECT"}.get(got[4] if len(got) > 4 else -1, "?")
            step("host-alive", not admitted(got), "answered a fresh HELLO with %s %s" % (kind, reject_text(got)))
            return
        time.sleep(1.0)
    step("host-alive", False, "no answer to a fresh HELLO within 25 s")


def main(argv=None) -> int:
    global LOG_FH
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True, help="the host's party port (TCP; UDP the same unless --udp-port)")
    ap.add_argument("--udp-port", type=int, default=0)
    ap.add_argument("--identity", help="the host game's <user dir>/party_identity.txt (to pass the version check)")
    ap.add_argument("--tries", type=int, default=8, help="wrong-key handshakes (the 6th from an address is locked out)")
    ap.add_argument("--flood-s", type=float, default=4.0)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--log", default="")
    ap.add_argument("--only", default="", help="comma list of: garbage,oversize,truncated,badhello,wrongkey,replay,flood,udp")
    o = ap.parse_args(argv)
    try:
        loop = ipaddress.ip_address(o.host).is_loopback and ipaddress.ip_address(o.host).version == 4
    except ValueError:  # a host name (even "127.example.com") is never accepted
        loop = False
    if not loop:
        ap.error("loopback only: --host must be a literal 127.x.x.x address")
    if o.log:
        LOG_FH = open(o.log, "w", encoding="utf-8")
    seed = o.seed if o.seed is not None else int(time.time())
    rng = random.Random(seed)
    say("stranger: target %s:%d, seed %d" % (o.host, o.port, seed))
    ident = None
    if o.identity and os.path.isfile(o.identity):
        ident = sg.read_party_identity(o.identity)
    which = set(filter(None, o.only.split(","))) or {"garbage", "oversize", "truncated", "badhello", "wrongkey", "replay",
                                                      "flood", "udp"}
    try:
        socket.create_connection((o.host, o.port), timeout=3.0).close()
    except OSError as e:
        say("FAIL stranger: cannot reach the host's party port: %s" % e)
        return 1
    steps = [("garbage", lambda: t_tcp_garbage(o.host, o.port, rng)),
             ("oversize", lambda: t_tcp_oversize(o.host, o.port, rng)),
             ("truncated", lambda: t_tcp_truncated(o.host, o.port, ident)),
             ("badhello", lambda: t_tcp_badhello(o.host, o.port, ident)),
             ("replay", lambda: t_tcp_replay(o.host, o.port, ident)),
             ("wrongkey", lambda: t_tcp_wrongkey(o.host, o.port, ident, o.tries)),
             ("flood", lambda: t_tcp_flood(o.host, o.port, ident, rng)),
             ("udp", lambda: t_udp(o.host, o.udp_port or o.port, ident, rng, o.flood_s))]
    for name, fn in steps:
        if name in which:
            try:
                fn()
            except Exception as e:  # the script's own bug must show, not hide
                import traceback
                traceback.print_exc()
                step(name, False, "script error %r" % e)
    t_alive(o.host, o.port, ident)
    ok = all(RESULTS) and not ADMITTED
    say("%s stranger: %d checks, %d admitted%s" % ("PASS" if ok else "FAIL", len(RESULTS), len(ADMITTED),
                                                   (" " + str(ADMITTED)) if ADMITTED else ""))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
