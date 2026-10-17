#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# derived from droogie/bbhost tools/nettest/simclient.py @8f2746c
"""simguest: a headless simulated party guest for the serverless party co-op.

It joins a party host (the real game, or tests/party_host_harness.cpp) the way a guest's game
does, with no second game instance:

  PartyLink    TCP control channel: HELLO / CHALLENGE / AUTH (keyed BLAKE2b proof) / WELCOME,
               then XChaCha20-Poly1305 frames (key = Argon2i(password || code secret)); PING
               1 Hz both ways, ROSTER (our state), RPC_REQ/RESP, EVENT/ACK, BYE
               (gpu/shim/party/party_link.h, party_crypto.h).
  RemoteGuest  the JSON calls a guest's NP/HTTP layer makes over PartyLink RPC
               (gpu/shim/net/party_transport.h): the FROM API through "http" (ss.info, login,
               sync_chara_id, notices, summon_messenger/create with a 0xE0-byte SummonData,
               summon_messenger/get polling), context_start, join_room on guest_invite,
               heartbeat, signaling_resolve of the host, leave_room.
  UDP          optional: STUN Binding Request (+ the relay HELLO) to the host's party UDP port
               and hole-punch probes fe 'bbhp' at the host's resolved address.

Scripts (--script):
  join          one guest joins: boot online, put a sign up, get invited, join the host's room.
  crash-rejoin  join, then die without BYE / LeaveRoom (TCP reset), restart as a fresh process
                (no resume token) and be back in the host's room within --rejoin-budget s.
  soak N        N rounds of join + hold (--hold s), alternating a graceful leave and a crash.

Verdict lines on stdout: "PASS <script>: ..." or "FAIL <script>: <reason>" (exit code 0 / 1),
step lines "  ok   <step>: ..." / "  FAIL <step>: ...". A JSON log of every step, RPC, event and
verdict goes to --json-log (default simguest-<name>.json; "-" = none).

Connect with a party code ("BBP3-XXXX-...", carries host, port and secret) or --host/--port
(plain host:port parties: password only). The host checks the game version (PartyLink v2: eboot,
gameplay patches, gameplay mods): pass --game-dir DIR (DIR/eboot.bin sha256 and DIR/out/
party_patch_hash.txt, party_mods.txt as party_runtime.cpp reads them) or --eboot-sha256 /
--patches-hash / --mods-hash; the harness defaults to zeros, which is also simguest's default.
--protocol 1 speaks the older PartyLink v1 HELLO.

Dependencies: Python 3.9+ and nothing else. Optional speed-ups, all pip-installable on Windows:
  pip install argon2-cffi    Argon2i key derivation in C (else numpy / pure Python, ~5-20 s)
  pip install numpy          vectorised Argon2i fallback
  pip install pynacl         XChaCha20-Poly1305 in C (libsodium)
  pip install cryptography   ChaCha20-Poly1305 in C (+ HChaCha20 here); else pure Python
The key is derived once per process (crash-rejoin and soak reuse it). --key-hex skips the
derivation (the hex the first run prints with -v).

Examples:
  python tools/party/simguest.py --code BBP3-.... --password pw --script join
  python tools/party/simguest.py --host 127.0.0.1 --port 9317 --secret 0011223344556677 \
      --script crash-rejoin --name Sim1
  python tools/party/simguest.py --code BBP3-.... --script soak 10 --hold 5
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import queue
import socket
import struct
import sys
import threading
import time

VERSION = "1"

# ==================================================================================================
# Crypto (party_crypto.h): Argon2i, keyed BLAKE2b, XChaCha20-Poly1305 (monocypher-compatible)
# ==================================================================================================

M64 = (1 << 64) - 1
M32 = 0xFFFFFFFF


def blake2b_keyed(key: bytes, *parts: bytes) -> bytes:
    h = hashlib.blake2b(key=key, digest_size=32)
    for p in parts:
        h.update(p)
    return h.digest()


def _blake2b_long(out_len: int, data: bytes) -> bytes:
    """Argon2's H' (variable-length hash)."""
    pre = struct.pack("<I", out_len)
    if out_len <= 64:
        return hashlib.blake2b(pre + data, digest_size=out_len).digest()
    out = bytearray()
    v = hashlib.blake2b(pre + data, digest_size=64).digest()
    out += v[:32]
    remaining = out_len - 32
    while remaining > 64:
        v = hashlib.blake2b(v, digest_size=64).digest()
        out += v[:32]
        remaining -= 32
    out += hashlib.blake2b(v, digest_size=remaining).digest()
    return bytes(out)


def _argon2_h0(password: bytes, salt: bytes, t: int, m: int, tag: int) -> bytes:
    h = hashlib.blake2b(digest_size=64)
    for v in (1, tag, m, t, 0x13, 1):  # lanes, tag length, memory, passes, version, type (Argon2i)
        h.update(struct.pack("<I", v))
    h.update(struct.pack("<I", len(password)) + password)
    h.update(struct.pack("<I", len(salt)) + salt)
    h.update(struct.pack("<I", 0))  # secret
    h.update(struct.pack("<I", 0))  # additional data
    return h.digest()


def _index_alpha(pass_: int, slice_: int, index: int, seg: int, lane_len: int, j1: int) -> int:
    if pass_ == 0:
        area = index - 1 if slice_ == 0 else slice_ * seg + index - 1
    else:
        area = lane_len - seg + index - 1
    rel = (j1 * j1) >> 32
    rel = area - 1 - ((area * rel) >> 32)
    start = 0 if pass_ == 0 else (0 if slice_ == 3 else (slice_ + 1) * seg)
    return (start + rel) % lane_len


def _argon2i_generic(password: bytes, salt: bytes, t: int, m: int, tag: int, fill_block, new_block, to_words,
                     from_words) -> bytes:
    """Argon2i (v1.3, one lane); the block representation is the backend's."""
    m = max(8, m - m % 4)
    seg = m // 4
    h0 = _argon2_h0(password, salt, t, m, tag)
    mem = [None] * m
    mem[0] = from_words(struct.unpack("<128Q", _blake2b_long(1024, h0 + struct.pack("<II", 0, 0))))
    mem[1] = from_words(struct.unpack("<128Q", _blake2b_long(1024, h0 + struct.pack("<II", 1, 0))))
    zero = new_block()
    for p in range(t):
        for s in range(4):
            words = [0] * 128
            words[0:6] = [p, 0, s, m, t, 1]
            counter = 0
            addresses: list[int] = []

            def next_addresses():
                nonlocal counter, addresses
                counter += 1
                words[6] = counter
                a = fill_block(zero, from_words(words), None)
                a = fill_block(zero, a, None)
                addresses = to_words(a)

            start = 0
            if p == 0 and s == 0:
                start = 2
                next_addresses()
            for i in range(start, seg):
                if i % 128 == 0:
                    next_addresses()
                rand = addresses[i % 128]
                cur = s * seg + i
                prev = cur - 1 if cur else m - 1
                ref = _index_alpha(p, s, i, seg, m, rand & M32)
                mem[cur] = fill_block(mem[prev], mem[ref], mem[cur] if p else None)
    return _blake2b_long(tag, struct.pack("<128Q", *to_words(mem[m - 1])))


# ---- pure Python block function ----

def _gb(v, a, b, c, d):
    va, vb, vc, vd = v[a], v[b], v[c], v[d]
    va = (va + vb + 2 * (va & M32) * (vb & M32)) & M64
    x = vd ^ va
    vd = ((x >> 32) | (x << 32)) & M64
    vc = (vc + vd + 2 * (vc & M32) * (vd & M32)) & M64
    x = vb ^ vc
    vb = ((x >> 24) | (x << 40)) & M64
    va = (va + vb + 2 * (va & M32) * (vb & M32)) & M64
    x = vd ^ va
    vd = ((x >> 16) | (x << 48)) & M64
    vc = (vc + vd + 2 * (vc & M32) * (vd & M32)) & M64
    x = vb ^ vc
    vb = ((x >> 63) | (x << 1)) & M64
    v[a], v[b], v[c], v[d] = va, vb, vc, vd


_ROUND = ((0, 4, 8, 12), (1, 5, 9, 13), (2, 6, 10, 14), (3, 7, 11, 15),
          (0, 5, 10, 15), (1, 6, 11, 12), (2, 7, 8, 13), (3, 4, 9, 14))
_ROWS = [[16 * i + k for k in range(16)] for i in range(8)]
_COLS = [[2 * i + 16 * j + e for j in range(8) for e in range(2)] for i in range(8)]


def _fill_block_py(prev, ref, old):
    r = [x ^ y for x, y in zip(prev, ref)]
    tmp = r[:] if old is None else [x ^ y for x, y in zip(r, old)]
    for idx in _ROWS + _COLS:
        v = [r[k] for k in idx]
        for q in _ROUND:
            _gb(v, *q)
        for k, val in zip(idx, v):
            r[k] = val
    return [x ^ y for x, y in zip(tmp, r)]


def argon2i_pure(password: bytes, salt: bytes, t: int = 3, m: int = 8192, tag: int = 32) -> bytes:
    return _argon2i_generic(password, salt, t, m, tag, _fill_block_py, lambda: [0] * 128, lambda b: b,
                            lambda w: list(w))


# ---- numpy block function: the 8 rows (then the 8 columns) as one (8, 16) array ----

def argon2i_numpy(password: bytes, salt: bytes, t: int = 3, m: int = 8192, tag: int = 32) -> bytes:
    import numpy as np

    u = np.uint64
    c32, s1, r32, r24, r16, r63 = u(M32), u(1), u(32), u(24), u(16), u(63)
    l32, l40, l48, l1 = u(32), u(40), u(48), u(1)
    diag_b, diag_c, diag_d = [5, 6, 7, 4], [10, 11, 8, 9], [15, 12, 13, 14]

    def g(a, b, c, d):
        a = a + b + (((a & c32) * (b & c32)) << s1)
        x = d ^ a
        d = (x >> r32) | (x << l32)
        c = c + d + (((c & c32) * (d & c32)) << s1)
        x = b ^ c
        b = (x >> r24) | (x << l40)
        a = a + b + (((a & c32) * (b & c32)) << s1)
        x = d ^ a
        d = (x >> r16) | (x << l48)
        c = c + d + (((c & c32) * (d & c32)) << s1)
        x = b ^ c
        b = (x >> r63) | (x << l1)
        return a, b, c, d

    def perm(x):  # x: (8, 16), one P per row
        a, b, c, d = g(x[:, 0:4], x[:, 4:8], x[:, 8:12], x[:, 12:16])
        x = np.concatenate((a, b, c, d), axis=1)
        a, b, c, d = g(x[:, 0:4], x[:, diag_b], x[:, diag_c], x[:, diag_d])
        y = np.empty_like(x)
        y[:, 0:4] = a
        y[:, diag_b] = b
        y[:, diag_c] = c
        y[:, diag_d] = d
        return y

    def fill(prev, ref, old):
        r = prev ^ ref
        tmp = r if old is None else r ^ old
        x = perm(r.reshape(8, 16))
        x = x.reshape(8, 8, 2).transpose(1, 0, 2).reshape(8, 16)
        x = perm(x)
        x = x.reshape(8, 8, 2).transpose(1, 0, 2).reshape(128)
        return tmp ^ x

    with np.errstate(over="ignore"):
        return _argon2i_generic(password, salt, t, m, tag, fill, lambda: np.zeros(128, dtype=np.uint64),
                                lambda b: [int(v) for v in b], lambda w: np.array(w, dtype=np.uint64))


def argon2i(password: bytes, salt: bytes, t: int = 3, m: int = 8192, tag: int = 32) -> tuple[bytes, str]:
    try:
        from argon2.low_level import Type, hash_secret_raw  # argon2-cffi

        return hash_secret_raw(password, salt, time_cost=t, memory_cost=m, parallelism=1, hash_len=tag,
                               type=Type.I, version=19), "argon2-cffi"
    except ImportError:
        pass
    why = ""
    try:
        import numpy  # noqa: F401

        return argon2i_numpy(password, salt, t, m, tag), "numpy"
    except Exception as e:  # not installed, or an unusable build: the pure version gives the same key
        why = f" (numpy: {e!r})"
    return argon2i_pure(password, salt, t, m, tag), "pure-python" + why


def derive_party_key(password: str, secret: bytes) -> tuple[bytes, str]:
    return argon2i(password.encode() + secret, b"bbparty1")


# ---- ChaCha20 / Poly1305 ----

def _rotl32(v, n):
    return ((v << n) & M32) | (v >> (32 - n))


def _chacha_rounds(s: list[int]) -> list[int]:
    x = s[:]
    for _ in range(10):
        for a, b, c, d in ((0, 4, 8, 12), (1, 5, 9, 13), (2, 6, 10, 14), (3, 7, 11, 15),
                           (0, 5, 10, 15), (1, 6, 11, 12), (2, 7, 8, 13), (3, 4, 9, 14)):
            x[a] = (x[a] + x[b]) & M32
            x[d] = _rotl32(x[d] ^ x[a], 16)
            x[c] = (x[c] + x[d]) & M32
            x[b] = _rotl32(x[b] ^ x[c], 12)
            x[a] = (x[a] + x[b]) & M32
            x[d] = _rotl32(x[d] ^ x[a], 8)
            x[c] = (x[c] + x[d]) & M32
            x[b] = _rotl32(x[b] ^ x[c], 7)
    return x


_SIGMA = list(struct.unpack("<4I", b"expand 32-byte k"))


def hchacha20(key: bytes, nonce16: bytes) -> bytes:
    x = _chacha_rounds(_SIGMA + list(struct.unpack("<8I", key)) + list(struct.unpack("<4I", nonce16)))
    return struct.pack("<8I", *(x[0:4] + x[12:16]))


def _chacha20_xor(key: bytes, nonce12: bytes, counter: int, data: bytes) -> bytes:
    k = list(struct.unpack("<8I", key))
    n = list(struct.unpack("<3I", nonce12))
    out = bytearray()
    for off in range(0, len(data), 64):
        s = _SIGMA + k + [counter & M32] + n
        x = _chacha_rounds(s)
        ks = struct.pack("<16I", *((a + b) & M32 for a, b in zip(x, s)))
        chunk = data[off:off + 64]
        out += bytes(a ^ b for a, b in zip(chunk, ks))
        counter += 1
    return bytes(out)


def _poly1305(key: bytes, msg: bytes) -> bytes:
    r = int.from_bytes(key[:16], "little") & 0x0ffffffc0ffffffc0ffffffc0fffffff
    s = int.from_bytes(key[16:], "little")
    p = (1 << 130) - 5
    acc = 0
    for i in range(0, len(msg), 16):
        acc = ((acc + int.from_bytes(msg[i:i + 16] + b"\x01", "little")) * r) % p
    return ((acc + s) & ((1 << 128) - 1)).to_bytes(16, "little")


def _pad16(b: bytes) -> bytes:
    return b"\0" * (-len(b) % 16)


def _aead_pure(key: bytes, nonce12: bytes, ad: bytes, data: bytes, encrypt: bool):
    otk = _chacha20_xor(key, nonce12, 0, b"\0" * 32)
    if encrypt:
        ct = _chacha20_xor(key, nonce12, 1, data)
    else:
        ct = data
    mac = _poly1305(otk, ad + _pad16(ad) + ct + _pad16(ct) + struct.pack("<QQ", len(ad), len(ct)))
    if encrypt:
        return ct, mac
    return mac


class Aead:
    """One direction of a PartyLink stream: XChaCha20-Poly1305, nonce = u64 LE counter || 16 zeros."""

    backend = None

    def __init__(self, key: bytes):
        self.key = key
        self.counter = 0
        if Aead.backend is None:
            Aead.backend = Aead._pick()

    @staticmethod
    def _pick() -> str:
        try:
            from nacl.bindings import crypto_aead_xchacha20poly1305_ietf_encrypt  # noqa: F401

            return "pynacl"
        except ImportError:
            pass
        try:
            from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305  # noqa: F401

            return "cryptography"
        except ImportError as e:
            return f"pure-python (cryptography: {e})"

    def _nonce(self) -> bytes:
        return struct.pack("<Q", self.counter) + b"\0" * 16

    def seal(self, ad: bytes, pt: bytes) -> tuple[bytes, bytes]:
        nonce = self._nonce()
        if Aead.backend == "pynacl":
            from nacl.bindings import crypto_aead_xchacha20poly1305_ietf_encrypt as enc

            out = enc(pt, ad, nonce, self.key)
            ct, mac = out[:-16], out[-16:]
        else:
            sub = hchacha20(self.key, nonce[:16])
            n12 = b"\0\0\0\0" + nonce[16:]
            if Aead.backend == "cryptography":
                from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

                out = ChaCha20Poly1305(sub).encrypt(n12, pt, ad)
                ct, mac = out[:-16], out[-16:]
            else:
                ct, mac = _aead_pure(sub, n12, ad, pt, True)
        self.counter += 1
        return ct, mac

    def open(self, ad: bytes, mac: bytes, ct: bytes) -> bytes | None:
        nonce = self._nonce()
        try:
            if Aead.backend == "pynacl":
                from nacl.bindings import crypto_aead_xchacha20poly1305_ietf_decrypt as dec

                pt = dec(ct + mac, ad, nonce, self.key)
            else:
                sub = hchacha20(self.key, nonce[:16])
                n12 = b"\0\0\0\0" + nonce[16:]
                if Aead.backend == "cryptography":
                    from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

                    pt = ChaCha20Poly1305(sub).decrypt(n12, ct + mac, ad)
                else:
                    if not hmac_equal(_aead_pure(sub, n12, ad, ct, False), mac):
                        return None
                    pt = _chacha20_xor(sub, n12, 1, ct)
        except Exception:
            return None
        self.counter += 1
        return pt


def hmac_equal(a: bytes, b: bytes) -> bool:
    import hmac

    return hmac.compare_digest(a, b)


# ==================================================================================================
# Party codes (party_code.h)
# ==================================================================================================

ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
DEFAULT_PORT = 9307


def crc16_ccitt(d: bytes) -> int:
    crc = 0xFFFF
    for b in d:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def crc8(d: bytes) -> int:
    crc = 0
    for b in d:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def _local_prefix() -> list:
    """The first three octets of this machine's outbound IPv4 (127.0.0 when there is none)."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))  # no packet is sent
            return [int(x) for x in s.getsockname()[0].split(".")[:3]]
    except OSError:
        return [127, 0, 0]


def _decode_v2(vals: list) -> dict:
    """BBP2: flags | ipv4[4] (LAN: last octet) | port[2] (absent with flag 8) | secret[6] | crc8."""
    if len(vals) < 2:
        raise ValueError("party code too short")
    wire = (vals[0] << 3 | vals[1] >> 2) & 0xFF
    size = 1 + (1 if wire & 1 else 4) + (0 if wire & 8 else 2) + 6 + 1
    if len(vals) != (size * 8 + 4) // 5:
        raise ValueError(f"party code has {len(vals)} symbols, expected {(size * 8 + 4) // 5}")
    acc, bits, raw = 0, 0, bytearray()
    for v in vals:
        acc = (acc << 5) | v
        bits += 5
        if bits >= 8:
            raw.append((acc >> (bits - 8)) & 0xFF)
            bits -= 8
    if crc8(bytes(raw[:-1])) != raw[-1] or acc & ((1 << bits) - 1):
        raise ValueError("party code checksum mismatch (typo?)")
    n = 1
    if wire & 1:
        ip = _local_prefix() + [raw[n]]
        n += 1
    else:
        ip = list(raw[n:n + 4])
        n += 4
    port = DEFAULT_PORT
    if not wire & 8:
        port = raw[n] << 8 | raw[n + 1]
        n += 2
    return {"host": ".".join(str(b) for b in ip), "port": port, "secret": bytes(raw[n:n + 6]) + b"\0\0",
            "flags": wire & ~8, "plain": False}


_CRC_BITS_POLY = {8: 0x07, 9: 0x119, 10: 0x233, 11: 0x385, 12: 0x80F}


def _decode_v3(vals: list) -> dict:
    """BBP3 (party_code.h): bits flags(4) | ipv4 32 (LAN: 8) | port 16 (none with flag 8) |
    secret 48 (32 with flag 2) | a CRC filling the last symbol (8..12 bits, init all ones)."""
    if not vals:
        raise ValueError("party code too short")
    wire = vals[0] >> 1
    payload = 4 + (8 if wire & 1 else 32) + (0 if wire & 8 else 16) + (32 if wire & 2 else 48)
    want = (payload + 12) // 5
    if len(vals) != want:
        raise ValueError(f"party code has {len(vals)} symbols, expected {want}")
    bits = [(v >> i) & 1 for v in vals for i in range(4, -1, -1)]
    k = len(bits) - payload
    reg, mask = (1 << k) - 1, (1 << k) - 1
    for b in bits[:payload]:
        top = ((reg >> (k - 1)) & 1) ^ b
        reg = (reg << 1) & mask
        if top:
            reg ^= _CRC_BITS_POLY[k]
    pos = [4]

    def take(n):
        v = int("".join(map(str, bits[pos[0]:pos[0] + n])), 2)
        pos[0] += n
        return v

    if int("".join(map(str, bits[payload:])), 2) != reg:
        raise ValueError("party code checksum mismatch (typo?)")
    ip = _local_prefix() + [take(8)] if wire & 1 else [take(8) for _ in range(4)]
    port = DEFAULT_PORT if wire & 8 else take(16)
    secret = bytes(take(8) for _ in range(4 if wire & 2 else 6))
    return {"host": ".".join(str(b) for b in ip), "port": port, "secret": secret.ljust(8, b"\0"),
            "flags": wire & ~8, "plain": False}


def decode_party_code(text: str) -> dict:
    """{'host', 'port', 'secret', 'flags', 'plain'}; raises ValueError."""
    t = text.strip()
    if not t:
        raise ValueError("empty party code")
    if not t.upper().startswith("BBP"):
        host, _, port = t.rpartition(":")
        if not host:
            host, port = t, str(DEFAULT_PORT)
        return {"host": host, "port": int(port), "secret": b"\0" * 8, "flags": 0, "plain": True}
    if t[3:4].upper() not in ("1", "I", "L", "2", "3"):
        raise ValueError("unknown party code version")
    v2, v3 = t[3] == "2", t[3] == "3"
    body = t[4:]
    vals = []
    for c in body:
        if c in "- \t":
            continue
        c = c.upper()
        c = {"I": "1", "L": "1", "O": "0"}.get(c, c)
        if c not in ALPHABET:
            raise ValueError(f"bad character {c!r} in the party code")
        vals.append(ALPHABET.index(c))
    if v3:
        return _decode_v3(vals)
    if v2:
        return _decode_v2(vals)
    if len(vals) != 29:
        raise ValueError(f"party code has {len(vals)} symbols, expected 29")
    acc, bits, raw = 0, 0, bytearray()
    for v in vals:
        acc = (acc << 5) | v
        bits += 5
        if bits >= 8:
            raw.append((acc >> (bits - 8)) & 0xFF)
            bits -= 8
    raw = bytes(raw[:18])
    if crc16_ccitt(raw[:16]) != (raw[16] << 8 | raw[17]):
        raise ValueError("party code checksum mismatch (typo?)")
    if raw[0] != 1:
        raise ValueError(f"party code version {raw[0]} unknown")
    return {"host": ".".join(str(b) for b in raw[2:6]), "port": raw[6] << 8 | raw[7], "secret": raw[8:16],
            "flags": raw[1], "plain": False}


# ==================================================================================================
# PartyLink guest (party_link.cpp's guest side)
# ==================================================================================================

HELLO, CHALLENGE, AUTH, WELCOME, REJECT = 1, 2, 3, 4, 5
PING, PONG, ROSTER, RPC_REQ, RPC_RESP, EVENT, EVENT_ACK, PARTY_CMD, PROGRESS, BYE = range(10, 20)
ENCRYPTED = 0xE0
PROTOCOL_VERSION = 2  # 2: HELLO carries the gameplay patch / mod sets (1: --protocol 1)
STATES = ["title", "home", "joining", "in_host_world", "dead", "loading"]
REJECT_NAMES = {0: "none", 1: "version", 2: "mismatch", 3: "auth", 4: "full", 5: "name", 6: "protocol",
                7: "shutdown", 8: "kicked"}
ST_TITLE, ST_HOME, ST_JOINING, ST_IN_HOST, ST_DEAD, ST_LOADING = range(6)


class LinkError(Exception):
    def __init__(self, msg: str, code: int = -1):
        super().__init__(msg)
        self.code = code


class Rd:
    def __init__(self, b: bytes):
        self.b, self.o = b, 0

    def take(self, n: int) -> bytes:
        if self.o + n > len(self.b):
            raise LinkError("short frame")
        v = self.b[self.o:self.o + n]
        self.o += n
        return v

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack("<H", self.take(2))[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def u64(self):
        return struct.unpack("<Q", self.take(8))[0]

    def str16(self):
        return self.take(self.u16()).decode("utf-8", "replace")

    def str32(self):
        return self.take(self.u32()).decode("utf-8", "replace")


def w_str16(s: str) -> bytes:
    b = s.encode()[:0xFFFF]
    return struct.pack("<H", len(b)) + b


def w_str32(s: str) -> bytes:
    b = s.encode()
    return struct.pack("<I", len(b)) + b


def read_roster(r: Rd) -> list[dict]:
    out = []
    for _ in range(r.u8()):
        e = {"slot": r.u8(), "name": r.str16()}
        st = r.u8()
        e["state"] = STATES[st] if st < len(STATES) else "title"
        e["connected"] = bool(r.u8())
        e["map_id"] = r.u32()
        e["ping_ms"] = r.u32()
        out.append(e)
    return out


class Link:
    """One PartyLink connection, guest side. Threads: a reader and a pinger."""

    def __init__(self, name: str, key: bytes, eboot: bytes, mods: bytes, token: bytes, log,
                 ping_interval: float = 1.0, lost_timeout: float = 10.0, patches: bytes = b"\0" * 32,
                 patch_names: list[str] | None = None, mod_names: list[str] | None = None,
                 protocol: int = PROTOCOL_VERSION, ident: dict | None = None):
        self.name, self.key, self.eboot, self.mods, self.token = name, key, eboot, mods, token
        self.patches, self.patch_names, self.mod_names = patches, patch_names or [], mod_names or []
        self.protocol = protocol
        self.ident = ident or {}  # A9: rules, build, params, hooks, hook names (absent: not sent)
        self.log = log
        self.ping_interval, self.lost_timeout = ping_interval, lost_timeout
        self.sock: socket.socket | None = None
        self.wlock = threading.Lock()
        self.lock = threading.Lock()
        self.tx: Aead | None = None
        self.rx: Aead | None = None
        self.alive = False
        self.closed_reason = ""
        self.reject: tuple[int, str] | None = None
        self.slot = -1
        self.max_players = 0
        self.resumed = False
        self.observed = ""
        self.roster: list[dict] = []
        self.roster_cv = threading.Condition()
        self.pending: dict[int, list] = {}
        self.next_rpc = 1
        self.rx_last = 0
        self.events: "queue.Queue[dict]" = queue.Queue()
        self.rtt_ms: list[int] = []
        self.host_pings = 0
        self.frames_in = 0
        self.frames_out = 0
        self.last_rx = time.monotonic()
        self.state = ST_TITLE
        self.map_id = 0
        self.peer = ("", 0)
        self.t0 = time.monotonic()

    def now_ms(self) -> int:
        return int((time.monotonic() - self.t0) * 1000)

    # ---- framing ----
    def _send_raw(self, type_: int, body: bytes) -> None:
        with self.wlock:
            if self.tx is not None:
                n = 1 + 16 + 1 + len(body)
                hdr = struct.pack("<IB", n, ENCRYPTED)
                ct, mac = self.tx.seal(hdr, bytes([type_]) + body)
                data = hdr + mac + ct
            else:
                data = struct.pack("<IB", 1 + len(body), type_) + body
            self.sock.sendall(data)
            self.frames_out += 1

    def send(self, type_: int, body: bytes = b"") -> bool:
        if not self.sock:
            return False
        try:
            self._send_raw(type_, body)
            return True
        except OSError as e:
            self._lost(f"send failed: {e}")
            return False

    def _recv_exact(self, n: int) -> bytes:
        buf = bytearray()
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise LinkError("connection closed by the host")
            buf += chunk
        return bytes(buf)

    def _read_frame(self) -> tuple[int, bytes, bool]:
        hdr = self._recv_exact(5)
        n, type_ = struct.unpack("<IB", hdr)
        if n == 0 or n > (16 << 20):
            raise LinkError(f"bad frame length {n}")
        body = self._recv_exact(n - 1)
        self.last_rx = time.monotonic()
        self.frames_in += 1
        if type_ == ENCRYPTED:
            if self.rx is None or len(body) < 17:
                raise LinkError("unexpected encrypted frame")
            pt = self.rx.open(hdr, body[:16], body[16:])
            if pt is None:
                raise LinkError("frame failed authentication (wrong key?)")
            return pt[0], pt[1:], True
        return type_, body, False

    # ---- handshake ----
    def connect(self, host: str, port: int, timeout: float = 5.0) -> None:
        self.peer = (host, port)
        s = socket.create_connection((host, port), timeout=timeout)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.settimeout(timeout)
        self.sock = s
        hello = b"BBPL" + struct.pack("<H", self.protocol) + w_str16(self.name) + self.eboot + self.mods + self.token
        if self.protocol >= 2:
            hello += self.patches + w_str16("\n".join(self.patch_names)) + w_str16("\n".join(self.mod_names))
            i = self.ident
            if "rules" in i or "build" in i:  # trailing optional fields, in order (party_link.h)
                hello += w_str16(i.get("rules", ""))
            if "build" in i:
                hello += (w_str16(i["build"]) + i.get("params", b"\0" * 32) + i.get("hooks", b"\0" * 32) +
                          w_str16("\n".join(i.get("hook_names", []))))
        self._send_raw(HELLO, hello)
        t, body, enc = self._read_frame()
        if t == REJECT and not enc:
            self._rejected(Rd(body))
        if t != CHALLENGE or enc:
            raise LinkError(f"expected CHALLENGE, got type {t}")
        hn = body[:24]
        gn = os.urandom(24)
        proof = blake2b_keyed(self.key, b"bbp-auth", hn, gn)
        self._send_raw(AUTH, gn + proof)
        self.rx = Aead(blake2b_keyed(self.key, b"bbp-h2g", hn, gn))
        tx_key = blake2b_keyed(self.key, b"bbp-g2h", hn, gn)
        t, body, enc = self._read_frame()
        if t == REJECT and not enc:
            self._rejected(Rd(body))
        if t != WELCOME or not enc:
            raise LinkError(f"expected WELCOME, got type {t}")
        r = Rd(body)
        self.slot, self.max_players, self.resumed = r.u8(), r.u8(), bool(r.u8())
        r.u64()  # host clock
        ip = r.take(4)
        oport = r.u16()
        self.token = r.take(16)
        self.roster = read_roster(r)
        self.observed = f"{ip[0]}.{ip[1]}.{ip[2]}.{ip[3]}:{oport}"
        self.tx = Aead(tx_key)
        self.alive = True
        s.settimeout(None)
        self.send_state()
        threading.Thread(target=self._reader, daemon=True, name="link-reader").start()
        threading.Thread(target=self._pinger, daemon=True, name="link-pinger").start()

    def _rejected(self, r: Rd) -> None:
        code = r.u8()
        why = r.str16()
        self.reject = (code, why)
        raise LinkError(f"rejected ({REJECT_NAMES.get(code, code)}): {why}", code)

    # ---- runtime ----
    def _reader(self) -> None:
        try:
            while self.alive:
                t, body, enc = self._read_frame()
                if not enc:
                    if t == REJECT:
                        try:
                            self._rejected(Rd(body))
                        except LinkError as e:
                            self._lost(str(e))
                            return
                    raise LinkError("plaintext frame after WELCOME")
                self._dispatch(t, Rd(body))
        except (LinkError, OSError) as e:
            if self.alive:
                self._lost(str(e))

    def _dispatch(self, t: int, r: Rd) -> None:
        if t == PING:
            self.host_pings += 1
            self.send(PONG, struct.pack("<QQ", r.u64(), 0))
        elif t == PONG:
            sent = r.u64()
            r.u64()
            self.rtt_ms.append(self.now_ms() - sent)
        elif t == ROSTER:
            ros = read_roster(r)
            with self.roster_cv:
                self.roster = ros
                self.roster_cv.notify_all()
        elif t == RPC_RESP:
            rid, ok, reply = r.u32(), bool(r.u8()), r.str32()
            with self.lock:
                p = self.pending.get(rid)
            if p:
                p[1], p[2] = ok, reply
                p[0].set()
        elif t == EVENT:
            cursor, name, js = r.u64(), r.str16(), r.str32()
            if cursor > self.rx_last:
                self.rx_last = cursor
                try:
                    ev = json.loads(js)
                except ValueError:
                    ev = {"_raw": js}
                ev.setdefault("Name", name)
                ev["_cursor"] = cursor
                self.events.put(ev)
            self.send(EVENT_ACK, struct.pack("<Q", cursor))
        elif t == EVENT_ACK:
            pass
        elif t == PARTY_CMD:
            cmd, js = r.str16(), r.str32()
            self.log("link", f"party cmd {cmd} {js}")
        elif t == PROGRESS:
            self.log("link", f"progress blob {len(r.str32())} B")
        elif t == BYE:
            code, why = r.u8(), r.str16()
            self.reject = (code or 7, why or "the host ended the party")
            self._lost(f"BYE ({REJECT_NAMES.get(code, code)}): {why}")

    def _pinger(self) -> None:
        while self.alive:
            time.sleep(self.ping_interval)
            if not self.alive:
                return
            if time.monotonic() - self.last_rx > self.lost_timeout:
                self._lost(f"no traffic for {self.lost_timeout:.0f} s")
                return
            self.send(PING, struct.pack("<Q", self.now_ms()))

    def _lost(self, why: str) -> None:
        with self.lock:
            if not self.alive and self.closed_reason:
                return
            self.alive = False
            self.closed_reason = why
            pend = list(self.pending.values())
        for p in pend:
            p[1], p[2] = False, "connection lost: " + why
            p[0].set()
        try:
            self.sock.close()
        except OSError:
            pass
        with self.roster_cv:
            self.roster_cv.notify_all()

    def send_state(self, state: int | None = None, map_id: int | None = None) -> None:
        if state is not None:
            self.state = state
        if map_id is not None:
            self.map_id = map_id
        body = bytes([1, self.slot & 0xFF]) + w_str16(self.name) + bytes([self.state, 1]) + \
            struct.pack("<II", self.map_id, 0)
        self.send(ROSTER, body)

    def rpc(self, kind: str, req: dict, timeout: float = 4.0) -> tuple[bool, str]:
        with self.lock:
            rid = self.next_rpc
            self.next_rpc += 1
            p = [threading.Event(), False, ""]
            self.pending[rid] = p
        if not self.alive or not self.send(RPC_REQ, struct.pack("<I", rid) + w_str16(kind) +
                                           w_str32(json.dumps(req, separators=(",", ":")))):
            with self.lock:
                self.pending.pop(rid, None)
            return False, "link down: " + (self.closed_reason or "not connected")
        ok = p[0].wait(timeout)
        with self.lock:
            self.pending.pop(rid, None)
        if not ok:
            return False, f"{kind}: timed out after {timeout:.0f} s"
        return p[1], p[2]

    def wait_roster(self, pred, timeout: float) -> bool:
        end = time.monotonic() + timeout
        with self.roster_cv:
            while not pred(self.roster):
                left = end - time.monotonic()
                if left <= 0 or not self.alive:
                    return pred(self.roster)
                self.roster_cv.wait(left)
            return True

    def bye(self) -> None:
        self.send(BYE, bytes([0]) + w_str16("simguest leaves"))
        time.sleep(0.2)
        self.alive = False
        self.closed_reason = "bye"
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
            self.sock.close()
        except OSError:
            pass

    def crash(self) -> None:
        """The process dies: no BYE, TCP reset (SO_LINGER 0)."""
        self.alive = False
        self.closed_reason = "crash"
        try:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("hh", 1, 0))
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


# ==================================================================================================
# UDP: STUN Binding (+ relay HELLO) and hole-punch probes (net_stun.h, net_socket.cpp)
# ==================================================================================================

PROBE = b"\xfebbhp\0\0\0"
STUN_COOKIE = 0x2112A442


class Udp:
    def __init__(self, port: int = 0, bind: str = "0.0.0.0"):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind((bind, port))
        self.s.settimeout(0.5)
        self.port = self.s.getsockname()[1]
        self.token = b"\0" * 8
        self.relay_vport = 0
        self.mapped: tuple[str, int] | None = None

    def stun(self, host: str, port: int, hello: bool = True, tries: int = 4) -> dict | None:
        txid = struct.pack(">I", STUN_COOKIE) + os.urandom(12)
        attrs = b""
        if hello:
            attrs = struct.pack(">HH", 0x8100, 12) + b"bbr1" + self.token
        req = struct.pack(">HH", 0x0001, len(attrs)) + txid + attrs
        for _ in range(tries):
            self.s.sendto(req, (host, port))
            end = time.monotonic() + 0.5
            while time.monotonic() < end:
                try:
                    data, _src = self.s.recvfrom(2048)
                except socket.timeout:
                    break
                except OSError:
                    break
                res = self._parse(data, txid)
                if res:
                    if res.get("relay"):
                        self.token = res["relay"]["token"]
                        self.relay_vport = res["relay"]["vport"]
                    self.mapped = (res["addr"], res["port"])
                    return res
        return None

    @staticmethod
    def _parse(d: bytes, txid: bytes) -> dict | None:
        if len(d) < 20 or struct.unpack(">H", d[:2])[0] != 0x0101 or d[4:20] != txid:
            return None
        ln = struct.unpack(">H", d[2:4])[0]
        at, end = 20, 20 + ln
        out: dict = {}
        xa = xp = None
        while at + 4 <= end:
            typ, alen = struct.unpack(">HH", d[at:at + 4])
            v = d[at + 4:at + 4 + alen]
            if typ == 0x8101 and alen >= 20 and v[:4] == b"bbr1":
                out["relay"] = {"token": v[4:12], "vport": struct.unpack(">H", v[12:14])[0],
                                "observed": socket.inet_ntoa(v[14:18]) + ":" + str(struct.unpack(">H", v[18:20])[0])}
            elif typ == 0x0001 and alen >= 8 and v[1] == 1:
                out["port"] = struct.unpack(">H", v[2:4])[0]
                out["addr"] = socket.inet_ntoa(v[4:8])
            elif typ in (0x0020, 0x8020) and alen >= 8 and v[1] == 1:
                xp = struct.unpack(">H", v[2:4])[0] ^ struct.unpack(">H", txid[:2])[0]
                xa = socket.inet_ntoa(bytes(a ^ b for a, b in zip(v[4:8], txid[:4])))
            at += 4 + ((alen + 3) & ~3)
        if "addr" not in out and xa:
            out["addr"], out["port"] = xa, xp
        if "addr" in out and xa and (out["addr"], out["port"]) != (xa, xp):
            out["xor_mismatch"] = f"{xa}:{xp}"
        return out if "addr" in out else None

    def probe(self, host: str, port: int, n: int = 3) -> int:
        sent = 0
        for _ in range(n):
            try:
                self.s.sendto(PROBE, (host, port))
                sent += 1
            except OSError:
                pass
        return sent

    def close(self) -> None:
        try:
            self.s.close()
        except OSError:
            pass


# ==================================================================================================
# SummonData (docs/party/from_api_schema.md §5)
# ==================================================================================================

SIGN_NORMAL_COOP = 7
SUMMON_TYPE_COOP = 0


def summon_data(online_id: str, area: int, pos: tuple[float, float, float], level: int, region: int,
                chara_id: int, nat_type: int = 2, sign_type: int = SIGN_NORMAL_COOP) -> bytes:
    b = bytearray(0xE0)
    # 0x00: 13 int32 phantom appearance / equipment ids (plausible non-zero values; not checked).
    struct.pack_into("<13i", b, 0x00, 100000, 1000000, 22000000, 6000, 270000, 271000, 272000, 273000,
                     -1, -1, -1, 1, 1)
    struct.pack_into("<4B", b, 0x34, 0xFF, 0xC8, 0x80, 0xFF)  # phantom colour RGBA
    b[0x38] = 1
    b[0x39:0x3E] = bytes([100, 100, 100, 100, 100])
    nb = online_id.encode()[:16]
    b[0x40:0x40 + len(nb)] = nb  # sender online ID, NUL padded (dedupe key)
    struct.pack_into("<Q", b, 0x50, int(time.time() * 1000))  # creation stamp, newer wins
    struct.pack_into("<I", b, 0x58, area)
    struct.pack_into("<3f", b, 0x5C, *pos)
    struct.pack_into("<f", b, 0x68, 0.0)  # yaw
    struct.pack_into("<I", b, 0x6C, region)
    struct.pack_into("<h", b, 0x70, level)
    struct.pack_into("<h", b, 0x72, 0)
    struct.pack_into("<H", b, 0x74, 0)
    b[0x76] = sign_type
    b[0x77] = 0  # refresh counter
    b[0x78] = 30  # lifetime
    b[0x79] = 0  # 0 on create; the receiver forces 99
    struct.pack_into("<H", b, 0x7A, 0x25)
    b[0x7C] = 2  # OnlineID serialization: 2 + SceNpId (36 bytes: handle[16], term, dummy[3], opt[8], rsvd[8])
    b[0x7D:0x7D + len(nb)] = nb
    b[0xCC] = nat_type
    struct.pack_into("<i", b, 0xD0, -1)  # server UserId: create sends 0xFFFFFFFF
    struct.pack_into("<Q", b, 0xD8, chara_id)
    return bytes(b)


def check_summon_data(blob: bytes) -> str:
    """'' when the game's GetList reader would take it, else why not (from_api_schema.md §4.5)."""
    if len(blob) != 0xE0:
        return f"{len(blob)} bytes, not 0xE0"
    if blob[0x7A:0x7C] != b"\x25\x00":
        return "OnlineID length field != 0x25"
    if blob[0x40] == 0:
        return "no sender online id at 0x40"
    return ""


# ==================================================================================================
# The guest's game, in miniature
# ==================================================================================================

SS_INFO_URL = "https://ss4.scej-network.jp:20443/bb-eu/ss.info"
API_TAGS = ("Login ServerTimeGet SyncCharaId NoticeNormalGet NoticeEmergencyGet UserAgreementGet BloodMessCreate "
            "BloodMessGetList BloodMessEvaluate BloodMessGetEvaluate BloodMessRemove BloodMessSearchAdd "
            "ChannelUpload ChannelShare ChannelSearch ChannelWordSearch ChannelGetDetailsInfo ChannelGetInfo "
            "ChannelRandomJoin ChannelAddMaterial ChannelAddMaterialCompleteNotify MessengerShellUpload "
            "MultiPlayNetError UserPropertiesMoveCount UserPropertiesMoveCountCheck SummonDataCreate "
            "SummonDataGetList SummonDataRemove SummonDataSummon ChairMessGetList ChairMessRespawnPointNotice "
            "TombMessCreate TombMessGetList DeathVisionGet TombMessRemove WanderingGhostCreate "
            "WanderingGhostGet").split()


class StepFail(Exception):
    pass


class JsonLog:
    def __init__(self, path: str | None, args: dict):
        self.path = path
        self.t0 = time.time()
        self.doc = {"tool": "simguest", "version": VERSION, "started": time.strftime("%Y-%m-%dT%H:%M:%S"),
                    "args": args, "records": [], "verdicts": []}
        self.lock = threading.Lock()

    def rec(self, kind: str, **kw) -> None:
        with self.lock:
            self.doc["records"].append({"t": round(time.time() - self.t0, 3), "kind": kind, **kw})

    def verdict(self, script: str, ok: bool, reason: str, **kw) -> None:
        with self.lock:
            self.doc["verdicts"].append({"t": round(time.time() - self.t0, 3), "script": script,
                                         "result": "PASS" if ok else "FAIL", "reason": reason, **kw})

    def save(self) -> None:
        if not self.path:
            return
        with self.lock:
            self.doc["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S")
            with open(self.path, "w", encoding="utf-8") as f:
                json.dump(self.doc, f, indent=1, default=lambda o: o.hex() if isinstance(o, bytes) else str(o))


class Guest:
    """One run of a guest game process: link, boot online, sign, invite, room."""

    def __init__(self, opts, key: bytes, jlog: JsonLog, token: bytes = b"\0" * 16, tag: str = ""):
        self.o = opts
        self.key = key
        self.j = jlog
        self.tag = tag
        self.link: Link | None = None
        self.token = token
        self.udp: Udp | None = None
        self.base = ""
        self.session_id = ""
        self.user_id = 0
        self.chara_id = 0
        self.room = 0
        self.room_sid = ""
        self.member_id = 0
        self.members: list[dict] = []
        self.host_name = ""
        self.host_addr: tuple[str, int] | None = None
        self.in_room = threading.Event()
        self.hb_ok = 0
        self.hb_fail = 0
        self.stop = threading.Event()
        self.sign_id = 0
        self.events_seen: list[dict] = []

    # ---- output ----
    def say(self, msg: str) -> None:
        if self.o.verbose:
            print(f"{time.strftime('%H:%M:%S')} simguest[{self.o.name}{self.tag}] {msg}", file=sys.stderr, flush=True)

    def ok(self, step: str, detail: str = "", **data) -> None:
        print(f"  ok   {step}: {detail}", flush=True)
        self.j.rec("step", step=step, ok=True, detail=detail, run=self.tag, **data)

    def fail(self, step: str, detail: str, **data):
        print(f"  FAIL {step}: {detail}", flush=True)
        self.j.rec("step", step=step, ok=False, detail=detail, run=self.tag, **data)
        raise StepFail(f"{step}: {detail}")

    def linklog(self, what: str, msg: str) -> None:
        self.say(f"{what}: {msg}")
        self.j.rec(what, msg=msg, run=self.tag)

    # ---- RPC helpers ----
    def call(self, kind: str, req: dict, timeout: float = 5.0) -> dict:
        t = time.monotonic()
        ok, raw = self.link.rpc(kind, req, timeout)
        dt = round((time.monotonic() - t) * 1000)
        try:
            reply = json.loads(raw) if ok else {}
        except ValueError:
            reply = {}
        if kind != "http":
            self.j.rec("rpc", call=kind, req=req, ok=ok, reply=reply if ok else raw, ms=dt, run=self.tag)
        self.say(f"rpc {kind} -> {raw[:200] if isinstance(raw, str) else raw} ({dt} ms)")
        if not ok:
            raise StepFail(f"{kind}: {raw}")
        return reply

    def http(self, method: str, url: str, body: dict | None = None) -> tuple[int, str]:
        b = json.dumps(body, separators=(",", ":")).encode() if body is not None else b""
        rq = {"Method": method, "Url": url, "Headers": [], "Body": base64.b64encode(b).decode()}
        t = time.monotonic()
        reply = self.call("http", rq, timeout=8.0)
        if reply.get("ResKind", 0) != 0:
            raise StepFail(f"http {url}: refused {reply}")
        text = base64.b64decode(reply.get("Body", "") or "").decode("utf-8", "replace")
        status = int(reply.get("Status", 0) or 0)
        self.j.rec("http", method=method, url=url, body=body, status=status, reply=text[:2000],
                   ms=round((time.monotonic() - t) * 1000), run=self.tag)
        return status, text

    def api(self, path: str, body: dict, query: bool = True) -> dict:
        url = self.base + path + (f"?user_id={self.user_id}" if query else "")
        if query:
            body = {**body, "SessionId": self.session_id, "UserId": self.user_id}
        status, text = self.http("POST", url, body)
        if status != 200:
            self.fail(path, f"HTTP {status} (the game: 0x80000001)")
        try:
            rep = json.loads(text)
        except ValueError:
            self.fail(path, f"not JSON (the game: 0x80000004): {text[:120]!r}")
        if not isinstance(rep, dict):
            self.fail(path, "root is not an object (0x80000004)")
        if int(rep.get("ResKind", 0)) != 0:
            self.fail(path, f"ResKind {rep.get('ResKind')}")
        return rep

    # ---- the steps ----
    def connect(self) -> None:
        o = self.o
        self.link = Link(o.name, self.key, o.eboot, o.mods, self.token, self.linklog,
                         lost_timeout=o.lost_timeout, patches=o.patches, patch_names=o.patch_names,
                         mod_names=o.mod_names, protocol=o.protocol, ident=o.ident)
        t = time.monotonic()
        try:
            self.link.connect(o.host_ip, o.host_port)
        except LinkError as e:
            self.fail("link", str(e), reject=self.link.reject)
        except OSError as e:
            self.fail("link", f"cannot connect to {o.host_ip}:{o.host_port}: {e}")
        self.token = self.link.token
        self.ok("link", f"slot {self.link.slot}/{self.link.max_players}{' (resumed)' if self.link.resumed else ''}, "
                        f"seen at {self.link.observed}, roster "
                        f"{[(e['name'], e['slot'], e['state']) for e in self.link.roster]} "
                        f"({(time.monotonic() - t) * 1000:.0f} ms, aead {Aead.backend})",
                slot=self.link.slot, resumed=self.link.resumed)
        self.host_name = next((e["name"] for e in self.link.roster if e["slot"] == 0), "")
        threading.Thread(target=self._event_loop, daemon=True).start()

    def boot_online(self) -> None:
        """The title's online chain: ss.info, login, sync_chara_id, notices, move-count check."""
        status, text = self.http("GET", SS_INFO_URL)
        if status != 200:
            self.fail("ss.info", f"HTTP {status}")
        if "<ss>" not in text:
            # The game base64-decodes the body before parsing it (completion 0x1e7f240), so the
            # host serves it encoded.
            try:
                text = base64.b64decode(text.strip(), validate=True).decode("utf-8", "replace")
            except ValueError:
                self.fail("ss.info", "body is neither XML nor base64")
        if "<ss>0</ss>" not in text:
            self.fail("ss.info", "no <ss>0</ss> (offline msg 0x1131)")
        bases = {}
        for n in range(10):
            a, b = text.find(f"<gameurl{n}>"), text.find(f"</gameurl{n}>")
            if a < 0 or b < 0:
                continue
            seg = text[a:b]
            missing = [t for t in API_TAGS if f"<api_{t}>" not in seg or f"</api_{t}>" not in seg]
            if missing:
                self.fail("ss.info", f"gameurl{n} misses {missing[:5]}")
            s = seg.find("<api_Login>") + len("<api_Login>")
            bases[n] = seg[s:seg.find("</api_Login>")]
        if not bases:
            self.fail("ss.info", "no <gameurlN>")
        self.base = bases.get(2) or next(iter(bases.values()))  # eu = 2
        if self.base.endswith("/"):
            self.fail("ss.info", "gameurl base has a trailing slash")
        lw = {}
        for tag in ("LowerAbs", "LowerRel", "UpperAbs", "UpperRel"):
            k = f"<SummonDataCoopMatchingLevel{tag}2>"
            if k in text:
                lw[tag] = text[text.find(k) + len(k):text.find(f"</SummonDataCoopMatchingLevel{tag}2>")]
        self.ok("ss.info", f"<ss>0</ss>, gameurl indices {sorted(bases)}, base {self.base}, level window {lw}")

        r = self.api("/basic_utils/login", {
            "MessageId": "LoginRequest", "PlatformAccountId": self.o.name, "AuthorizationCode": "DUMMY",
            "NatType": 2, "RegionId": 2, "LanguageId": 1, "IssuerId": 1, "ApplicationVersion": 109}, query=False)
        sid, uid = r.get("SessionId"), r.get("UserId")
        if not isinstance(sid, str) or not sid:
            self.fail("login", f"SessionId must be a non-empty string: {r}")
        if not isinstance(uid, int) or uid < 0 or uid > 0x7FFFFFFF:
            self.fail("login", f"UserId must be a non-negative int32: {r}")
        self.session_id, self.user_id = sid, uid
        self.ok("login", f"user {uid}, session {sid}, server version {r.get('ServerVersion')}", user_id=uid)

        r = self.api("/basic_utils/sync_chara_id", {"MessageId": "SyncCharaIdRequest", "CharaIdNum": 1})
        lst = r.get("PublishCharacterIdList")
        if not isinstance(lst, list) or not lst or not isinstance(lst[0], dict) or \
                not isinstance(lst[0].get("PublishCharaId"), int):
            self.fail("sync_chara_id", f"PublishCharacterIdList[0].PublishCharaId missing: {r}")
        self.chara_id = lst[0]["PublishCharaId"]
        self.ok("sync_chara_id", f"chara id {self.chara_id}")

        r = self.api("/basic_utils/get_normal_notice", {"MessageId": "NoticeNormalGetRequest", "Language": 1,
                                                         "Region": 2})
        if not isinstance(r.get("NoticeList"), list):
            self.fail("notices", f"get_normal_notice NoticeList not an array: {r}")
        r = self.api("/basic_utils/get_emergency_notice", {
            "MessageId": "NoticeEmergencyGetRequest", "Language": 1, "Region": 2,
            "CheckTime": time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime())})
        if not isinstance(r.get("NoticeList"), list):
            self.fail("notices", f"get_emergency_notice NoticeList not an array: {r}")
        self.api("/penalty/check_user_priority_move_count", {"MessageId": "UserPropertiesMoveCountCheckRequest",
                                                              "Count": 0})
        self.ok("notices", "normal / emergency notices and move-count check answered")
        self.link.send_state(ST_HOME, self.o.area)

    def udp_and_context(self) -> None:
        o = self.o
        local = "127.0.0.1" if o.host_ip.startswith("127.") else self._local_ip()
        mapped, relay_port = None, 0
        if o.udp:
            if self.udp is None:
                self.udp = Udp(o.udp_port)
            res = self.udp.stun(o.host_ip, o.host_udp_port, hello=True)
            if not res:
                self.fail("stun", f"no STUN answer from {o.host_ip}:{o.host_udp_port} (party UDP port closed?)")
            mapped = (res["addr"], res["port"])
            relay_port = res.get("relay", {}).get("vport", 0)
            if res.get("xor_mismatch"):
                self.fail("stun", f"MAPPED {mapped} != XOR-MAPPED {res['xor_mismatch']}")
            if mapped[1] != self.udp.port:
                self.say(f"stun: mapped port {mapped[1]} != local {self.udp.port} (NAT)")
            self.ok("stun", f"mapped {mapped[0]}:{mapped[1]} (local udp {self.udp.port}), relay vport {relay_port}",
                    relay=res.get("relay"))
        sig_port = self.udp.port if self.udp else (o.udp_port or 9307)
        self.endpoint = {"OnlineId": o.name, "LocalAddr": local, "LocalPort": sig_port,
                         "PublicAddr": mapped[0] if mapped else local, "PublicPort": mapped[1] if mapped else sig_port,
                         "MappedAddr": mapped[0] if mapped else "", "MappedPort": mapped[1] if mapped else 0}
        rq = {"OnlineId": o.name, "SignalingAddr": local, "SignalingPort": sig_port}
        if mapped:
            rq.update({"MappedAddr": mapped[0], "MappedPort": mapped[1]})
        if relay_port:
            rq["RelayPort"] = relay_port
        r = self.call("context_start", rq)
        if r.get("ResKind") != 0 or r.get("OnlineId") != o.name:
            self.fail("context_start", f"{r}")
        self.ok("context_start", f"registered {local}:{sig_port}")

    @staticmethod
    def _local_ip() -> str:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("192.0.2.1", 9))
            ip = s.getsockname()[0]
            s.close()
            return ip
        except OSError:
            return "127.0.0.1"

    def put_sign(self) -> None:
        o = self.o
        pos = (12.5, -3.25, 101.0)
        blob = summon_data(o.name, o.area, pos, o.level, o.region, self.chara_id)
        bad = check_summon_data(blob)
        if bad:
            self.fail("sign", f"own SummonData invalid: {bad}")
        b64 = base64.b64encode(blob).decode()
        r = self.api("/summon_messenger/create", {
            "MessageId": "SummonDataCreateRequest", "ClientSummonTypeList": [SUMMON_TYPE_COOP], "SummonMethod": 0,
            "SummonType": SUMMON_TYPE_COOP, "CharaId": self.chara_id, "NatType": 2, "MatchingLevel": o.level,
            "Region": 2, "RegionFlag": 1, "AreaId": o.area, "AreaRegionId": o.region, "ChannelId": 0,
            "PosX": pos[0], "PosY": pos[1], "PosZ": pos[2], "SummonDataVersion": 3, "SummonData": b64,
            "ClientVersion": 109, "SummonWord": 0, "NaturalEnemy_Vileblood": 0, "NaturalEnemy_VilebloodHunter": 0,
            "NaturalEnemy_BloodHunter": 0, "NaturalEnemy_HunterOfHunter": 0})
        self.sign_id = int(r.get("SummonDataId", 0) or 0)
        self.ok("sign", f"summon_messenger/create OK (sign id {self.sign_id or '?'}, 0xE0-byte SummonData, "
                        f"area 0x{o.area:x}, level {o.level})")

    def poll_signs(self) -> list:
        r = self.api("/summon_messenger/get", {
            "MessageId": "SummonDataGetListRequest",
            "SummonTypeList": [{"SummonType": SUMMON_TYPE_COOP, "GetLimitCount": 5}], "SummonWordMatchingType": 0,
            "AreaId": self.o.area, "ChannelId": 0, "ClientSummonTypeList": [SUMMON_TYPE_COOP], "GetMaxCount": 20,
            "CharaId": self.chara_id, "SummonDataVersion": 3, "NatType": 2, "ClientVersion": 109,
            "MatchingLevel": self.o.level, "PosX": 0, "PosY": 0, "PosZ": 0, "Region": 2, "SummonMethod": 0,
            "AreaRegionId": self.o.region, "DistanceThreshold": 0, "RegionFlag": 1, "SummonWord": 0,
            "UnlockFlagList": [], "CoopOrNaturalEnemyRecruitNum": 1, "IsInvationMultiPlayRequesting": 0})
        lst = r.get("SummonDataList")
        if not isinstance(lst, list):
            self.fail("sign_poll", f"SummonDataList is not an array: {r}")
        for e in lst:
            if not isinstance(e, dict):
                self.fail("sign_poll", f"entry not an object: {e}")
            if e.get("UserId") == self.user_id:
                self.fail("sign_poll", "our own sign is listed back to us")
            if e.get("SummonDataVersion") != 3:
                self.fail("sign_poll", f"SummonDataVersion {e.get('SummonDataVersion')} != 3 (the game skips it)")
            try:
                blob = base64.b64decode(e.get("SummonData", ""))
            except ValueError:
                blob = b""
            if len(blob) != 0xE0:
                self.fail("sign_poll", f"SummonData decodes to {len(blob)} bytes, not 0xE0")
        return lst

    def _event_loop(self) -> None:
        link = self.link
        while not self.stop.is_set() and (link.alive or not link.events.empty()):
            try:
                ev = link.events.get(timeout=0.3)
            except queue.Empty:
                continue
            self.events_seen.append(ev)
            self.j.rec("event", name=ev.get("Name"), event=ev, run=self.tag)
            self.say(f"event {ev.get('Name')}: {ev}")

    def wait_event(self, name: str, timeout: float, start: int = 0, during=None) -> dict | None:
        end = time.monotonic() + timeout
        seen = start
        next_poll = 0.0
        while time.monotonic() < end:
            while seen < len(self.events_seen):
                ev = self.events_seen[seen]
                seen += 1
                if ev.get("Name") == name:
                    return ev
            if not self.link.alive:
                return None
            if during and time.monotonic() >= next_poll:
                next_poll = time.monotonic() + 2.0
                during()
            time.sleep(0.05)
        return None

    def await_invite_and_join(self) -> float:
        o = self.o
        t0 = time.monotonic()
        polls = [0]

        def poll():
            polls[0] += 1
            self.poll_signs()

        ev = self.wait_event("guest_invite", o.invite_timeout, during=poll)
        if not ev:
            self.fail("invite", f"no guest_invite within {o.invite_timeout:.0f} s "
                                f"(link {'up' if self.link.alive else 'down: ' + self.link.closed_reason}; "
                                f"is the host polling summon_messenger/get?)")
        for k in ("RoomId", "HostOnlineId", "HostAddr", "HostPort"):
            if k not in ev:
                self.fail("invite", f"guest_invite lacks {k}: {ev}")
        self.ok("invite", f"guest_invite: room {ev['RoomId']} from {ev['HostOnlineId']} at "
                          f"{ev.get('HostAddr')}:{ev.get('HostPort')} after {time.monotonic() - t0:.1f} s "
                          f"({polls[0]} sign polls OK)")
        self.link.send_state(ST_JOINING)
        if self.host_name and ev["HostOnlineId"] != self.host_name:
            self.fail("invite", f"invite from {ev['HostOnlineId']}, but the party host is {self.host_name}")
        self.host_name = ev["HostOnlineId"]

        # The game's JoinRoom (deliver_invite -> sceNpMatching2JoinRoom).
        r = self.call("join_room", {**self.endpoint, "RoomId": ev["RoomId"]})
        if r.get("ResKind") != 0:
            self.fail("join_room", f"refused: {r.get('Error', r)}")
        self.room, self.room_sid, self.member_id = r["RoomId"], r["SessionId"], r["MemberId"]
        self.members = r.get("Members", [])
        self.owner_id = r.get("OwnerMemberId")
        names = [m.get("OnlineId") for m in self.members]
        if self.host_name not in names:
            self.fail("join_room", f"the host {self.host_name} is not among the members {names}")
        if o.name in names:
            self.fail("join_room", f"our own stale membership is still listed: {names}")
        owner = next((m for m in self.members if m.get("MemberId") == r.get("OwnerMemberId")), None)
        if not owner or owner.get("OnlineId") != self.host_name:
            self.fail("join_room", f"owner member {r.get('OwnerMemberId')} is not the host: {self.members}")
        self.ok("join_room", f"room {self.room} as member {self.member_id} (owner {r.get('OwnerMemberId')}), "
                             f"members {names}, max {r.get('MaxMembers')}")

        # Signaling: the game resolves the host and opens P2P to it.
        rs = self.call("signaling_resolve", {"OnlineId": self.host_name})
        if rs.get("ResKind") != 0 or not rs.get("Addr") or not rs.get("Port"):
            self.fail("signaling_resolve", f"{rs}")
        self.host_addr = (rs["Addr"], int(rs["Port"]))
        extra = ""
        if self.udp:
            n = self.udp.probe(*self.host_addr)
            extra = f"; {n} probes fe 'bbhp' sent"
            res = self.udp.stun(o.host_ip, o.host_udp_port, hello=True)  # keepalive / relay refresh
            if not res:
                self.fail("signaling_resolve", "STUN keepalive unanswered after joining")
        self.ok("signaling_resolve", f"{self.host_name} at {rs['Addr']}:{rs['Port']} (local {rs.get('LocalAddr')}:"
                                     f"{rs.get('LocalPort')}, mapped {rs.get('MappedAddr')}:{rs.get('MappedPort')}, "
                                     f"relay {rs.get('RelayPort')}){extra}")

        hb = self.call("heartbeat", {"SessionId": self.room_sid, "MemberId": self.member_id})
        if hb.get("InRoom") != 1:
            self.fail("heartbeat", f"InRoom != 1 right after joining: {hb}")
        self.link.send_state(ST_IN_HOST, o.area)
        me = o.name
        if not self.link.wait_roster(lambda ros: any(e["name"] == me and e["state"] == "in_host_world" and
                                                     e["connected"] for e in ros), 6.0):
            self.fail("roster", f"the host's roster never showed us in its world: {self.link.roster}")
        dup = [e for e in self.link.roster if e["name"] == me]
        if len(dup) != 1:
            self.fail("roster", f"{len(dup)} roster entries named {me}: {self.link.roster}")
        self.ok("roster", f"host roster: {[(e['name'], e['slot'], e['state'], e['ping_ms']) for e in self.link.roster]}")
        self.in_room.set()
        threading.Thread(target=self._room_loop, daemon=True).start()
        return time.monotonic() - t0

    def _room_loop(self) -> None:
        """While in the room: Matching2 heartbeats (5 s), STUN keepalive (10 s), probes."""
        n = 0
        while not self.stop.wait(1.0):
            if not self.link.alive:
                return
            n += 1
            if n % 5 == 0 and self.room_sid:
                try:
                    hb = self.call("heartbeat", {"SessionId": self.room_sid, "MemberId": self.member_id})
                    if hb.get("InRoom") == 1:
                        self.hb_ok += 1
                    else:
                        self.hb_fail += 1
                        self.j.rec("warn", msg=f"heartbeat: not in room: {hb}", run=self.tag)
                except StepFail as e:
                    self.hb_fail += 1
                    self.j.rec("warn", msg=f"heartbeat failed: {e}", run=self.tag)
            if self.udp and n % 10 == 0:
                self.udp.stun(self.o.host_ip, self.o.host_udp_port, hello=True, tries=1)
                if self.host_addr:
                    self.udp.probe(*self.host_addr, n=1)

    def hold(self, seconds: float) -> None:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if not self.link.alive:
                self.fail("hold", f"link lost while in the room: {self.link.closed_reason}")
            time.sleep(0.2)
        if self.hb_fail:
            self.fail("hold", f"{self.hb_fail} heartbeat(s) failed / not in room")
        rtt = self.link.rtt_ms[-10:]
        self.ok("hold", f"{seconds:.0f} s in the room: {self.hb_ok} heartbeats, {self.link.host_pings} host pings, "
                        f"rtt {min(rtt) if rtt else '-'}..{max(rtt) if rtt else '-'} ms")

    def leave(self) -> None:
        """A graceful exit: LeaveRoom, delete the sign, BYE."""
        self.stop.set()
        if self.room_sid and self.link.alive:
            self.call("leave_room", {"SessionId": self.room_sid, "MemberId": self.member_id})
            self.api("/summon_messenger/delete", {"MessageId": "SummonDataRemoveRequest", "CharaId": self.chara_id})
        self.link.bye()
        if self.udp:
            self.udp.close()
        self.ok("leave", "leave_room, summon_messenger/delete, BYE")

    def crash(self) -> None:
        self.stop.set()
        self.link.crash()
        if self.udp:
            self.udp.close()
        self.ok("crash", "the guest process died: TCP reset, no BYE, no LeaveRoom, UDP gone")

    def close_quietly(self) -> None:
        self.stop.set()
        if self.link and self.link.alive:
            try:
                self.link.bye()
            except OSError:
                pass
        if self.udp:
            self.udp.close()

    def spoof(self, victim: str) -> None:
        """Impersonation probes (the host must answer each with ResKind 7 and change nothing):
        context_start / signaling_update / join_room / create_room naming `victim`, and
        kick_member / leave_room / heartbeat acting as the room owner's member id."""
        ep = {"SignalingAddr": "6.6.6.6", "SignalingPort": 666}
        before = self.call("signaling_resolve", {"OnlineId": victim})
        probes = [
            ("context_start", {"OnlineId": victim, **ep}),
            ("signaling_update", {"OnlineId": victim, "MappedAddr": "6.6.6.6", "MappedPort": 666}),
            ("join_room", {**self.endpoint, "OnlineId": victim, "RoomId": self.room}),
            ("create_room", {"OnlineId": victim, "MaxMembers": 2}),
            ("kick_member", {"SessionId": self.room_sid, "MemberId": self.member_id,
                             "KickerMemberId": self.owner_id, "OptData": ""}),
            ("heartbeat", {"SessionId": self.room_sid, "MemberId": self.owner_id}),
            ("leave_room", {"SessionId": self.room_sid, "MemberId": self.owner_id}),
        ]
        for kind, rq in probes:
            r = self.call(kind, rq)
            if r.get("ResKind") != 7:
                self.fail("spoof", f"{kind} as {victim} / member {self.owner_id} was not refused: {r}")
        after = self.call("signaling_resolve", {"OnlineId": victim})
        if (before.get("Addr"), before.get("Port")) != (after.get("Addr"), after.get("Port")):
            self.fail("spoof", f"{victim}'s address changed: {before} -> {after}")
        hb = self.call("heartbeat", {"SessionId": self.room_sid, "MemberId": self.member_id})
        if hb.get("ResKind") != 0 or hb.get("InRoom") != 1:
            self.fail("spoof", f"no longer in the room after the probes: {hb}")
        self.ok("spoof", f"{len(probes)} impersonation probes as {victim} refused (ResKind 7); room intact")

    def full_join(self) -> float:
        t0 = time.monotonic()
        self.connect()
        self.boot_online()
        self.udp_and_context()
        self.put_sign()
        self.await_invite_and_join()
        return time.monotonic() - t0


# ==================================================================================================
# Scripts
# ==================================================================================================

def script_join(o, key, j) -> tuple[bool, str]:
    g = Guest(o, key, j)
    try:
        dt = g.full_join()
        if o.spoof_online_id:
            g.spoof(o.spoof_online_id)
        if o.hold:
            g.hold(o.hold)
        if not o.stay:
            g.leave()
        return True, f"slot {g.link.slot}, user {g.user_id}, room {g.room} member {g.member_id} in {dt:.1f} s"
    finally:
        if not o.stay:
            g.close_quietly()


def script_crash_rejoin(o, key, j) -> tuple[bool, str]:
    a = Guest(o, key, j, tag="#1")
    a.full_join()
    if o.hold:
        a.hold(o.hold)
    slot, uid, room = a.link.slot, a.user_id, a.room
    token = a.token
    a.crash()
    t_crash = time.monotonic()
    time.sleep(o.crash_delay)
    b = Guest(o, key, j, token=token if o.keep_token else b"\0" * 16, tag="#2")
    try:
        # The restarted game reconnects (backoff 1, 2, 4 .. s) until the host takes it back.
        deadline = t_crash + o.rejoin_budget
        tries = 0
        while True:
            tries += 1
            try:
                b.connect()
                break
            except StepFail as e:
                if time.monotonic() > deadline or (b.link and b.link.reject and b.link.reject[0] not in (5,)):
                    raise
                print(f"  ..   reconnect attempt {tries} refused ({e}); retrying", flush=True)
                time.sleep(min(2 ** (tries - 1), 4))
        if b.link.slot != slot:
            b.fail("rejoin", f"came back in slot {b.link.slot}, not our kept slot {slot}")
        if o.keep_token and not b.link.resumed:
            b.fail("rejoin", "resume token not honoured (WELCOME resumed = 0)")
        b.boot_online()
        if b.user_id != uid:
            b.fail("rejoin", f"login gave user {b.user_id}, before the crash {uid}")
        b.udp_and_context()
        b.put_sign()
        b.await_invite_and_join()
        took = time.monotonic() - t_crash
        if b.room != room:
            b.ok("rejoin", f"note: a new room {b.room} (was {room})")
        if took > o.rejoin_budget:
            b.fail("rejoin", f"back in the host's room after {took:.1f} s > budget {o.rejoin_budget:.0f} s")
        if o.hold:
            b.hold(o.hold)
        b.ok("rejoin", f"back in slot {slot}, room {b.room} as member {b.member_id} {took:.1f} s after the crash "
                       f"({tries} connect attempt(s))")
        if not o.stay:
            b.leave()
        return True, f"crashed in slot {slot} / room {room}, back in slot {b.link.slot} / room {b.room} " \
                     f"{took:.1f} s later (budget {o.rejoin_budget:.0f} s)"
    finally:
        if not o.stay:
            b.close_quietly()


def script_soak(o, key, j, rounds: int) -> tuple[bool, str]:
    fails = []
    times = []
    prev_slot = None
    crashed = False
    for i in range(rounds):
        how = "crash" if i % 2 else "leave"
        print(f"--- soak round {i + 1}/{rounds} (ends with {how})", flush=True)
        g = Guest(o, key, j, tag=f"#{i + 1}")
        t_start = time.monotonic()
        try:
            deadline = time.monotonic() + o.rejoin_budget
            while True:
                try:
                    g.connect()
                    break
                except StepFail:
                    if time.monotonic() > deadline or (g.link and g.link.reject and g.link.reject[0] != 5):
                        raise
                    time.sleep(1.0)
            if crashed and prev_slot is not None and g.link.slot != prev_slot:
                g.fail("rejoin", f"slot {g.link.slot} after a crash in slot {prev_slot}")
            g.boot_online()
            g.udp_and_context()
            g.put_sign()
            g.await_invite_and_join()
            times.append(time.monotonic() - t_start)
            g.hold(o.hold or 3.0)
            prev_slot = g.link.slot
            if how == "crash":
                g.crash()
                crashed = True
                time.sleep(o.crash_delay)
            else:
                g.leave()
                crashed = False
                time.sleep(0.5)
            print(f"PASS soak round {i + 1}", flush=True)
            j.verdict(f"soak round {i + 1}", True, how)
        except StepFail as e:
            fails.append(f"round {i + 1}: {e}")
            print(f"FAIL soak round {i + 1}: {e}", flush=True)
            j.verdict(f"soak round {i + 1}", False, str(e))
            g.close_quietly()
            crashed = False
            time.sleep(1.0)
    if fails:
        return False, f"{len(fails)}/{rounds} rounds failed: " + "; ".join(fails[:3])
    return True, f"{rounds} rounds, join {min(times):.1f}..{max(times):.1f} s"


# ==================================================================================================
# main
# ==================================================================================================

def read_identity_file(path: str, item_key: str) -> tuple[bytes, list[str]] | None:
    """party_link.cpp parse_identity_file: "hash <64 hex>" and "<item_key> <name>" lines."""
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return None
    h, names = None, []
    for line in text.split("\n"):
        line = line.rstrip("\r ")
        if not line or line.startswith("#") or " " not in line:
            continue
        k, v = line.split(" ", 1)
        if k == "hash":
            try:
                h = bytes.fromhex(v)
            except ValueError:
                return None
            if len(h) != 32:
                return None
        elif k == item_key and v:
            names.append(v)
    return (h, names) if h else None


def game_identity(data_dir: str) -> tuple[bytes, list[str], bytes, list[str]]:
    """party_runtime.cpp patch_identity / mod_identity with default env: (patches hash, patch names,
    mods hash, mod names) from <data dir>/out."""
    out = os.path.join(data_dir, "out")
    patches, patch_names = b"\0" * 32, []
    got = read_identity_file(os.path.join(out, "party_patch_hash.txt"), "patch")
    if got:
        patches, patch_names = got
    elif os.path.isfile(os.path.join(out, "patches.bin")):
        with open(os.path.join(out, "patches.bin"), "rb") as f:
            data = f.read()
        patches = hashlib.blake2b(b"bbparty-patches-bin\0" + data, digest_size=32).digest()
        patch_names = ["patches.bin (all patches: no party_patch_hash.txt)"]
    mods, mod_names = b"\0" * 32, []
    got = read_identity_file(os.path.join(out, "party_mods.txt"), "mod")
    if got:
        mods, mod_names = got
    return patches, patch_names, mods, mod_names


def read_party_identity(path: str) -> dict:
    """<user dir>/party_identity.txt written by party_runtime.cpp write_identity_file (A9): every
    field a real game's HELLO carries, so a simulated member passes the version check."""
    ident: dict = {"hook_names": [], "patch_names": [], "mod_names": []}
    for line in open(path, encoding="utf-8", errors="replace").read().split("\n"):
        line = line.rstrip("\r")
        if not line or line.startswith("#") or " " not in line:
            continue
        k, v = line.split(" ", 1)
        if k in ("build", "rules"):
            ident[k] = v
        elif k in ("eboot", "params", "patches", "mods", "hooks"):
            ident[k] = bytes.fromhex(v)
        elif k in ("hook", "patch", "mod"):
            ident[k + "_names"].append(v)
    return ident


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Headless simulated party guest (see the module docstring).",
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--code", help="party code BBP3-... (or host:port)")
    ap.add_argument("--host", help="host address (instead of --code)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help="host party port (TCP; UDP the same)")
    ap.add_argument("--udp-port-host", type=int, default=0, help="host party UDP port if not the TCP port")
    ap.add_argument("--password", default="", help="BB_PARTY_PASSWORD of the host")
    ap.add_argument("--secret", default="", help="16 hex digits: the code secret (with --host)")
    ap.add_argument("--key-hex", default="", help="the derived party key (skips Argon2i)")
    ap.add_argument("--name", default="SimGuest", help="party / online name (1-16 of A-Za-z0-9_-)")
    ap.add_argument("--script", nargs="+", default=["join"], metavar="SCRIPT",
                    help="join | crash-rejoin | soak N")
    ap.add_argument("--game-dir", help="the game's identity like party_runtime: DIR/eboot.bin sha256, and the "
                                       "gameplay patches / mods from <data dir>/out/party_patch_hash.txt, "
                                       "party_mods.txt (patches.bin)")
    ap.add_argument("--data-dir", help="BB_DATA_DIR for the patch / mod identity (default: --game-dir)")
    ap.add_argument("--eboot-sha256", default="", help="64 hex digits (default zeros)")
    ap.add_argument("--patches-hash", default="", help="64 hex digits (default zeros)")
    ap.add_argument("--mods-hash", default="", help="64 hex digits (default zeros)")
    ap.add_argument("--identity", help="a game's party_identity.txt (user dir): send its whole version-check "
                                       "identity (A9), e.g. to join a real game host")
    ap.add_argument("--build-id", default=None, help="A9 bbport build id to send (default: none)")
    ap.add_argument("--rules", default=None, help="party rules to send (\"max 4 players, 4p:...\")")
    ap.add_argument("--protocol", type=int, default=PROTOCOL_VERSION, help="PartyLink version to speak (1 or 2)")
    ap.add_argument("--no-udp", dest="udp", action="store_false", help="skip STUN and probes")
    ap.add_argument("--udp-port", type=int, default=0, help="our UDP port (0 = any)")
    ap.add_argument("--area", type=lambda s: int(s, 0), default=0x18010000, help="AreaId (default 0x18010000)")
    ap.add_argument("--region", type=int, default=1)
    ap.add_argument("--level", type=int, default=50)
    ap.add_argument("--invite-timeout", type=float, default=30.0)
    ap.add_argument("--hold", type=float, default=0.0, help="seconds to stay in the room after joining")
    ap.add_argument("--crash-delay", type=float, default=2.0, help="seconds between the crash and the restart")
    ap.add_argument("--rejoin-budget", type=float, default=60.0, help="crash -> back in the room, seconds")
    ap.add_argument("--keep-token", action="store_true", help="the restart reuses the resume token")
    ap.add_argument("--lost-timeout", type=float, default=10.0)
    ap.add_argument("--spoof-online-id", default="", metavar="NAME",
                    help="join, then send RPCs claiming to be NAME / the room owner: each must be refused")
    ap.add_argument("--stay", action="store_true", help="do not leave at the end (until Ctrl+C)")
    ap.add_argument("--json-log", default=None, help="JSON log path (default simguest-<name>.json, - = none)")
    ap.add_argument("-v", "--verbose", action="store_true")
    o = ap.parse_args(argv)

    script = o.script[0]
    if script not in ("join", "crash-rejoin", "soak"):
        ap.error("--script: join | crash-rejoin | soak N")
    rounds = 0
    if script == "soak":
        try:
            rounds = int(o.script[1])
        except (IndexError, ValueError):
            ap.error("--script soak N")
    if not (1 <= len(o.name) <= 16 and all(c.isalnum() or c in "_-" for c in o.name) and o.name.isascii()):
        ap.error("--name: 1-16 of A-Z a-z 0-9 _ -")

    secret = b"\0" * 8
    if o.code:
        try:
            pc = decode_party_code(o.code)
        except ValueError as e:
            ap.error(f"--code: {e}")
        o.host_ip, o.host_port, secret = pc["host"], pc["port"], pc["secret"]
        if pc["flags"] & 2 and not o.password:
            print("simguest: note: the code says the host set a password (--password)", file=sys.stderr)
    elif o.host:
        o.host_ip, o.host_port = o.host, o.port
    else:
        ap.error("--code or --host")
    if o.secret:
        secret = bytes.fromhex(o.secret)
        if len(secret) != 8:
            ap.error("--secret: 16 hex digits")
    try:
        o.host_ip = socket.gethostbyname(o.host_ip)
    except OSError as e:
        ap.error(f"cannot resolve {o.host_ip}: {e}")
    o.host_udp_port = o.udp_port_host or o.host_port

    o.eboot = o.mods = o.patches = b"\0" * 32
    o.patch_names, o.mod_names = [], []
    if o.game_dir:
        with open(os.path.join(o.game_dir, "eboot.bin"), "rb") as f:
            o.eboot = hashlib.sha256(f.read()).digest()
        o.patches, o.patch_names, o.mods, o.mod_names = game_identity(o.data_dir or o.game_dir)
    if o.eboot_sha256:
        o.eboot = bytes.fromhex(o.eboot_sha256)
    if o.patches_hash:
        o.patches = bytes.fromhex(o.patches_hash)
    if o.mods_hash:
        o.mods = bytes.fromhex(o.mods_hash)
    o.ident = {}
    if o.identity:
        o.ident = read_party_identity(o.identity)
        o.eboot = o.ident.get("eboot", o.eboot)
        o.patches, o.mods = o.ident.get("patches", o.patches), o.ident.get("mods", o.mods)
        o.patch_names, o.mod_names = o.ident["patch_names"], o.ident["mod_names"]
    if o.build_id is not None:
        o.ident["build"] = o.build_id
    if o.rules is not None:
        o.ident["rules"] = o.rules
    if len(o.eboot) != 32 or len(o.mods) != 32 or len(o.patches) != 32:
        ap.error("--eboot-sha256 / --patches-hash / --mods-hash: 64 hex digits")

    log_path = o.json_log if o.json_log is not None else f"simguest-{o.name}.json"
    if log_path == "-":
        log_path = None
    shown = {k: v for k, v in vars(o).items() if k not in ("password", "key_hex", "eboot", "mods", "patches", "ident")}
    shown["password_set"] = bool(o.password)
    shown["eboot"], shown["mods"], shown["patches"] = o.eboot.hex(), o.mods.hex(), o.patches.hex()
    j = JsonLog(log_path, shown)

    t = time.monotonic()
    if o.key_hex:
        key, how = bytes.fromhex(o.key_hex), "given"
    else:
        key, how = derive_party_key(o.password, secret)
    msg = f"party key: Argon2i via {how} in {time.monotonic() - t:.1f} s"
    print(f"simguest: {msg}; host {o.host_ip}:{o.host_port}, name {o.name}, script {' '.join(o.script)}",
          flush=True)
    if o.verbose:
        print(f"simguest: key {key.hex()} (pass --key-hex to skip the derivation)", file=sys.stderr)
    j.rec("key", msg=msg)

    label = " ".join(o.script)
    try:
        if script == "join":
            ok, why = script_join(o, key, j)
        elif script == "crash-rejoin":
            ok, why = script_crash_rejoin(o, key, j)
        else:
            ok, why = script_soak(o, key, j, rounds)
    except StepFail as e:
        ok, why = False, str(e)
    except KeyboardInterrupt:
        ok, why = False, "interrupted"
    except Exception as e:  # a bug here should still give a verdict and the log
        import traceback

        traceback.print_exc()
        ok, why = False, f"internal error: {e!r}"
    if ok and o.stay:
        print("simguest: staying (Ctrl+C to quit)", flush=True)
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass
    print(f"{'PASS' if ok else 'FAIL'} {label}: {why}", flush=True)
    j.verdict(label, ok, why)
    try:
        j.save()
        if log_path:
            print(f"simguest: log {os.path.abspath(log_path)}", flush=True)
    except OSError as e:
        print(f"simguest: cannot write the log: {e}", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
