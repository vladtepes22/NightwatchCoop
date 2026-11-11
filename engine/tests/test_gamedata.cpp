// SPDX-License-Identifier: GPL-3.0-or-later
// gpu/shim/gamedata: the readers (DCX, BND4, PARAM + PARAMDEF, EMEDF / EMEVD / EMELD, MSB) and the table
// derivation, on SYNTHETIC data built here (no game bytes). The equality with the Python oracle on a real game
// is tests/test_party_tables.py. cmake --build out/gpu --target gamedata-test
#include "gamedata/build.h"
#include "gamedata/container.h"
#include "gamedata/derive.h"
#include "gamedata/events.h"
#include "gamedata/msb.h"
#include "gamedata/param.h"
#include "party/party_gamedata.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace bbdata;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

template <class T>
void Put(Bytes& b, std::size_t at, T v) {
    if (b.size() < at + sizeof(T)) b.resize(at + sizeof(T), 0);
    std::memcpy(b.data() + at, &v, sizeof(T));
}
void PutStr16(Bytes& b, std::size_t at, const std::string& s) {
    // UTF-8 (BMP only) to UTF-16LE
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t u = c;
        std::size_t len = 1;
        if (c >= 0xE0) {
            u = (c & 0x0Fu) << 12 | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6 | (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            len = 3;
        } else if (c >= 0xC0) {
            u = (c & 0x1Fu) << 6 | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            len = 2;
        }
        Put<std::uint16_t>(b, at + n * 2, static_cast<std::uint16_t>(u));
        ++n;
        i += len;
    }
    Put<std::uint16_t>(b, at + n * 2, 0);
}

struct DefField {
    std::string type, name;
    std::uint32_t size;
};

Bytes MakeParamdef(const std::vector<DefField>& fields) {
    Bytes b(0x38 + fields.size() * 0xd0, 0);
    Put<std::uint16_t>(b, 8, static_cast<std::uint16_t>(fields.size()));
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const std::size_t o = 0x38 + i * 0xd0;
        std::memcpy(b.data() + o + 0x70, fields[i].type.data(), fields[i].type.size());
        std::memcpy(b.data() + o + 0x90, fields[i].name.data(), fields[i].name.size());
        Put<std::uint32_t>(b, o + 0x64, fields[i].size);
    }
    return b;
}

// rows: {id, row bytes}
Bytes MakeParam(const std::vector<std::pair<std::int32_t, Bytes>>& rows) {
    Bytes b(0x40 + rows.size() * 0x18, 0);
    Put<std::uint16_t>(b, 0x0a, static_cast<std::uint16_t>(rows.size()));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const std::size_t at = b.size();
        b.insert(b.end(), rows[i].second.begin(), rows[i].second.end());
        Put<std::int32_t>(b, 0x40 + i * 0x18, rows[i].first);
        Put<std::int64_t>(b, 0x40 + i * 0x18 + 8, static_cast<std::int64_t>(at));
    }
    return b;
}

Bytes MakeBnd4(const std::vector<std::pair<std::string, Bytes>>& entries) {
    const std::size_t esz = 0x24;
    Bytes b(0x40 + entries.size() * esz, 0);
    std::memcpy(b.data(), "BND4", 4);
    Put<std::uint32_t>(b, 0x0c, static_cast<std::uint32_t>(entries.size()));
    Put<std::uint64_t>(b, 0x20, esz);
    Put<std::uint8_t>(b, 0x30, 1);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::size_t name_at = b.size();
        b.resize(name_at + (entries[i].first.size() + 1) * 2, 0);
        PutStr16(b, name_at, entries[i].first);
        const std::size_t data_at = b.size();
        b.insert(b.end(), entries[i].second.begin(), entries[i].second.end());
        const std::size_t o = 0x40 + i * esz;
        Put<std::int64_t>(b, o + 8, static_cast<std::int64_t>(entries[i].second.size()));
        Put<std::uint32_t>(b, o + 0x18, static_cast<std::uint32_t>(data_at));
        Put<std::uint32_t>(b, o + 0x20, static_cast<std::uint32_t>(name_at));
    }
    return b;
}

struct SynInstr {
    std::uint32_t bank, id;
    Bytes args;
};
struct SynParam {
    std::int64_t instr, target, source;
    std::int32_t count;
};
struct SynEvent {
    std::int64_t id;
    std::uint32_t rest;
    std::vector<SynInstr> ins;
    std::vector<SynParam> params;
};

Bytes MakeEmevd(const std::vector<SynEvent>& events) {
    Bytes b(0x100, 0);
    std::memcpy(b.data(), "EVD\0", 4);
    Put<std::uint32_t>(b, 8, 0xCC);
    std::size_t n_ins = 0, n_par = 0;
    for (const SynEvent& e : events) n_ins += e.ins.size(), n_par += e.params.size();
    const std::size_t ev_o = b.size();
    b.resize(ev_o + events.size() * 0x30);
    const std::size_t in_o = b.size();
    b.resize(in_o + n_ins * 0x20);
    const std::size_t pa_o = b.size();
    b.resize(pa_o + n_par * 0x20);
    const std::size_t args_o = b.size();
    std::size_t ii = 0, pi = 0, args = 0;
    for (std::size_t k = 0; k < events.size(); ++k) {
        const SynEvent& e = events[k];
        const std::size_t eo = ev_o + k * 0x30;
        Put<std::int64_t>(b, eo, e.id);
        Put<std::int64_t>(b, eo + 8, static_cast<std::int64_t>(e.ins.size()));
        Put<std::int64_t>(b, eo + 16, static_cast<std::int64_t>(ii * 0x20));
        Put<std::int64_t>(b, eo + 24, static_cast<std::int64_t>(e.params.size()));
        Put<std::int64_t>(b, eo + 32, static_cast<std::int64_t>(pi * 0x20));
        Put<std::uint32_t>(b, eo + 40, e.rest);
        for (const SynInstr& in : e.ins) {
            const std::size_t io = in_o + ii * 0x20;
            Put<std::uint32_t>(b, io, in.bank);
            Put<std::uint32_t>(b, io + 4, in.id);
            Put<std::int64_t>(b, io + 8, static_cast<std::int64_t>(in.args.size()));
            Put<std::int64_t>(b, io + 16, static_cast<std::int64_t>(args));
            b.resize(args_o + args + in.args.size());
            std::memcpy(b.data() + args_o + args, in.args.data(), in.args.size());
            args += in.args.size();
            ++ii;
        }
        for (const SynParam& p : e.params) {
            const std::size_t po = pa_o + pi * 0x20;
            Put<std::int64_t>(b, po, p.instr);
            Put<std::int64_t>(b, po + 8, p.target);
            Put<std::int64_t>(b, po + 16, p.source);
            Put<std::int32_t>(b, po + 24, p.count);
            ++pi;
        }
    }
    Put<std::int64_t>(b, 0x10, static_cast<std::int64_t>(events.size()));
    Put<std::int64_t>(b, 0x18, static_cast<std::int64_t>(ev_o));
    Put<std::int64_t>(b, 0x28, static_cast<std::int64_t>(in_o));
    Put<std::int64_t>(b, 0x10 + 8 * 8, static_cast<std::int64_t>(n_par));
    Put<std::int64_t>(b, 0x10 + 9 * 8, static_cast<std::int64_t>(pa_o));
    Put<std::int64_t>(b, 0x10 + 13 * 8, static_cast<std::int64_t>(args_o));
    return b;
}

struct SynDef {
    std::int64_t bank, id;
    std::vector<std::pair<std::string, std::uint32_t>> args;
};

Bytes MakeEmedf(const std::vector<SynDef>& defs) {
    // header 0xb0, one class, instructions, args, strings
    Bytes b(0xb0, 0);
    std::memcpy(b.data(), "EDF\0", 4);
    const std::size_t ocls = b.size();
    b.resize(ocls + 32);
    const std::size_t oins = b.size();
    b.resize(oins + defs.size() * 32);
    std::size_t n_args = 0;
    for (const SynDef& d : defs) n_args += d.args.size();
    const std::size_t oarg = b.size();
    b.resize(oarg + n_args * 64);
    const std::size_t ostr = b.size();
    std::size_t str_at = 2;  // offset 0 is the empty name
    b.resize(ostr + 2);
    std::size_t ai = 0;
    for (std::size_t i = 0; i < defs.size(); ++i) {
        const std::size_t io = oins + i * 32;
        Put<std::int64_t>(b, io, defs[i].id);
        Put<std::int64_t>(b, io + 8, static_cast<std::int64_t>(defs[i].args.size()));
        Put<std::int64_t>(b, io + 16, static_cast<std::int64_t>(ai * 64));
        for (const auto& a : defs[i].args) {
            const std::size_t ao = oarg + ai * 64;
            Put<std::int64_t>(b, ao, static_cast<std::int64_t>(str_at));
            Put<std::int64_t>(b, ao + 8, a.second);
            b.resize(ostr + str_at + (a.first.size() + 1) * 2);
            PutStr16(b, ostr + str_at, a.first);
            str_at += (a.first.size() + 1) * 2;
            ++ai;
        }
    }
    // all instructions under one class (bank of the first; the party tooling keys by the class's bank)
    Put<std::int64_t>(b, ocls, defs.empty() ? 0 : defs[0].bank);
    Put<std::int64_t>(b, ocls + 8, static_cast<std::int64_t>(defs.size()));
    Put<std::int64_t>(b, ocls + 16, 0);
    Put<std::int64_t>(b, 0x10, 1);
    Put<std::int64_t>(b, 0x18, static_cast<std::int64_t>(ocls));
    Put<std::int64_t>(b, 0x20, static_cast<std::int64_t>(defs.size()));
    Put<std::int64_t>(b, 0x28, static_cast<std::int64_t>(oins));
    Put<std::int64_t>(b, 0x30, static_cast<std::int64_t>(n_args));
    Put<std::int64_t>(b, 0x38, static_cast<std::int64_t>(oarg));
    Put<std::int64_t>(b, 0x10 + 8 * 16 + 8, static_cast<std::int64_t>(ostr));
    return b;
}

void TestContainers() {
    Bytes raw(5000);
    for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<std::uint8_t>(i % 7);
    const Bytes dcx = MakeDcx(raw);
    CHECK(dcx.size() < raw.size() && std::memcmp(dcx.data(), "DCX\0", 4) == 0);
    CHECK(DecodeMaybeDcx(dcx) == raw);
    CHECK(DecodeMaybeDcx(raw) == raw);  // not a DCX: unchanged
    Bytes other = dcx;
    for (int i = 0; i < 4; ++i) other.at(0x28 + static_cast<std::size_t>(i)) = static_cast<std::uint8_t>("KRAK"[i]);
    bool threw = false;
    try {
        DecodeMaybeDcx(other);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
    Bytes cut(dcx.begin(), dcx.begin() + 0x60);
    threw = false;
    try {
        DecodeMaybeDcx(cut);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);

    const Bytes bnd = MakeBnd4({{"N:\\x\\param\\gameparam\\AParam.param", {1, 2, 3}}, {"N:\\x\\paramdef\\BParam.paramdef", {9}}});
    const auto es = ParseBnd4(bnd);
    CHECK(es.size() == 2);
    const BndEntry* a = FindBndEntry(es, "AParam.param");
    CHECK(a && a->size == 3 && bnd[a->offset] == 1);
    CHECK(!FindBndEntry(es, "Nope.param"));
    threw = false;
    try {
        Bytes bad = bnd;
        Put<std::int64_t>(bad, 0x40 + 8, 1 << 20);
        ParseBnd4(bad);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
}

void TestParam() {
    const Bytes def = MakeParamdef({{"s32", "flag", 4}, {"u8", "a:3", 1}, {"u8", "b:5", 1}, {"dummy8", "pad[1]", 1}, {"s16", "neg", 2}, {"u16", "big", 2}});
    Bytes r1(10, 0), r2(10, 0);
    Put<std::int32_t>(r1, 0, -5);
    Put<std::uint8_t>(r1, 4, static_cast<std::uint8_t>(5 | 17 << 3));  // a = 5, b = 17
    Put<std::int16_t>(r1, 6, -2);
    Put<std::uint16_t>(r1, 8, 65535);
    Put<std::int32_t>(r2, 0, 77);
    const Bytes prm = MakeParam({{100, r1}, {200, r2}});
    const ParamTable t = ParseParam(prm, def);
    CHECK(t.Rows() == 2 && t.ids[1] == 200);
    CHECK(t.Get(0, "flag") == -5 && t.Get(0, "a") == 5 && t.Get(0, "b") == 17);
    CHECK(t.Get(0, "neg") == -2 && t.Get(0, "big") == 65535 && t.Get(1, "flag") == 77);
    CHECK(t.Col("pad") < 0 && t.Get(0, "missing", 42) == 42);
}

void TestEvents() {
    const Bytes edf = MakeEmedf({{2003, 2, {{"flag", 2}, {"state", 0}}}, {2003, 4, {{"lot", 5}}}});
    const Emedf emedf = ParseEmedf(edf);
    CHECK(emedf.Find(2003, 2) && emedf.Find(2003, 2)->args.size() == 2 && !emedf.Find(2003, 3));
    Bytes set_args(5, 0);
    Put<std::uint32_t>(set_args, 0, 111);
    Put<std::uint8_t>(set_args, 4, 1);
    Bytes call(12, 0);
    Put<std::int32_t>(call, 0, 3);    // slot
    Put<std::uint32_t>(call, 4, 200);  // target event
    Put<std::uint32_t>(call, 8, 12345678);  // its parameter
    Bytes lot(4, 0);
    Put<std::int32_t>(lot, 0, 5000);
    const Bytes evd = MakeEmevd({{0, 0, {{2000, 0, call}}, {}},
                                 {200, 1, {{2003, 2, set_args}, {2003, 4, lot}}, {{0, 0, 0, 4}}}});
    const Emevd e = ParseEmevd(evd);
    CHECK(e.events.size() == 2 && e.events[1].params.size() == 1);
    const auto recs = ExpandFile(e, {{200, "name"}}, emedf, {});
    CHECK(recs.size() == 2);
    const EventRec* r = nullptr;
    for (const EventRec& x : recs) {
        if (x.event == 200) r = &x;
    }
    CHECK(r && r->slot == 3 && r->rest == 1 && r->name == "name");
    CHECK(r && r->ops.size() == 1 && r->ops[0].op == FlagOp::Set && r->ops[0].first == 12345678 && r->ops[0].StateIs(1));
    CHECK(r && r->markers.size() == 1 && r->markers[0].kind == MarkerKind::ItemLot && r->markers[0].v[0] == 5000);
    // the same event called from another file (external call) gets its own instance
    ExternalCalls ext;
    Bytes p2(4, 0);
    Put<std::uint32_t>(p2, 0, 777);
    ext[200].push_back({9, std::string(reinterpret_cast<const char*>(p2.data()), 4)});
    const auto recs2 = ExpandFile(e, {}, emedf, ext);
    int n200 = 0;
    for (const EventRec& x : recs2) n200 += x.event == 200;
    CHECK(n200 == 2);
    bool threw = false;
    try {
        Bytes bad = evd;
        bad[8] = 0;
        ParseEmevd(bad);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
}

void TestMsb() {
    // header 0x10, one list "POINT_PARAM_ST" with one entry that has an entity id
    Bytes b(0x200, 0);
    std::memcpy(b.data(), "MSB ", 4);
    const std::size_t list = 0x10, name_at = 0x80, entry = 0x100, ent = 0x1c0;
    Put<std::int32_t>(b, list + 4, 2);  // entries + the next-list offset
    Put<std::int64_t>(b, list + 8, name_at);
    Put<std::int64_t>(b, list + 16, entry);
    Put<std::int64_t>(b, list + 24, 0);
    PutStr16(b, name_at, "POINT_PARAM_ST");
    Put<float>(b, entry + 0x14, 1.5f);
    Put<float>(b, entry + 0x18, -2.f);
    Put<float>(b, entry + 0x1c, 3.f);
    Put<float>(b, entry + 0x24, 90.f);
    Put<std::int64_t>(b, entry + 0x48, ent - entry);
    Put<std::int32_t>(b, ent, 2412951);
    const auto lists = ParseMsbLists(b);
    CHECK(lists.size() == 1 && lists[0].name == "POINT_PARAM_ST" && lists[0].entries.size() == 1);
    const auto pts = ReadMsbPoints(b, *FindMsbList(lists, "POINT_PARAM_ST"), false);
    CHECK(pts.size() == 1 && pts[0].x == 1.5f && pts[0].yaw_deg == 90.f && pts[0].has_entity && pts[0].entity == 2412951);
    std::vector<std::int32_t> ents;
    ScanMsbEntities(b, 2410000, &ents);
    CHECK(ents.size() == 1 && ents[0] == 2412951);
    bool threw = false;
    try {
        b[0] = 'X';
        ScanMsbEntities(b, 0, &ents);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
}

// ---- the derivation on a synthetic game folder, and a "mod" that edits a param ----

void WriteFile(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

struct ParamSpec {
    std::string name;
    std::vector<DefField> fields;
    std::vector<std::pair<std::int32_t, std::vector<std::int64_t>>> rows;  // per field value (all stored as s32 / s16 / u8 by type)
};

Bytes RowBytes(const std::vector<DefField>& fields, const std::vector<std::int64_t>& vals) {
    Bytes b;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].type == "s32") Put<std::int32_t>(b, b.size(), static_cast<std::int32_t>(vals[i]));
        else if (fields[i].type == "s16") Put<std::int16_t>(b, b.size(), static_cast<std::int16_t>(vals[i]));
        else Put<std::uint8_t>(b, b.size(), static_cast<std::uint8_t>(vals[i]));
    }
    return b;
}

void WriteGame(const fs::path& root, const std::vector<ParamSpec>& params) {
    std::vector<std::pair<std::string, Bytes>> pe, de;
    for (const ParamSpec& p : params) {
        std::vector<std::pair<std::int32_t, Bytes>> rows;
        for (const auto& r : p.rows) rows.emplace_back(r.first, RowBytes(p.fields, r.second));
        pe.emplace_back("N:\\p\\" + p.name + ".param", MakeParam(rows));
        de.emplace_back("N:\\p\\" + p.name + ".paramdef", MakeParamdef(p.fields));
    }
    WriteFile(root / "param/gameparam/gameparam.parambnd.dcx", MakeDcx(MakeBnd4(pe)));
    WriteFile(root / "paramdef/paramdef.paramdefbnd.dcx", MakeDcx(MakeBnd4(de)));
    WriteFile(root / "event/common.emedf.dcx", MakeDcx(MakeEmedf({{2003, 2, {{"flag", 2}, {"state", 0}}}})));
}

std::vector<ParamSpec> BaseParams(bool extra_lot) {
    std::vector<ParamSpec> v;
    ParamSpec lot{"ItemLotParam", {{"s32", "getItemFlagId", 4}, {"s32", "lotItemId01", 4}, {"s32", "lotItemCategory01", 4}}, {}};
    lot.rows = {{2000, {50002000, 100, 4}}};
    if (extra_lot) lot.rows.push_back({3000, {50003000, 200, 4}});
    v.push_back(lot);
    v.push_back({"EquipParamGoods", {{"s32", "goodsType", 4}, {"s32", "maxNum", 4}}, {{100, {1, 1}}, {200, {0, 5}}}});
    v.push_back({"EquipParamWeapon", {{"s32", "sortId", 4}, {"s16", "equipModelId", 2}, {"u8", "rightHandEquipable", 1}, {"u8", "leftHandEquipable", 1}},
                 {{1000000, {1, 7, 1, 0}}, {1010000, {2, 7, 1, 0}}, {1020000, {3, 8, 0, 1}}}});
    v.push_back({"EquipParamProtector", {{"s32", "sortId", 4}, {"s16", "equipModelId", 2}, {"u8", "headEquip", 1}}, {{10000, {1, 3, 1}}, {11000, {1, 3, 0}}}});
    v.push_back({"ReturnPointParam", {{"u8", "areaNo", 1}, {"u8", "blockNo", 1}, {"s32", "returnPointEntityId", 4}}, {}});
    v.push_back({"ShopLineupParam", {{"s32", "eventFlag", 4}, {"s32", "qwcId", 4}}, {}});
    return v;
}

// The cache key (0.8.1 tools081: sizes + mtimes read in one folder pass, was 2.7 s): it follows every kind of change a
// game / mod / update makes and stays the same when nothing changed.
void TestKey() {
    const fs::path d = fs::temp_directory_path() / "bb_gamedata_key_test";
    std::error_code ec;
    fs::remove_all(d, ec);
    const fs::path root = d / "dvdroot_ps4";
    WriteGame(root, BaseParams(false));
    const std::string& r = root.string();
    const std::string k0 = coop::gamedata::ComputeKey(r);
    CHECK(!k0.empty() && coop::gamedata::ComputeKey(r) == k0);
    // an event file appears / grows / is edited in place (same size, later time)
    WriteFile(root / "event/m10_00_00_00.emevd.dcx", Bytes(8, 1));
    const std::string k1 = coop::gamedata::ComputeKey(r);
    CHECK(k1 != k0);
    WriteFile(root / "event/m10_00_00_00.emevd.dcx", Bytes(9, 1));
    const std::string k2 = coop::gamedata::ComputeKey(r);
    CHECK(k2 != k1);
    fs::last_write_time(root / "event/m10_00_00_00.emevd.dcx", fs::last_write_time(root / "event/m10_00_00_00.emevd.dcx") + std::chrono::hours(2), ec);
    const std::string k3 = coop::gamedata::ComputeKey(r);
    CHECK(!ec && k3 != k2 && coop::gamedata::ComputeKey(r) == k3);
    // a nested folder under event/ counts, a map and a remo entry (names only) count, an msb counts
    WriteFile(root / "event/sub/deep.dcx", Bytes(4, 2));
    const std::string k4 = coop::gamedata::ComputeKey(r);
    CHECK(k4 != k3);
    WriteFile(root / "map/mapstudio/m10_00_00_00.msb.dcx", Bytes(4, 3));
    const std::string k5 = coop::gamedata::ComputeKey(r);
    CHECK(k5 != k4);
    WriteFile(root / "map/mapstudio/readme.txt", Bytes(4, 3));  // not an msb: ignored
    CHECK(coop::gamedata::ComputeKey(r) == k5);
    fs::create_directories(root / "map/m11_00_00_00", ec);
    const std::string k6 = coop::gamedata::ComputeKey(r);
    CHECK(k6 != k5);
    WriteFile(root / "remo/cut.remobnd.dcx", Bytes(4, 4));
    CHECK(coop::gamedata::ComputeKey(r) != k6);
    // the params (a game update or a mod)
    const std::string k7 = coop::gamedata::ComputeKey(r);
    WriteGame(root, BaseParams(true));
    CHECK(coop::gamedata::ComputeKey(r) != k7);
    fs::remove_all(d, ec);
}

void TestDerive() {
    const fs::path d = fs::temp_directory_path() / "bb_gamedata_test";
    std::error_code ec;
    fs::remove_all(d, ec);
    const fs::path root = d / "dvdroot_ps4";
    WriteGame(root, BaseParams(false));
    coop::gamedata::Timings tm;
    coop::gamedata::Progress prog;
    const auto s1 = coop::gamedata::DeriveAll(root.string(), 3, &prog, &tm);
    CHECK(s1.at("itemLotFlag").size() == 1 && s1.at("itemLotFlag")[0][0] == "2000" && s1.at("itemLotFlag")[0][1] == "50002000");
    CHECK(s1.at("warperRight").size() == 1 && s1.at("warperRight")[0][0] == "1000000");  // one per model (7), the 8 is left only
    CHECK(s1.at("warperLeft").size() == 1 && s1.at("warperLeft")[0][0] == "1020000");
    CHECK(s1.at("warperArmor").size() == 1 && s1.at("warperArmor")[0][0] == "10000");
    CHECK(s1.at("maps").empty() && s1.at("npcFog").empty() && s1.at("itemKillAward").size() == 1);
    CHECK(prog.Percent() >= 99);
    // a mod that edits the params: the next build follows it (and its key changes)
    const std::string key1 = coop::gamedata::ComputeKey(root.string());
    CHECK(!key1.empty());
    WriteGame(root, BaseParams(true));
    const auto s2 = coop::gamedata::DeriveAll(root.string(), 1, nullptr, nullptr);
    CHECK(s2.at("itemLotFlag").size() == 2 && s2.at("itemLotFlag")[1][0] == "3000");
    CHECK(coop::gamedata::ComputeKey(root.string()) != key1);
    // cache text round trip through the party parser
    const std::string text = coop::gamedata::RenderCache("k", s2);
    CHECK(coop::gamedata::CacheTextKey(text) == "k");
    coop::gamedata::Tables t;
    std::string err;
    CHECK(coop::gamedata::Parse(text, &t, &err) && t.lot_flags.size() == 2 && t.warper_left.size() == 1);
    // no game data: the key is empty, the build reports it
    CHECK(coop::gamedata::ComputeKey((d / "nothing").string()).empty());
    std::string log;
    CHECK(!coop::gamedata::BuildToFile((d / "nothing").string(), (d / "out.txt").string(), false, 1, &err, &log));
    // BuildToFile: build, then a current cache is left alone
    coop::gamedata::ResetRebuildFlag();
    CHECK(coop::gamedata::BuildToFile(d.string(), (d / "cache.txt").string(), false, 2, &err, &log) && fs::exists(d / "cache.txt"));
    CHECK(coop::gamedata::Rebuilding());  // a real build raises the flag the in-game notice keys on
    coop::gamedata::ResetRebuildFlag();
    const auto t1 = fs::last_write_time(d / "cache.txt");
    CHECK(coop::gamedata::BuildToFile(d.string(), (d / "cache.txt").string(), false, 2, &err, &log) && log.find("cache current") != std::string::npos);
    CHECK(!coop::gamedata::Rebuilding());  // a cache hit stays silent: no popup
    CHECK(fs::last_write_time(d / "cache.txt") == t1);
    fs::remove_all(d, ec);
}

// The boss fog gates and gate rules from event records built here (made-up ids, no game data).
void TestWorldAct() {
    using namespace bbdata;
    auto rec = [](std::int64_t id, const char* name) {
        EventRec r;
        r.event = id;
        r.name = name;
        return r;
    };
    auto set = [](EventRec& r, std::int64_t flag, std::int64_t state = 1) {
        Op o;
        o.op = FlagOp::Set;
        o.first = o.last = flag;
        o.state = state;
        r.ops.push_back(o);
    };
    auto cond = [](EventRec& r, std::int64_t flag) { r.conds.push_back({3, 0, {1, 1, 0, flag}}); };
    auto boss = [&](std::int64_t id) {
        EventRec r = rec(id, "boss");
        r.markers.push_back({MarkerKind::BossDefeat, 0, {1}});
        return r;
    };
    EventRec b1 = boss(100), b2 = boss(101), b3 = boss(102);
    set(b1, 2050);
    set(b1, 2051, 0);  // a flag set off is no rule
    set(b1, 9999);     // outside the gate range
    set(b2, 2050);     // owned by the first event
    set(b2, 2052);
    set(b3, 2060);
    EventRec both = rec(900, "perm");  // two bosses must be dead
    set(both, 2070);
    cond(both, 100);
    cond(both, 102);
    EventRec stray = rec(901, "perm");  // not a boss event flag among its conditions
    set(stray, 2071);
    cond(stray, 100);
    cond(stray, 5);
    EventRec g1 = rec(500, "ゲストがボス部屋入場_x");
    cond(g1, 100);
    cond(g1, 1234);
    cond(g1, 555);
    g1.conds.push_back({3, 24, {1, 2, 777}});
    set(g1, 556);
    EventRec first = rec(600, "ホストがボス部屋入場_初戦_y");
    cond(first, 102);
    set(first, 600);
    EventRec g2 = rec(501, "y_ゲストがボス部屋入場");
    cond(g2, 102);
    cond(g2, 566);
    g2.conds.push_back({3, 24, {1, 2, 778}});
    set(g2, 567);
    EventRec g3 = rec(502, "ゲストがボス部屋入場_z");  // the guest flag comes from the landing event
    cond(g3, 104);
    cond(g3, 105);
    cond(g3, 570);
    g3.conds.push_back({3, 24, {1, 2, 779}});
    EventRec land = rec(503, "ゲストが着地_z");
    cond(land, 570);
    set(land, 571);
    EventRec nofog = rec(504, "ゲストがボス部屋入場_w");  // no fog object: no gate
    cond(nofog, 1);
    cond(nofog, 2);
    set(nofog, 3);
    std::vector<coop::gamedata::FileEvents> ev = {{"m01", {b1, b2, b3, both, stray, g1, first, g2}}, {"m02", {g3, land, nofog}}};
    const auto s = coop::gamedata::DeriveWorldAct(ev);
    const auto& rules = s.at("gateRules");
    CHECK(rules.size() == 4);
    CHECK(rules[0] == (coop::gamedata::Row{"100", "2050", "0"}) && rules[1] == (coop::gamedata::Row{"100", "2070", "102"}));
    CHECK(rules[2] == (coop::gamedata::Row{"101", "2052", "0"}) && rules[3] == (coop::gamedata::Row{"102", "2060", "0"}));
    const auto& gates = s.at("bossGates");
    CHECK(gates.size() == 3);
    CHECK(gates[0] == (coop::gamedata::Row{"500", "555", "556", "100", "1234", "777", "0"}));
    CHECK(gates[1] == (coop::gamedata::Row{"501", "566", "567", "102", "600", "778", "0"}));
    CHECK(gates[2] == (coop::gamedata::Row{"502", "570", "571", "104", "105", "779", "503"}));
    // the boss-doping SpEffect: world bosses from the map scripts, dungeon bosses from the area 29 scripts and the
    // expansion with the dungeon maps' calls; other effects and entity 0 are no boss
    EventRec d1 = rec(700, "dope"), d2 = rec(701, "dope"), d3 = rec(702, "dope"), d4 = rec(703, "dope");
    d1.conds.push_back({2004, 8, {4001, 7500, 1}});
    d1.conds.push_back({2004, 8, {4002, 7501, 1}});
    d1.conds.push_back({2004, 8, {4003, 7000, 1}});  // another effect
    d2.conds.push_back({2004, 8, {0, 7500, 1}});     // an unresolved parameter
    d3.conds.push_back({2004, 8, {5001, 7500, 1}});
    d4.conds.push_back({2004, 8, {5002, 7501, 1}});
    d4.conds.push_back({2004, 8, {4001, 7500, 1}});  // listed once, as a world boss
    const auto be = coop::gamedata::DeriveBossEntities({{"m01", {d1, d2}}, {"m29_01_00_00", {d3}}}, {d4}).at("bossEntities");
    CHECK(be.size() == 4);
    CHECK(be[0] == (coop::gamedata::Row{"4001", "0"}) && be[1] == (coop::gamedata::Row{"4002", "0"}));
    CHECK(be[2] == (coop::gamedata::Row{"5001", "1"}) && be[3] == (coop::gamedata::Row{"5002", "1"}));
    // no events: empty sections, still present
    const auto none = coop::gamedata::DeriveWorldAct({});
    CHECK(none.at("gateRules").empty() && none.at("bossGates").empty());
    // the cache text round trip
    coop::gamedata::Tables t;
    std::string err;
    CHECK(coop::gamedata::Parse(coop::gamedata::RenderCache("k", s), &t, &err));
    CHECK(t.gate_rules.size() == 4 && t.gate_rules[1].also == 102 && t.boss_gates.size() == 3 && t.boss_gates[2].landing_event == 503);
    coop::gamedata::Sections all = s;
    all["bossEntities"] = be;
    CHECK(coop::gamedata::Parse(coop::gamedata::RenderCache("k", all), &t, &err));
    CHECK(t.boss_entities.size() == 2 && t.chalice_boss_entities.size() == 2 && t.chalice_boss_entities[1] == 5002);
}

// ---- the Messenger shop gates, the arena places and the progress id lists (made-up data) ----

ParamTable SynTable(const std::vector<DefField>& fields, const std::vector<std::pair<std::int32_t, std::vector<std::int64_t>>>& rows) {
    std::vector<std::pair<std::int32_t, Bytes>> r;
    for (const auto& row : rows) r.emplace_back(row.first, RowBytes(fields, row.second));
    return ParseParam(MakeParam(r), MakeParamdef(fields));
}

void TestShopGates() {
    using namespace bbdata;
    std::vector<DefField> lot_f = {{"s32", "getItemFlagId", 4}};
    for (int k = 1; k <= 8; ++k) lot_f.push_back({"s32", "lotItemId0" + std::to_string(k), 4});
    for (int k = 1; k <= 8; ++k) lot_f.push_back({"s32", "lotItemCategory0" + std::to_string(k), 4});
    auto lot = [](std::int64_t flag, std::int64_t item1, std::int64_t cat1, std::int64_t item2 = 0) {
        return std::vector<std::int64_t>{flag, item1, item2, 0, 0, 0, 0, 0, 0, cat1, 0, 0, 0, 0, 0, 0, 0};
    };
    coop::gamedata::ParamSet P;
    P.item_lot = SynTable(lot_f, {{10, lot(5100, 7010000, 0)},            // a weapon lot behind a shop gate
                                   {11, lot(5101, 7020000, 0, 7020001)},  // two items: no gate
                                   {12, lot(5102, 7030000, 4)},           // goods, not a weapon
                                   {13, lot(5103, 7040000, 0)}});         // the shop sells another weapon behind it
    P.shop = SynTable({{"s32", "qwcId", 4}, {"s32", "equipType", 4}, {"s32", "equipId", 4}},
                      {{1, {5100, 0, 7010000}}, {2, {5101, 0, 7020000}}, {3, {5102, 0, 7030000}}, {4, {5103, 0, 7999999}},
                       {5, {5500, 0, 23010000}}, {6, {5501, 0, 23020000}}, {7, {5501, 0, 23030000}}, {8, {5600, 0, 9990000}},
                       {9, {5104, 3, 7010000}}, {10, {-1, 0, 7010000}}});
    auto owns = [](std::int64_t slot, std::int64_t weapon, std::int64_t type = 0) {
        EventRec r;
        r.event = 5500;
        r.slot = static_cast<std::int32_t>(slot);
        r.conds.push_back({4, 3, {1, 10000, 0}});
        r.conds.push_back({3, 4, {0, type, weapon, 1}});
        return r;
    };
    EventRec other = owns(9, 7010000);  // a map script's event of the same id: not read
    const std::vector<coop::gamedata::FileEvents> ev = {
        {"common", {owns(0, 23010000), owns(1, 23020000), owns(7, 23999999), owns(8, 5, 3)}}, {"m01", {other}}};
    const auto rows = coop::gamedata::DeriveShopGates(P, ev).at("shopGates");
    CHECK(rows.size() == 2 && rows[0] == (coop::gamedata::Row{"5100", "7010000"}) && rows[1] == (coop::gamedata::Row{"5500", "23010000"}));
    // no params, no events: an empty section, no crash
    CHECK(coop::gamedata::DeriveShopGates(coop::gamedata::ParamSet{}, {}).at("shopGates").empty());
    coop::gamedata::Tables t;
    std::string err;
    CHECK(coop::gamedata::Parse(coop::gamedata::RenderCache("k", coop::gamedata::DeriveShopGates(P, ev)), &t, &err));
    CHECK(t.shop_gates.size() == 2 && t.shop_gates[1].flag == 5500 && t.shop_gates[1].weapon == 23010000);
}

void TestStartWeapons() {
    using namespace bbdata;
    coop::gamedata::ParamSet P;
    // vanilla-like rows (synthetic ids) with a randomized world's copies behind the stock flags
    const std::vector<DefField> f = {{"s32", "eventFlag", 4}, {"s32", "equipType", 4}, {"s32", "equipId", 4}};
    P.shop = SynTable(f, {{2000, {72101000, 0, 25500000}}, {2001, {72101001, 0, 7010000}}, {2010, {72101010, 0, 14000000}},
                          {2011, {72101011, 0, 14500000}}, {2012, {72101011, 0, 14600000}}});
    auto rows = coop::gamedata::DeriveStartWeapons(P).at("startWeapons");
    CHECK(rows.size() == 2 && rows[0] == (coop::gamedata::Row{"72101000", "25500000"}) &&
          rows[1] == (coop::gamedata::Row{"72101011", "14500000"}));  // the lowest row of a flag wins
    // a non-weapon row behind the flag, or no such row: nothing derived for that hand
    P.shop = SynTable(f, {{2000, {72101000, 3, 7000000}}, {2011, {72101011, 0, 14000000}}});
    rows = coop::gamedata::DeriveStartWeapons(P).at("startWeapons");
    CHECK(rows.size() == 1 && rows[0] == (coop::gamedata::Row{"72101011", "14000000"}));
    CHECK(coop::gamedata::DeriveStartWeapons(coop::gamedata::ParamSet{}).at("startWeapons").empty());
    coop::gamedata::Tables t;
    std::string err;
    CHECK(coop::gamedata::Parse(coop::gamedata::RenderCache("k", coop::gamedata::DeriveStartWeapons(P)), &t, &err));
    CHECK(t.start_weapons.size() == 1 && t.start_weapons[0].flag == 72101011 && t.start_weapons[0].weapon == 14000000);
}

// An MSB with a POINT_PARAM_ST and a PARTS_PARAM_ST list (names, positions, entity ids) for DeriveSpots.
struct SynPoint {
    std::string name;
    std::int32_t entity;
    float x, y, z;
};
struct SynPart {
    std::int32_t type, entity;
    float x, y, z;
};
Bytes MakeMsb(const std::vector<SynPoint>& points, const std::vector<SynPart>& parts) {
    Bytes b(0x6000, 0);
    std::memcpy(b.data(), "MSB ", 4);
    const std::size_t list1 = 0x10, list2 = 0x100, names = 0x200, pts = 0x1000, prt = 0x3000;
    Put<std::int32_t>(b, list1 + 4, static_cast<std::int32_t>(points.size() + 1));
    Put<std::int64_t>(b, list1 + 8, names);
    PutStr16(b, names, "POINT_PARAM_ST");
    for (std::size_t i = 0; i < points.size(); ++i) {
        Put<std::int64_t>(b, list1 + 16 + i * 8, pts + i * 0x100);
    }
    Put<std::int64_t>(b, list1 + 16 + points.size() * 8, list2);
    Put<std::int32_t>(b, list2 + 4, static_cast<std::int32_t>(parts.size() + 1));
    Put<std::int64_t>(b, list2 + 8, names + 0x40);
    PutStr16(b, names + 0x40, "PARTS_PARAM_ST");
    for (std::size_t i = 0; i < parts.size(); ++i) {
        Put<std::int64_t>(b, list2 + 16 + i * 8, prt + i * 0x100);
    }
    Put<std::int64_t>(b, list2 + 16 + parts.size() * 8, 0);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const std::size_t e = pts + i * 0x100, nm = 0x4800 + i * 0x80;
        Put<std::int64_t>(b, e, static_cast<std::int64_t>(nm - e));
        PutStr16(b, nm, points[i].name);
        Put<float>(b, e + 0x14, points[i].x);
        Put<float>(b, e + 0x18, points[i].y);
        Put<float>(b, e + 0x1c, points[i].z);
        Put<std::int64_t>(b, e + 0x48, 0xC0);
        Put<std::int32_t>(b, e + 0xC0, points[i].entity);
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::size_t e = prt + i * 0x100;
        Put<std::int32_t>(b, e + 0x14, parts[i].type);
        Put<float>(b, e + 0x28, parts[i].x);
        Put<float>(b, e + 0x2c, parts[i].y);
        Put<float>(b, e + 0x30, parts[i].z);
        Put<std::int64_t>(b, e + 0xB0, 0xC0);
        Put<std::int32_t>(b, e + 0xC0, parts[i].entity);
    }
    return b;
}

void TestSpots() {
    using namespace bbdata;
    using namespace coop::gamedata;
    const fs::path d = fs::temp_directory_path() / "bb_gamedata_spots";
    std::error_code ec;
    fs::remove_all(d, ec);
    WriteFile(d / "m01.msb.dcx",
              MakeDcx(MakeMsb({{"Event_霧壁埋まりワープ先", 8801, 10, 2, 30},    // 1 m inside the fog of gate 1
                               {"Event_ボス部屋進入目標", 8802, 50, 2, 60},        // an entry target farther away: not taken
                               {"Event_領域_初戦", 8800, 1, 2, 3},                 // the first fight's region
                               {"Event_ボス部屋侵入目標", 8820, 130, 0, 0},        // gate 2 has no stuck point: the entry target
                               {"Event_霧壁埋まりワープ先_遠い", 8830, 900, 0, 0}},  // far from the fog of gate 3
                              {{1, 777, 10, 2, 29}, {2, 4001, 0, 0, 0}, {2, 4002, 0, 0, 0}, {2, 4011, 0, 0, 0}, {1, 4003, 0, 0, 0},
                               {1, 778, 100, 0, 0}, {2, 4101, 7, 8, 9}, {1, 779, -500, 0, 0}})));
    auto rec = [](std::int64_t id, const char* name) {
        EventRec r;
        r.event = id;
        r.name = name;
        return r;
    };
    EventRec boss1 = rec(100, "ボス撃破"), boss2 = rec(101, "ボス撃破"), boss3 = rec(102, "ボス撃破");
    boss1.conds.push_back({4, 0, {1, 4001, 1}});
    boss1.markers.push_back({MarkerKind::BossDefeat, 0, {4001}});
    boss2.markers.push_back({MarkerKind::BossDefeat, 0, {4101}});
    boss3.markers.push_back({MarkerKind::BossDefeat, 0, {4201}});
    EventRec entry1 = rec(500, "ゲストがボス部屋入場"), entry2 = rec(501, "ゲストがボス部屋入場"), entry3 = rec(502, "ゲストがボス部屋入場");
    EventRec in1 = rec(600, "ホストがボス部屋入場_初戦_後半"), in2 = rec(601, "ホストがボス部屋入場_初戦"), in3 = rec(602, "ホストがボス部屋入場_初戦");
    in1.conds.push_back({3, 2, {1, 1, 10000, 8800}});  // 3[2]: the region 8800
    in1.markers.push_back({MarkerKind::Cutscene, 3, {1000, 0, 10000}});
    const float metres = 16.0f;
    std::int32_t bits = 0;
    std::memcpy(&bits, &metres, 4);
    in2.conds.push_back({3, 3, {1, 1, 4101, 10000, bits}});  // 3[3]: within 16 m of the character 4101
    in3.conds.push_back({3, 2, {1, 1, 10000, 8800}});        // two position conditions: no spot
    in3.conds.push_back({3, 2, {1, 1, 10000, 8801}});
    const std::vector<FileEvents> ev = {{"m01", {boss1, boss2, boss3, entry1, entry2, entry3, in1, in2, in3}}};
    Sections wa;
    wa["bossGates"] = {{"500", "1", "2", "100", "600", "777", "0"},
                       {"501", "1", "2", "101", "601", "778", "0"},
                       {"502", "1", "2", "102", "602", "779", "0"},
                       {"503", "1", "2", "100", "0", "777", "0"}};  // an entry event that is in no script: skipped
    const Sections s = DeriveSpots(ev, wa, d.string(), 2);
    const auto& ar = s.at("bossArenas");
    // gate 1: the boss 4001 and its decade mate 4002 (a character part), not 4011 or the object 4003; gate 2: 4101
    CHECK(ar.size() == 3);
    if (ar.size() == 3) {
        CHECK(ar[0] == (Row{"4001", "10.00", "2.00", "30.00"}) && ar[1] == (Row{"4002", "10.00", "2.00", "30.00"}));
        CHECK(ar[2] == (Row{"4101", "130.00", "0.00", "0.00"}));
    }
    const auto& ff = s.at("firstFightSpots");
    CHECK(ff.size() == 2);
    if (ff.size() == 2) {
        CHECK(ff[0] == (Row{"600", "1.00", "2.50", "3.00", "1", "1"}));   // region 8800 raised 0.5 m; second half; plays a scene
        CHECK(ff[1] == (Row{"601", "7.00", "8.50", "11.00", "0", "0"}));  // the character's part, +0.5 m up, +2 m along z
    }
    // the cache round trip
    Tables t;
    std::string err;
    CHECK(Parse(RenderCache("k", s), &t, &err));
    CHECK(t.boss_arenas.size() == 3 && t.boss_arenas[2].boss == 4101 && t.boss_arenas[2].x == 130.0f);
    CHECK(t.first_fights.size() == 2 && t.first_fights[0].chained && t.first_fights[0].scene && !t.first_fights[1].chained);
    // no map file, no gates: nothing, no crash
    const Sections miss = DeriveSpots(ev, wa, (d / "nowhere").string(), 1);
    CHECK(miss.at("bossArenas").empty() && miss.at("firstFightSpots").empty());
    CHECK(DeriveSpots({}, {}, d.string(), 1).at("bossArenas").empty());
    fs::remove_all(d, ec);
}

void TestProgressIds() {
    using namespace bbdata;
    using namespace coop::gamedata;
    auto set = [](EventRec& r, std::int64_t first, std::int64_t last, std::int64_t state, FlagOp op = FlagOp::Set) {
        Op o;
        o.op = op;
        o.first = first;
        o.last = last;
        o.state = state;
        r.ops.push_back(o);
    };
    EventRec door, rnd, counter, never, wide, toggled;
    door.event = 12400140;
    set(door, 12400150, 12400151, 0);  // a door flag cleared by its event: a state flag (a range of two)
    set(door, 12400152, 12400152, 1);  // set on: no state
    rnd.event = 12400141;
    set(rnd, 12400160, 12400161, 0, FlagOp::Random);  // randomised: runtime
    counter.event = 12400142;
    set(counter, 12400170, 12400170, 0, FlagOp::ValueInc);
    never.event = 12400143;
    set(never, 21, 21, 0);  // an ending flag: a never_sync row, in neither list
    wide.event = 12400144;
    set(wide, 12400200, 12400900, 0);  // a whole group at once: ignored
    toggled.event = 12400145;
    set(toggled, 12400180, 12400180, 2, FlagOp::Toggle);
    EventRec common_ev;
    common_ev.event = 3503;
    set(common_ev, 12400190, 12400190, 0);  // common events are per-player systems: not read for the state list
    const std::vector<FileEvents> ev = {{"m24_00_00_00", {door, rnd, counter, never, wide, toggled}}, {"common", {common_ev}}};
    const ParamSet P;
    const FlagCategories cats(ev, P);
    CHECK(cats.Lookup(12400150) == Cat::world_state);  // the made-up flags sit in a category the lists take
    const Sections s = DeriveProgressIds(cats, ev);
    std::vector<std::string> st, rt;
    for (const Row& r : s.at("stateIds")) st.push_back(r[0]);
    for (const Row& r : s.at("runtimeIds")) rt.push_back(r[0]);
    CHECK((st == std::vector<std::string>{"12400150", "12400151", "12400180"}));
    CHECK((rt == std::vector<std::string>{"12400160", "12400161", "12400170"}));
    Tables t;
    std::string err;
    CHECK(Parse(RenderCache("k", s), &t, &err));
    CHECK(t.state_ids.size() == 3 && t.runtime_ids.size() == 3 && t.state_ids[0] == 12400150u);
    const std::vector<FileEvents> no_events;
    const Sections none = DeriveProgressIds(FlagCategories(no_events, P), no_events);
    CHECK(none.at("stateIds").empty() && none.at("runtimeIds").empty());
}

void TestParallel() {
    std::atomic<int> sum{0};
    coop::gamedata::ParallelFor(100, 4, [&](std::size_t i) { sum += static_cast<int>(i); });
    CHECK(sum == 4950);
    bool threw = false;
    try {
        coop::gamedata::ParallelFor(10, 4, [&](std::size_t i) {
            if (i == 3 || i == 7) throw std::runtime_error("e" + std::to_string(i));
        });
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()) == "e3";  // the lowest failing index
    }
    CHECK(threw);
}

}  // namespace

int main() {
    TestContainers();
    TestParam();
    TestEvents();
    TestMsb();
    TestParallel();
    TestWorldAct();
    TestShopGates();
    TestStartWeapons();
    TestSpots();
    TestProgressIds();
    TestDerive();
    TestKey();
    std::printf(g_fail ? "gamedata-test: %d FAILED\n" : "gamedata-test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
