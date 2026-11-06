// SPDX-License-Identifier: GPL-3.0-or-later
#include "emevd.h"

namespace bbdata {

Emevd ParseEmevd(Bytes data) {
    Emevd out;
    out.buf = std::move(data);
    const Span b(out.buf);
    if (!b.Tag(0, "EVD\0") || b.Le<std::uint32_t>(8) != 0xCC) {
        throw FormatError("not a Bloodborne EMEVD");
    }
    std::int64_t h[16];
    for (int i = 0; i < 16; ++i) {
        h[i] = b.Le<std::int64_t>(0x10 + i * 8);
    }
    const std::int64_t ev_n = h[0], ev_o = h[1], in_o = h[3], pa_o = h[9], args_o = h[13];
    if (ev_n < 0 || ev_n > (1 << 24)) {
        throw FormatError("EMEVD event count implausible");
    }
    out.events.reserve(static_cast<std::size_t>(ev_n));
    for (std::int64_t i = 0; i < ev_n; ++i) {
        const std::int64_t eo = ev_o + i * 0x30;
        EvEvent e;
        e.id = b.Le<std::int64_t>(eo);
        const std::int64_t icnt = b.Le<std::int64_t>(eo + 8), ioff = b.Le<std::int64_t>(eo + 16);
        const std::int64_t pcnt = b.Le<std::int64_t>(eo + 24), poff = b.Le<std::int64_t>(eo + 32);
        e.rest = b.Le<std::uint32_t>(eo + 40);
        if (icnt < 0 || icnt > (1 << 24) || pcnt < 0 || pcnt > (1 << 24)) {
            throw FormatError("EMEVD instruction count implausible");
        }
        e.ins.reserve(static_cast<std::size_t>(icnt));
        for (std::int64_t k = 0; k < icnt; ++k) {
            const std::int64_t io = in_o + ioff + k * 0x20;
            EvInstr in;
            in.bank = b.Le<std::uint32_t>(io);
            in.id = b.Le<std::uint32_t>(io + 4);
            const std::int64_t alen = b.Le<std::int64_t>(io + 8), aoff = b.Le<std::int64_t>(io + 16);
            const Span s = b.Slice(args_o + aoff, args_o + aoff + alen);  // clamped, as Python's slice
            in.off = static_cast<std::uint32_t>(s.p - b.p);
            in.len = static_cast<std::uint32_t>(s.n);
            e.ins.push_back(in);
        }
        e.params.reserve(static_cast<std::size_t>(pcnt));
        for (std::int64_t k = 0; k < pcnt; ++k) {
            const std::int64_t po = pa_o + poff + k * 0x20;
            EvParam p;
            p.instr = b.Le<std::int64_t>(po);
            p.target = b.Le<std::int64_t>(po + 8);
            p.source = b.Le<std::int64_t>(po + 16);
            p.count = b.Le<std::int32_t>(po + 24);
            e.params.push_back(p);
        }
        out.events.push_back(std::move(e));
    }
    return out;
}

Emedf ParseEmedf(Span b) {
    if (!b.Tag(0, "EDF\0")) {
        throw FormatError("not an EMEDF");
    }
    std::int64_t hdr[10][2];
    for (int i = 0; i < 10; ++i) {
        hdr[i][0] = b.Le<std::int64_t>(0x10 + i * 16);
        hdr[i][1] = b.Le<std::int64_t>(0x18 + i * 16);
    }
    const std::int64_t ncls = hdr[0][0], ocls = hdr[0][1], oins = hdr[1][1], oarg = hdr[2][1], ostr = hdr[8][1];
    if (ncls < 0 || ncls > (1 << 20)) {
        throw FormatError("EMEDF class count implausible");
    }
    Emedf out;
    for (std::int64_t c = 0; c < ncls; ++c) {
        const std::int64_t co = ocls + c * 32;
        const std::int64_t bank = b.Le<std::int64_t>(co), ni = b.Le<std::int64_t>(co + 8), io = b.Le<std::int64_t>(co + 16);
        if (ni < 0 || ni > (1 << 20)) {
            throw FormatError("EMEDF instruction count implausible");
        }
        for (std::int64_t i = 0; i < ni; ++i) {
            const std::int64_t o = oins + io + i * 32;
            const std::int64_t iid = b.Le<std::int64_t>(o), na = b.Le<std::int64_t>(o + 8), ao = b.Le<std::int64_t>(o + 16);
            if (na < 0 || na > (1 << 16)) {
                throw FormatError("EMEDF argument count implausible");
            }
            EmedfEntry ent;
            for (std::int64_t a = 0; a < na; ++a) {
                const std::int64_t fo = oarg + ao + a * 64;
                EmedfArg arg;
                arg.name = Utf16z(b, ostr + b.Le<std::int64_t>(fo));
                arg.type = static_cast<std::uint32_t>(b.Le<std::int64_t>(fo + 8) & 0xffffffff);
                ent.args.push_back(std::move(arg));
            }
            out.map_[Emedf::Key(static_cast<std::uint64_t>(bank) & 0xffffffffu, static_cast<std::uint64_t>(iid))] = std::move(ent);
        }
    }
    return out;
}

std::unordered_map<std::int64_t, std::string> ParseEmeld(Span b) {
    std::unordered_map<std::int64_t, std::string> out;
    if (!b.Tag(0, "ELD\0")) {
        return out;
    }
    const std::int64_t cnt = b.Le<std::int64_t>(0x10), off = b.Le<std::int64_t>(0x18), so = b.Le<std::int64_t>(0x48);
    if (cnt < 0 || cnt > (1 << 24)) {
        throw FormatError("EMELD count implausible");
    }
    for (std::int64_t i = 0; i < cnt; ++i) {
        const std::int64_t eid = b.Le<std::int64_t>(off + i * 16), no = b.Le<std::int64_t>(off + i * 16 + 8);
        out[eid] = Utf16z(b, so + no);
    }
    return out;
}

std::vector<std::int64_t> UnpackArgs(const EmedfEntry& def, Span raw) {
    std::vector<std::int64_t> vals;
    std::size_t pos = 0;
    for (const EmedfArg& a : def.args) {
        std::size_t sz = 4;
        switch (a.type) {
            case 0: case 3: sz = 1; break;
            case 1: case 4: sz = 2; break;
            case 8: sz = 8; break;
            default: sz = 4; break;
        }
        pos = (pos + sz - 1) / sz * sz;
        if (pos + sz > raw.n) {
            break;
        }
        const std::uint8_t* p = raw.p + pos;
        std::int64_t v = 0;
        switch (a.type) {
            case 0: v = p[0]; break;
            case 1: v = p[0] | p[1] << 8; break;
            case 2: { std::uint32_t x; std::memcpy(&x, p, 4); v = x; break; }
            case 3: v = static_cast<std::int8_t>(p[0]); break;
            case 4: { std::int16_t x; std::memcpy(&x, p, 2); v = x; break; }
            case 8: { std::uint64_t x; std::memcpy(&x, p, 8); v = static_cast<std::int64_t>(x); break; }
            default: { std::int32_t x; std::memcpy(&x, p, 4); v = x; break; }  // s32 (5), f32 bits (6), unknown
        }
        vals.push_back(v);
        pos += sz;
    }
    return vals;
}

}  // namespace bbdata
