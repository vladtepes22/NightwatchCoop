"""The one rule for hiding network addresses in logs, reports and overlay text (0.8.1, NW-019b146).

  1.2.3.4            -> 1.2.x.x                   (the harness's mask_ipv4 form: the network stays readable)
  2001:db8:a:b::1    -> 2001:db8:x:x:x:x:x:x      (the first two groups; less before a "::")
  127.x.x.x, 0.0.0.0, 255.255.255.255, ::1, ::   stay (no one's address)

src/runtime_ipmask.c does the same for the game's own log output (tests/ipmask_vectors.txt pins both).
mask_ips() is idempotent. unmasked_ips() lists what a scan would still call an address (tests/test_ip_masking.py).
"""
import re
import sys

_V4 = re.compile(r"(?<![\w.])(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?![\w.]*\d)")
_V6 = re.compile(r"(?<![\w:])(?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f]{0,4}(?![\w:])")
_HEX = re.compile(r"[0-9A-Fa-f]{1,4}")


def _v4(m):
    o = [int(g) for g in m.groups()]
    if any(x > 255 for x in o):
        return m.group(0)
    if o[0] == 127 or o == [0, 0, 0, 0] or o == [255, 255, 255, 255]:
        return m.group(0)
    return "%s.%s.x.x" % (m.group(1), m.group(2))


def _v6_ok(s):
    groups = [g for g in s.split(":") if g]
    if not (len(groups) == 8 or ("::" in s and len(groups) >= 2)):
        return None
    if not all(_HEX.fullmatch(g) for g in groups):
        return None
    return groups


def _v6(m):
    s = m.group(0)
    if _v6_ok(s) is None:
        return s
    head = s.split("::")[0] if "::" in s else s
    lead = [g for g in head.split(":") if g][:2]
    return ":".join(lead + ["x"] * (8 - len(lead)))


def mask_ips(text):
    """text with every IPv4 and IPv6 address masked."""
    if not text:
        return text
    return _V6.sub(_v6, _V4.sub(_v4, text))


def unmasked_ips(text):
    """The addresses mask_ips() would still change (loopback and unspecified ones excluded)."""
    out = []
    for m in _V4.finditer(text):
        if _v4(m) != m.group(0):
            out.append(m.group(0))
    for m in _V6.finditer(text):
        if _v6(m) != m.group(0):
            out.append(m.group(0))
    return out


if __name__ == "__main__":  # python scripts/ipmask.py < log > masked
    sys.stdout.write(mask_ips(sys.stdin.read()))
