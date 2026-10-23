"""Dumps the instructions of the given events: index, bank[id], args as i32 words and bytes."""
import struct, sys, zlib

def load(path):
    raw = open(path, "rb").read()
    return zlib.decompress(raw[0x4c:]) if raw[:4] == b"DCX\0" else raw

def main(path, *events):
    d = load(path)
    q = lambda o: struct.unpack_from("<q", d, o)[0]
    ev_n, ev_off, ins_off, args_off = q(0x10), q(0x18), q(0x28), q(0x78)
    par_off = q(0x58)
    want = {int(e) for e in events}
    for e in range(ev_n):
        eo = ev_off + e * 0x30
        eid, n, io, pn, po, rest = q(eo), q(eo + 8), q(eo + 0x10), q(eo + 0x18), q(eo + 0x20), struct.unpack_from("<I", d, eo + 0x28)[0]
        if eid not in want:
            continue
        print(f"== event {eid} rest {rest} params {pn}")
        for i in range(n):
            o = ins_off + io + i * 0x20
            bank, iid = struct.unpack_from("<ii", d, o)
            alen, aoff = q(o + 8), q(o + 0x10)
            a = d[args_off + aoff: args_off + aoff + alen]
            words = [struct.unpack_from("<i", a, k)[0] for k in range(0, len(a) - len(a) % 4, 4)]
            print(f"  [{i:>3}] {bank}[{iid:02d}] {words}  {a.hex()}")
        for k in range(pn):
            p = par_off + po + k * 0x20
            ii, dst, src, ln = q(p), q(p + 8), q(p + 0x10), struct.unpack_from("<i", d, p + 0x18)[0]
            print(f"  param: instr {ii} arg+{dst} <- event param +{src} ({ln} bytes)")

main(*sys.argv[1:])
