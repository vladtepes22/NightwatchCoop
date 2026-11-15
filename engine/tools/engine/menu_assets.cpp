// SPDX-License-Identifier: GPL-2.0-or-later
#include "menu_assets.h"
#include "rebirth_script.h"
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <set>

namespace menu_assets {
namespace {
std::uint64_t Le(const Bytes& b, std::size_t at, int n) {
    if (at > b.size() || std::size_t(n) > b.size() - at) throw Error("a file ends early");
    std::uint64_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = v << 8 | b[at + i];
    return v;
}
std::uint16_t U16(const Bytes& b, std::size_t at) { return std::uint16_t(Le(b, at, 2)); }
std::uint32_t U32(const Bytes& b, std::size_t at) { return std::uint32_t(Le(b, at, 4)); }
std::uint64_t U64(const Bytes& b, std::size_t at) { return Le(b, at, 8); }
std::uint32_t Be32(const Bytes& b, std::size_t at) {
    const std::uint32_t v = U32(b, at);
    return (v >> 24) | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | (v << 24);
}
void SetLe(Bytes& b, std::size_t at, std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i) b[at + i] = std::uint8_t(v >> (8 * i));
}
void SetBe32(Bytes& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = std::uint8_t(v >> (24 - 8 * i));
}
void PutLe(Bytes& b, std::uint64_t v, int n) {
    b.resize(b.size() + n);
    SetLe(b, b.size() - n, v, n);
}
void Append(Bytes& b, const Bytes& more) { b.insert(b.end(), more.begin(), more.end()); }
bool Has(const Bytes& b, std::size_t at, const char* magic, std::size_t n) {
    return at <= b.size() && n <= b.size() - at && std::memcmp(b.data() + at, magic, n) == 0;
}
std::size_t WideEnd(const Bytes& b, std::size_t from) {
    for (std::size_t at = from; at + 1 < b.size(); at += 2)
        if (!b[at] && !b[at + 1]) return at;
    throw Error("a string runs past the end");
}

enum Code { kEnd = 0, kShowFrame = 1, kPlace2 = 26, kSprite = 39, kPlace3 = 70, kSymbolClass = 76 };
const std::set<int> kDefines = {2, 6, 7, 10, 11, 14, 20, 21, 22, 32, 33, 34, 35, 36, 37, 39,
                                46, 48, 60, 75, 83, 84, 90, 91, 1001, 1003, 1005, 1006, 1007, 1008, 1009, 1010};

struct Tag {
    int code = 0;
    Bytes body;
    Bytes raw;
};

std::vector<Tag> ReadTags(const Bytes& b, std::size_t at, std::size_t end) {
    std::vector<Tag> out;
    while (at < end) {
        const std::size_t head = at;
        const std::uint16_t code_length = U16(b, at);
        at += 2;
        std::size_t length = code_length & 0x3f;
        if (length == 0x3f) {
            length = U32(b, at);
            at += 4;
        }
        if (length > end - std::min(at, end)) throw Error("a movie tag runs past its end");
        Tag t;
        t.code = code_length >> 6;
        t.body.assign(b.begin() + at, b.begin() + at + length);
        t.raw.assign(b.begin() + head, b.begin() + at + length);
        at += length;
        out.push_back(std::move(t));
        if (out.back().code == kEnd) break;
    }
    return out;
}

void WriteTag(Bytes& out, const Tag& t) {
    if (!t.raw.empty()) return Append(out, t.raw);
    if (t.body.size() < 0x3f) {
        PutLe(out, std::uint32_t(t.code << 6 | t.body.size()), 2);
    } else {
        PutLe(out, std::uint32_t(t.code << 6 | 0x3f), 2);
        PutLe(out, t.body.size(), 4);
    }
    Append(out, t.body);
}
Tag Changed(int code, Bytes body) { return Tag{code, std::move(body), {}}; }

struct BitReader {
    const Bytes& b;
    std::size_t at;
    int bit = 0;
    std::uint32_t Unsigned(int n) {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            if (at >= b.size()) throw Error("a bit field runs past the end");
            v = v << 1 | (b[at] >> (7 - bit) & 1);
            if (++bit == 8) bit = 0, ++at;
        }
        return v;
    }
    std::int64_t Signed(int n) {
        const std::uint32_t v = Unsigned(n);
        return n && (v >> (n - 1) & 1) ? std::int64_t(v) - (std::int64_t(1) << n) : std::int64_t(v);
    }
    std::size_t End() { return bit ? at + 1 : at; }
};
struct BitWriter {
    Bytes out;
    int bit = 0;
    void Put(std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            if (!bit) out.push_back(0);
            out.back() |= std::uint8_t((v >> i & 1) << (7 - bit));
            bit = (bit + 1) % 8;
        }
    }
};
int BitsFor(std::int64_t v) {
    int n = 1;
    while (v < -(std::int64_t(1) << (n - 1)) || v >= (std::int64_t(1) << (n - 1))) ++n;
    return n;
}

struct Matrix {
    bool scaled = false, rotated = false;
    int scale_bits = 0, rotate_bits = 0;
    std::uint32_t sx = 0, sy = 0, r0 = 0, r1 = 0;
    std::int64_t tx = 0, ty = 0;
};
Matrix ReadMatrix(BitReader& r) {
    Matrix m;
    if ((m.scaled = r.Unsigned(1))) {
        m.scale_bits = int(r.Unsigned(5));
        m.sx = r.Unsigned(m.scale_bits);
        m.sy = r.Unsigned(m.scale_bits);
    }
    if ((m.rotated = r.Unsigned(1))) {
        m.rotate_bits = int(r.Unsigned(5));
        m.r0 = r.Unsigned(m.rotate_bits);
        m.r1 = r.Unsigned(m.rotate_bits);
    }
    const int n = int(r.Unsigned(5));
    m.tx = r.Signed(n);
    m.ty = r.Signed(n);
    return m;
}
Bytes WriteMatrix(const Matrix& m) {
    BitWriter w;
    w.Put(m.scaled, 1);
    if (m.scaled) w.Put(m.scale_bits, 5), w.Put(m.sx, m.scale_bits), w.Put(m.sy, m.scale_bits);
    w.Put(m.rotated, 1);
    if (m.rotated) w.Put(m.rotate_bits, 5), w.Put(m.r0, m.rotate_bits), w.Put(m.r1, m.rotate_bits);
    const int n = std::max(BitsFor(m.tx), BitsFor(m.ty));
    w.Put(n, 5);
    w.Put(std::uint64_t(m.tx) & ((std::uint64_t(1) << n) - 1), n);
    w.Put(std::uint64_t(m.ty) & ((std::uint64_t(1) << n) - 1), n);
    return w.out;
}
double Fixed(std::uint32_t v, int bits) {
    return double(bits && (v >> (bits - 1) & 1) ? std::int64_t(v) - (std::int64_t(1) << bits) : std::int64_t(v)) / 65536.0;
}

struct Place {
    int code = 0;
    std::uint8_t flags = 0;
    std::uint16_t depth = 0;
    std::size_t character_at = 0, matrix_at = 0, matrix_end = 0, name_at = 0, name_end = 0;
    std::optional<std::uint16_t> character;
    std::optional<Matrix> matrix;
    std::optional<std::string> name;
};
bool IsPlace(const Tag& t) { return t.code == kPlace2 || t.code == kPlace3; }
Place ReadPlace(const Tag& t) {
    const Bytes& b = t.body;
    Place p;
    p.code = t.code;
    p.flags = std::uint8_t(Le(b, 0, 1));
    std::size_t at = 1;
    std::uint8_t more = 0;
    if (t.code == kPlace3) more = std::uint8_t(Le(b, at++, 1));
    p.depth = U16(b, at);
    at += 2;
    if (t.code == kPlace3 && ((more & 0x08) || ((more & 0x10) && (p.flags & 0x02)))) {
        while (at < b.size() && b[at]) ++at;
        ++at;
    }
    p.character_at = at;
    if (p.flags & 0x02) {
        p.character = U16(b, at);
        at += 2;
    }
    p.matrix_at = p.matrix_end = at;
    if (p.flags & 0x04) {
        BitReader r{b, at};
        p.matrix = ReadMatrix(r);
        at = p.matrix_end = r.End();
    }
    if (p.flags & 0x08) {
        BitReader r{b, at};
        const bool add = r.Unsigned(1), mult = r.Unsigned(1);
        const int n = int(r.Unsigned(4));
        if (mult) r.Unsigned(4 * n);
        if (add) r.Unsigned(4 * n);
        at = r.End();
    }
    if (p.flags & 0x10) at += 2;
    p.name_at = p.name_end = at;
    if (p.flags & 0x20) {
        std::size_t end = at;
        while (end < b.size() && b[end]) ++end;
        if (end >= b.size()) throw Error("an unterminated instance name");
        p.name.emplace(b.begin() + at, b.begin() + end);
        p.name_end = end + 1;
    }
    return p;
}
Bytes Rewrite(const Tag& t, const Place& p, std::optional<std::uint16_t> depth, std::optional<std::uint16_t> character,
              const std::optional<Matrix>& matrix, const std::optional<std::string>& name) {
    const Bytes& b = t.body;
    Bytes out(b.begin(), b.begin() + p.character_at);
    if (depth) SetLe(out, t.code == kPlace3 ? 2 : 1, *depth, 2);
    if (p.character) PutLe(out, character.value_or(*p.character), 2);
    if (p.matrix) Append(out, matrix ? WriteMatrix(*matrix) : Bytes(b.begin() + p.matrix_at, b.begin() + p.matrix_end));
    out.insert(out.end(), b.begin() + p.matrix_end, b.begin() + p.name_at);
    if (p.name) {
        const std::string& n = name.value_or(*p.name);
        out.insert(out.end(), n.begin(), n.end());
        out.push_back(0);
    }
    out.insert(out.end(), b.begin() + p.name_end, b.end());
    return out;
}

struct Movie {
    Bytes header;
    std::vector<Tag> tags;
};
Movie ReadMovie(const Bytes& b) {
    if (!Has(b, 0, "GFX", 3) || b.size() < 9) throw Error("optionsetting.gfx is not an uncompressed GFX movie");
    const std::size_t start = 8 + (5 + 4 * (b[8] >> 3) + 7) / 8 + 4;
    if (start > b.size()) throw Error("optionsetting.gfx ends in its header");
    return Movie{Bytes(b.begin(), b.begin() + start), ReadTags(b, start, b.size())};
}
Bytes WriteMovie(const Movie& m) {
    Bytes out = m.header;
    for (const Tag& t : m.tags) WriteTag(out, t);
    SetLe(out, 4, out.size(), 4);
    return out;
}

struct Sprite {
    std::uint16_t id = 0, frames = 0;
    std::vector<Tag> tags;
};
Sprite ReadSprite(const Tag& t) { return Sprite{U16(t.body, 0), U16(t.body, 2), ReadTags(t.body, 4, t.body.size())}; }
Tag WriteSprite(const Sprite& s) {
    Bytes body;
    PutLe(body, s.id, 2);
    PutLe(body, s.frames, 2);
    for (const Tag& t : s.tags) WriteTag(body, t);
    return Changed(kSprite, std::move(body));
}

std::size_t FindSprite(const Movie& m, std::uint16_t id) {
    for (std::size_t i = 0; i < m.tags.size(); ++i)
        if (m.tags[i].code == kSprite && U16(m.tags[i].body, 0) == id) return i;
    throw Error("no sprite " + std::to_string(id) + " in the movie");
}
std::optional<std::size_t> Placed(const Movie& m, const std::string& name) {
    for (std::size_t i = 0; i < m.tags.size(); ++i)
        if (IsPlace(m.tags[i]) && ReadPlace(m.tags[i]).name == name) return i;
    return std::nullopt;
}
std::size_t FindPlaced(const Movie& m, const std::string& name) {
    if (const auto at = Placed(m, name)) return *at;
    throw Error("the movie places no " + name);
}
std::uint16_t HighestCharacter(const Movie& m) {
    std::uint16_t most = 0;
    for (const Tag& t : m.tags)
        if (kDefines.count(t.code) && t.body.size() >= 2) most = std::max(most, U16(t.body, 0));
    return most;
}
int RowIndex(const std::string& name) {
    if (name.size() < 8 || name.compare(0, 5, "Item_") || name.compare(name.size() - 2, 2, "_0")) return -1;
    const std::string n = name.substr(5, name.size() - 7);
    if (n.empty() || n.size() > 2 || !std::all_of(n.begin(), n.end(), ::isdigit)) return -1;
    return std::stoi(n);
}
std::string RowName(int index) { return "Item_" + std::to_string(index) + "_0"; }

struct Section {
    const char* name;
    std::int32_t title;
    int rows;
    bool defaults;
};
constexpr Section kSections[] = {
    {"PCGraphics", 117020, 6, true}, {"PCControls", 118020, 8, false}, {"PCKeys", 119020, 8, false},
    {"PCEffects", 120020, 5, true},  {"PCCamera", 121020, 3, true},    {"PCEnhance", 124020, 3, true},
    {"PCColour", 125020, 5, true},
};
constexpr int kDonorRows = 6, kDonorDefaults = 6;
constexpr std::uint16_t kFirstSectionDepth = 383;

struct Donor {
    std::size_t tag = 0;
    Sprite sprite;
    Matrix placed;
    std::optional<std::string> class_name;
    std::uint16_t title = 0;
    Sprite title_sprite;
    std::string title_field;
    std::uint16_t row_depth[kDonorRows] = {}, defaults_depth = 0;
    std::int64_t pitch = 0;
    std::set<std::uint16_t> depths;
};

std::map<std::uint16_t, std::string> SymbolClasses(const Movie& m) {
    std::map<std::uint16_t, std::string> out;
    for (const Tag& t : m.tags) {
        if (t.code != kSymbolClass) continue;
        std::size_t at = 2;
        for (std::uint16_t n = U16(t.body, 0); n; --n) {
            const std::uint16_t id = U16(t.body, at);
            std::size_t end = at + 2;
            while (end < t.body.size() && t.body[end]) ++end;
            if (end >= t.body.size()) throw Error("an unterminated class name");
            out[id].assign(t.body.begin() + at + 2, t.body.begin() + end);
            at = end + 1;
        }
    }
    return out;
}

Donor ReadDonor(const Movie& m) {
    Donor d;
    const Place placed = ReadPlace(m.tags[FindPlaced(m, "ControllSetting")]);
    if (!placed.character || !placed.matrix) throw Error("ControllSetting is placed without a character or position");
    d.placed = *placed.matrix;
    d.tag = FindSprite(m, *placed.character);
    d.sprite = ReadSprite(m.tags[d.tag]);
    if (auto c = SymbolClasses(m); c.count(d.sprite.id)) d.class_name = c[d.sprite.id];
    bool rows[kDonorRows] = {}, defaults = false;
    std::int64_t row0 = 0, row1 = 0;
    for (const Tag& t : d.sprite.tags) {
        if (!IsPlace(t)) continue;
        const Place p = ReadPlace(t);
        d.depths.insert(p.depth);
        if (p.name && p.matrix) {
            const int row = RowIndex(*p.name);
            if (row >= 0 && row < kDonorRows) {
                rows[row] = true;
                d.row_depth[row] = p.depth;
                if (row == 0) row0 = p.matrix->ty;
                if (row == 1) row1 = p.matrix->ty;
            } else if (row == kDonorDefaults) {
                defaults = true;
                d.defaults_depth = p.depth;
            }
        }
        if (p.character && !d.title) {
            for (std::size_t i = 0; i < m.tags.size(); ++i) {
                if (m.tags[i].code != kSprite || U16(m.tags[i].body, 0) != *p.character) continue;
                const Sprite child = ReadSprite(m.tags[i]);
                for (const Tag& c : child.tags) {
                    if (c.code != kPlace2) continue;
                    const Place field = ReadPlace(c);
                    if (field.name && field.name->rfind("StaticText_", 0) == 0) {
                        d.title = child.id;
                        d.title_sprite = child;
                        d.title_field = *field.name;
                    }
                }
            }
        }
    }
    if (!std::all_of(rows, rows + kDonorRows, [](bool r) { return r; }) || !defaults || !d.title)
        throw Error("ControllSetting is not laid out as 1.09's (six rows, Defaults, a title)");
    d.pitch = row1 - row0;
    return d;
}

Sprite SectionSprite(const Donor& d, const Section& s, std::uint16_t id, std::uint16_t title) {
    const int kept = std::min(s.rows, kDonorRows);
    std::set<std::uint16_t> dropped;
    for (int r = kept; r < kDonorRows; ++r) dropped.insert(d.row_depth[r]);
    if (!s.defaults) dropped.insert(d.defaults_depth);
    std::vector<std::uint16_t> added;
    for (std::uint16_t depth = d.row_depth[kDonorRows - 1] + 1; int(added.size()) < s.rows - kDonorRows; ++depth)
        if (!d.depths.count(depth)) added.push_back(depth);
    const std::int64_t defaults_dy = d.pitch * (s.rows - kDonorDefaults);

    Sprite out{id, d.sprite.frames, {}};
    for (const Tag& t : d.sprite.tags) {
        if (!IsPlace(t)) {
            out.tags.push_back(t);
            continue;
        }
        const Place p = ReadPlace(t);
        if (dropped.count(p.depth)) continue;
        if (p.character == d.title) {
            out.tags.push_back(Changed(t.code, Rewrite(t, p, {}, title, {}, {})));
        } else if (p.depth == d.defaults_depth && defaults_dy) {
            std::optional<Matrix> m = p.matrix;
            if (m) m->ty += defaults_dy;
            const auto name = p.name ? std::optional<std::string>(RowName(s.rows)) : std::nullopt;
            out.tags.push_back(Changed(t.code, Rewrite(t, p, {}, {}, m, name)));
        } else {
            out.tags.push_back(t);
        }
        if (p.depth == d.row_depth[kDonorRows - 1] && t.code == kPlace2) {
            for (std::size_t k = 0; k < added.size(); ++k) {
                std::optional<Matrix> m = p.matrix;
                if (m) m->ty += d.pitch * std::int64_t(k + 1);
                const auto name = p.name ? std::optional<std::string>(RowName(kDonorRows + int(k))) : std::nullopt;
                out.tags.push_back(Changed(kPlace2, Rewrite(t, p, added[k], {}, m, name)));
            }
        }
    }
    return out;
}

Sprite TitleSprite(const Donor& d, const Section& s, std::uint16_t id) {
    const std::string field = "StaticText_" + std::to_string(s.title);
    Sprite out{id, d.title_sprite.frames, {}};
    for (const Tag& t : d.title_sprite.tags) {
        if (t.code == kPlace2 && ReadPlace(t).name == d.title_field) {
            const Place p = ReadPlace(t);
            out.tags.push_back(Changed(kPlace2, Rewrite(t, p, {}, {}, {}, field)));
        } else {
            out.tags.push_back(t);
        }
    }
    return out;
}

Tag HiddenPlacement(std::uint16_t id, std::uint16_t depth, const Matrix& at, const std::string& name) {
    Bytes b{0x02 | 0x04 | 0x20, 0x20};
    PutLe(b, depth, 2);
    PutLe(b, id, 2);
    Append(b, WriteMatrix(at));
    b.insert(b.end(), name.begin(), name.end());
    b.push_back(0);
    b.push_back(0);
    return Changed(kPlace3, std::move(b));
}

void AddClasses(Movie& m, const std::vector<std::pair<std::uint16_t, std::string>>& classes) {
    for (Tag& t : m.tags) {
        if (t.code != kSymbolClass) continue;
        Bytes body = t.body;
        SetLe(body, 0, U16(body, 0) + classes.size(), 2);
        for (const auto& [id, name] : classes) {
            PutLe(body, id, 2);
            body.insert(body.end(), name.begin(), name.end());
            body.push_back(0);
        }
        t = Changed(kSymbolClass, std::move(body));
        return;
    }
    throw Error("the movie has no SymbolClass");
}

constexpr double kScrollX = 1290, kScrollY = 354.5;
void AddScrollBar(Movie& m) {
    const Place top = ReadPlace(m.tags[FindPlaced(m, "Top")]);
    if (!top.character) throw Error("Top is placed without a character");
    std::optional<std::uint16_t> bar;
    for (const Tag& t : m.tags) {
        if (t.code != kSprite || bar) continue;
        for (const Tag& c : ReadSprite(t).tags)
            if (IsPlace(c) && !bar) {
                const Place p = ReadPlace(c);
                if (p.name == "ScrollBarV" && p.character) bar = p.character;
            }
    }
    if (!bar) throw Error("the movie has no ScrollBarV to place");
    Matrix list = top.matrix.value_or(Matrix{});
    if (list.rotated) throw Error("Top is rotated");
    const double sx = list.scaled ? Fixed(list.sx, list.scale_bits) : 1, sy = list.scaled ? Fixed(list.sy, list.scale_bits) : 1;
    const std::size_t at = FindSprite(m, *top.character);
    Sprite s = ReadSprite(m.tags[at]);
    std::uint16_t depth = 0;
    for (const Tag& t : s.tags) {
        if (!IsPlace(t)) continue;
        const Place p = ReadPlace(t);
        if (p.name == "ScrollBarV") throw Error("Top has a ScrollBarV already");
        depth = std::max(depth, p.depth);
    }
    const auto first_frame = std::find_if(s.tags.begin(), s.tags.end(), [](const Tag& t) { return t.code == kShowFrame; });
    if (first_frame == s.tags.end()) throw Error("Top has no frame");
    Matrix place;
    place.tx = std::llrint((kScrollX * 20 - double(list.tx)) / sx);
    place.ty = std::llrint((kScrollY * 20 - double(list.ty)) / sy);
    Bytes b{0x02 | 0x04 | 0x20};
    PutLe(b, depth + 1, 2);
    PutLe(b, *bar, 2);
    Append(b, WriteMatrix(place));
    const std::string name = "ScrollBarV";
    b.insert(b.end(), name.begin(), name.end());
    b.push_back(0);
    s.tags.insert(first_frame, Changed(kPlace2, std::move(b)));
    m.tags[at] = WriteSprite(s);
}

const char kTsv[] =
#include "pc_option_messages.inc"
    ;

constexpr std::uint32_t kMenuGroup = 200, kHelpGroup = 201, kQuestionGroup = 78, kScreenQuestionGroup = 204, kEventGroup = 30;

std::string Unescape(std::string s) {
    for (std::size_t at = 0; (at = s.find("\\n", at)) != std::string::npos; ++at) s.replace(at, 2, "\n");
    return s;
}
}

std::u16string Utf16(const std::string& s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto lead = static_cast<unsigned char>(s[i++]);
        const int more = lead >= 0xf0 ? 3 : lead >= 0xe0 ? 2 : lead >= 0xc0 ? 1 : 0;
        std::uint32_t c = lead & (0x7f >> more);
        for (int k = 0; k < more && i < s.size(); ++k) c = c << 6 | (static_cast<unsigned char>(s[i++]) & 0x3f);
        if (c >= 0x10000) {
            out.push_back(char16_t(0xd800 + ((c - 0x10000) >> 10)));
            out.push_back(char16_t(0xdc00 + ((c - 0x10000) & 0x3ff)));
        } else {
            out.push_back(char16_t(c));
        }
    }
    return out;
}

Kind KindOf(std::int32_t id) {
    if (id >= 920000 && id <= 929999) return Kind::Question;
    if (id >= 14000332 && id <= 14000399) return Kind::Event;
    return Kind::Menu;
}

Language LanguageOf(const std::string& folder) {
    return folder == "zhocn" ? Language::Chinese : folder == "rusru" ? Language::Russian : Language::English;
}

std::map<std::int32_t, PcMessage> PcMessages(bool chinese) {
    return PcMessages(chinese ? Language::Chinese : Language::English);
}

std::map<std::int32_t, PcMessage> PcMessages(Language language) {
    // Columns: 0 id, 1-2 English text and help, 3-4 Simplified Chinese, 5-6 Russian.
    const std::size_t translated_text = language == Language::Chinese ? 3 : language == Language::Russian ? 5 : 1;
    std::map<std::int32_t, PcMessage> out;
    const std::string all(kTsv);
    for (std::size_t at = 0; at < all.size();) {
        std::size_t end = all.find('\n', at);
        if (end == std::string::npos) end = all.size();
        std::string line = all.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        std::vector<std::string> column;
        for (std::size_t from = 0;;) {
            const std::size_t tab = line.find('\t', from);
            column.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
            if (tab == std::string::npos) break;
            from = tab + 1;
        }
        if (column.size() < 2) throw Error("pc_option_messages.tsv: a line without text: " + line);
        auto pick = [&](std::size_t english, std::size_t translated) {
            const std::size_t c = translated != english && column.size() > translated && !column[translated].empty() ? translated : english;
            return c < column.size() ? Unescape(column[c]) : std::string();
        };
        const std::int32_t id = std::stoi(column[0]);
        if (out.count(id)) throw Error("pc_option_messages.tsv: id " + column[0] + " twice");
        out[id] = PcMessage{pick(1, translated_text), pick(2, translated_text + 1)};
    }
    return out;
}

Dcx DcxRead(const Bytes& b) {
    if (!Has(b, 0, "DCX\0", 4) || !Has(b, 0x18, "DCS\0", 4) || !Has(b, 0x24, "DCP\0DFLT", 8) || !Has(b, 0x44, "DCA\0", 4))
        throw Error("not a DFLT DCX");
    const std::size_t start = 0x44 + std::size_t(Be32(b, 0x48));
    const std::size_t size = Be32(b, 0x1c), packed = Be32(b, 0x20);
    if (start > b.size() || packed > b.size() - start) throw Error("a DCX shorter than its header says");
    if (size > kMostBytes) throw Error("a DCX of more than 64 MiB");
    Dcx d{Bytes(b.begin(), b.begin() + start), Bytes(size)};
    z_stream z{};
    if (inflateInit(&z) != Z_OK) throw Error("zlib");
    z.next_in = const_cast<Bytef*>(b.data() + start);
    z.avail_in = uInt(b.size() - start);
    z.next_out = d.data.data();
    z.avail_out = uInt(size);
    const int r = inflate(&z, Z_FINISH);
    const bool whole = r == Z_STREAM_END && z.avail_out == 0;
    inflateEnd(&z);
    if (!whole) throw Error("a DCX whose data does not inflate to its size");
    return d;
}

Bytes DcxWrite(const Dcx& d) {
    uLongf packed = compressBound(uLong(d.data.size()));
    Bytes z(packed);
    if (compress2(z.data(), &packed, d.data.data(), uLong(d.data.size()), 9) != Z_OK) throw Error("zlib");
    Bytes out = d.header;
    SetBe32(out, 0x1c, std::uint32_t(d.data.size()));
    SetBe32(out, 0x20, std::uint32_t(packed));
    out.insert(out.end(), z.begin(), z.begin() + packed);
    return out;
}

Binder BinderRead(const Bytes& b) {
    if (!Has(b, 0, "BND4", 4) || b.size() < 0x40) throw Error("not a BND4");
    Binder r;
    r.header.assign(b.begin(), b.begin() + 0x40);
    r.entry_bytes = std::size_t(U64(b, 0x20));
    r.wide_names = b[0x30] != 0;
    if (r.entry_bytes < 0x24 || r.entry_bytes > 0x100 || U64(b, 0x38)) throw Error("a BND4 laid out unlike the game's");
    const std::uint32_t count = U32(b, 0x0c);
    if (count > 4096) throw Error("a BND4 of too many files");
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::size_t e = 0x40 + k * r.entry_bytes;
        BinderFile f;
        f.flags = U64(b, e);
        const std::uint64_t size = U64(b, e + 0x08), at = U32(b, e + 0x18);
        f.id = U32(b, e + 0x1c);
        const std::size_t name = U32(b, e + 0x20);
        if (at > b.size() || size > b.size() - at || name >= b.size()) throw Error("a BND4 entry past the end");
        std::size_t end = name;
        if (r.wide_names) end = WideEnd(b, name);
        else
            while (end < b.size() && b[end]) ++end;
        f.name.assign(b.begin() + name, b.begin() + end);
        f.data.assign(b.begin() + at, b.begin() + at + size);
        r.files.push_back(std::move(f));
    }
    return r;
}

Bytes BinderWrite(const Binder& r) {
    Bytes out = r.header;
    SetLe(out, 0x0c, r.files.size(), 4);
    const std::size_t names = 0x40 + r.files.size() * r.entry_bytes;
    out.resize(names, 0);
    std::vector<std::size_t> name_at;
    for (const BinderFile& f : r.files) {
        name_at.push_back(out.size());
        Append(out, f.name);
        out.resize(out.size() + (r.wide_names ? 2 : 1), 0);
    }
    SetLe(out, 0x28, out.size(), 8);
    for (std::size_t k = 0; k < r.files.size(); ++k) {
        const BinderFile& f = r.files[k];
        out.resize((out.size() + 15) & ~std::size_t(15), 0);
        const std::size_t e = 0x40 + k * r.entry_bytes;
        SetLe(out, e, f.flags, 8);
        SetLe(out, e + 0x08, f.data.size(), 8);
        SetLe(out, e + 0x10, f.data.size(), 8);
        SetLe(out, e + 0x18, out.size(), 4);
        SetLe(out, e + 0x1c, f.id, 4);
        SetLe(out, e + 0x20, name_at[k], 4);
        Append(out, f.data);
    }
    return out;
}

Fmg FmgRead(const Bytes& b) {
    if (b.size() < 0x28 || b[2] != 2) throw Error("an FMG that is not version 2");
    Fmg f;
    f.header.assign(b.begin(), b.begin() + 0x28);
    const std::uint32_t groups = U32(b, 0x0c), strings = U32(b, 0x10);
    const std::uint64_t offsets = U64(b, 0x18);
    for (std::uint32_t g = 0; g < groups; ++g) {
        const std::size_t at = 0x28 + std::size_t(g) * 16;
        const std::uint32_t index = U32(b, at);
        const auto first = std::int32_t(U32(b, at + 4)), last = std::int32_t(U32(b, at + 8));
        if (last < first || std::uint64_t(index) + std::uint64_t(last - first) >= strings) throw Error("an FMG group past its strings");
        for (std::int32_t id = first; id <= last; ++id) {
            const std::uint64_t text = U64(b, offsets + 8 * (index + std::uint64_t(id - first)));
            if (!text) {
                f.text[id] = std::nullopt;
                continue;
            }
            if (text >= b.size()) throw Error("an FMG string past the end");
            const std::size_t end = WideEnd(b, text);
            std::u16string s((end - text) / 2, u'\0');
            for (std::size_t i = 0; i < s.size(); ++i) s[i] = char16_t(U16(b, text + 2 * i));
            f.text[id] = std::move(s);
        }
    }
    return f;
}

Bytes FmgWrite(const Fmg& f) {
    struct Run {
        std::uint32_t index;
        std::int32_t first, last;
    };
    std::vector<Run> runs;
    std::uint32_t index = 0;
    for (const auto& [id, text] : f.text) {
        if (runs.empty() || runs.back().last + 1 != id) runs.push_back({index, id, id});
        else runs.back().last = id;
        ++index;
    }
    const std::size_t offsets = 0x28 + runs.size() * 16, pool = offsets + f.text.size() * 8;
    Bytes out = f.header;
    out.resize(pool, 0);
    std::map<std::u16string, std::size_t> placed;
    std::size_t k = 0;
    for (const auto& [id, text] : f.text) {
        std::size_t at = 0;
        if (text) {
            auto [it, fresh] = placed.emplace(*text, out.size());
            if (fresh) {
                for (const char16_t c : *text) PutLe(out, c, 2);
                PutLe(out, 0, 2);
            }
            at = it->second;
        }
        SetLe(out, offsets + 8 * k++, at, 8);
    }
    for (std::size_t g = 0; g < runs.size(); ++g) {
        SetLe(out, 0x28 + g * 16, runs[g].index, 4);
        SetLe(out, 0x28 + g * 16 + 4, std::uint32_t(runs[g].first), 4);
        SetLe(out, 0x28 + g * 16 + 8, std::uint32_t(runs[g].last), 4);
        SetLe(out, 0x28 + g * 16 + 12, 0, 4);
    }
    SetLe(out, 0x04, out.size(), 4);
    SetLe(out, 0x0c, runs.size(), 4);
    SetLe(out, 0x10, f.text.size(), 4);
    SetLe(out, 0x18, offsets, 8);
    return out;
}

Bytes MenuMessages(const Bytes& file, bool chinese) {
    return MenuMessages(file, chinese ? Language::Chinese : Language::English);
}

Bytes MenuMessages(const Bytes& file, Language language) {
    const bool packed = Has(file, 0, "DCX\0", 4);
    Dcx dcx = packed ? DcxRead(file) : Dcx{{}, file};
    Binder bundle = BinderRead(dcx.data);
    const auto messages = PcMessages(language);
    struct Group {
        std::uint32_t id;
        Kind kind;
        bool help;
        bool found = false;
    };
    Group groups[] = {{kMenuGroup, Kind::Menu, false}, {kHelpGroup, Kind::Menu, true}, {kQuestionGroup, Kind::Question, false},
                      {kScreenQuestionGroup, Kind::Question, false}, {kEventGroup, Kind::Event, false}};
    for (BinderFile& file_in : bundle.files) {
        Group* g = nullptr;
        for (Group& candidate : groups)
            if (candidate.id == file_in.id) g = &candidate;
        if (!g) continue;
        Fmg fmg = FmgRead(file_in.data);
        for (const auto& [id, m] : messages) {
            if (KindOf(id) != g->kind) continue;
            if (fmg.text.count(id))
                throw Error("message " + std::to_string(id) + " is already in group " + std::to_string(g->id));
            const std::string& s = g->help ? m.help : m.text;
            fmg.text[id] = s.empty() ? std::nullopt : std::optional<std::u16string>(Utf16(s));
        }
        file_in.data = FmgWrite(fmg);
        g->found = true;
    }
    for (const Group& g : groups)
        if (!g.found) throw Error("the message bundle has no group " + std::to_string(g.id));
    dcx.data = BinderWrite(bundle);
    return packed ? DcxWrite(dcx) : dcx.data;
}

// Nightwatch: Environment gets a row under Display HUD (the port's Auto-Hide HUD, gpu/shim/bbport_game_menu.cpp
// adds it as the screen's third row): a copy of Display HUD's slot one row lower as Item_2_0; Music .. Voice and
// Defaults one slot on (Item_3_0 ..); everything under Display HUD (the Sound band and header, its rows, the
// help line, the window's lower edge) one row lower; the window's background and the Display section's line one
// row taller (pictures keep their size), in every frame (the screen slides in). Only EnvironmentSetting places its
// window (checked), so no other screen changes.
namespace {
struct Span {
    std::int64_t top = 0, bottom = 0, left = 0, right = 0;  // twips
};
std::optional<Span> CharacterSpan(const Movie& m, std::uint16_t id) {
    for (const Tag& t : m.tags) {
        if (t.code == kSprite && U16(t.body, 0) == id) {
            std::optional<Span> only;
            int parts = 0;
            for (const Tag& c : ReadSprite(t).tags) {
                if (!IsPlace(c)) continue;
                const Place p = ReadPlace(c);
                if (!p.character) continue;
                if (++parts > 1 || (p.matrix && (p.matrix->scaled || p.matrix->rotated))) return std::nullopt;
                if (auto s = CharacterSpan(m, *p.character)) {
                    const std::int64_t dx = p.matrix ? p.matrix->tx : 0, dy = p.matrix ? p.matrix->ty : 0;
                    only = Span{s->top + dy, s->bottom + dy, s->left + dx, s->right + dx};
                }
            }
            return only;
        }
        if ((t.code == 2 || t.code == 22 || t.code == 32 || t.code == 83) && U16(t.body, 0) == id) {
            BitReader r{t.body, 2};
            const int n = int(r.Unsigned(5));
            const std::int64_t left = r.Signed(n), right = r.Signed(n), top = r.Signed(n);
            return Span{top, r.Signed(n), left, right};
        }
    }
    return std::nullopt;
}
Matrix ScaledY(Matrix m, double by, std::int64_t top) {
    const double sx = m.scaled ? Fixed(m.sx, m.scale_bits) : 1, sy = m.scaled ? Fixed(m.sy, m.scale_bits) : 1;
    const std::int64_t x = std::llround(sx * 65536), y = std::llround(sy * by * 65536);
    m.ty -= std::llround(double(top) * sy * (by - 1));  // the top edge stays where it was
    m.scaled = true;
    m.scale_bits = std::max(BitsFor(x), BitsFor(y));
    const std::uint64_t mask = (std::uint64_t(1) << m.scale_bits) - 1;
    m.sx = std::uint32_t(std::uint64_t(x) & mask), m.sy = std::uint32_t(std::uint64_t(y) & mask);
    return m;
}
void EnvironmentRow(Movie& m) {
    const Place placed = ReadPlace(m.tags[FindPlaced(m, "EnvironmentSetting")]);
    if (!placed.character) throw Error("EnvironmentSetting is placed without a character");
    const std::size_t env_at = FindSprite(m, *placed.character);
    Sprite env = ReadSprite(m.tags[env_at]);
    std::map<std::string, Place> named;
    std::set<std::uint16_t> depths;
    std::optional<Place> panel;
    std::size_t panel_parts = 0;
    for (const Tag& t : env.tags) {
        if (t.code == kShowFrame) break;  // the first frame places everything
        if (!IsPlace(t)) continue;
        const Place p = ReadPlace(t);
        depths.insert(p.depth);
        if (p.name) {
            named[*p.name] = p;
        } else if (p.character && p.matrix) {  // the window: the unnamed sprite of the most parts
            for (const Tag& s : m.tags) {
                if (s.code != kSprite || U16(s.body, 0) != *p.character) continue;
                std::size_t parts = 0;
                for (const Tag& c : ReadSprite(s).tags) parts += IsPlace(c) && ReadPlace(c).character;
                if (parts > panel_parts) panel_parts = parts, panel = p;
            }
        }
    }
    for (const char* need : {"Item_0_0", "Item_1_0", "Item_2_0", "Item_3_0", "Item_4_0", "Item_5_0", "StaticText_111011",
                             "StatusBar"})
        if (!named.count(need) || !named[need].matrix) throw Error(std::string("EnvironmentSetting has no ") + need);
    if (named.count("Item_6_0") || !panel || panel_parts < 4)
        throw Error("EnvironmentSetting is not laid out as 1.09's (five rows, Defaults, a window)");
    for (const Tag& s : m.tags)  // the window is EnvironmentSetting's own
        if (s.code == kSprite && U16(s.body, 0) != env.id)
            for (const Tag& c : ReadSprite(s).tags)
                if (IsPlace(c) && ReadPlace(c).character == panel->character)
                    throw Error("EnvironmentSetting's window is shared with another sprite");
    const std::int64_t pitch = named["Item_1_0"].matrix->ty - named["Item_0_0"].matrix->ty;
    const std::int64_t split = named["Item_1_0"].matrix->ty;
    if (pitch <= 0) throw Error("EnvironmentSetting's rows are not laid out downwards");
    std::map<std::uint16_t, std::optional<std::string>> moved;  // depth -> its new name (rows) or as it was
    for (int r = 2; r <= 5; ++r) moved[named[RowName(r)].depth] = RowName(r + 1);
    moved[named["StaticText_111011"].depth] = std::nullopt;
    moved[named["StatusBar"].depth] = std::nullopt;
    const std::uint16_t row1 = named["Item_1_0"].depth;
    std::uint16_t added = row1 + 1;
    while (depths.count(added)) ++added;

    std::vector<Tag> tags;
    for (const Tag& t : env.tags) {
        if (!IsPlace(t)) {
            tags.push_back(t);
            continue;
        }
        const Place p = ReadPlace(t);
        if (const auto it = moved.find(p.depth); it != moved.end() && p.matrix) {
            Matrix lower = *p.matrix;
            lower.ty += pitch;
            tags.push_back(Changed(t.code, Rewrite(t, p, {}, {}, lower, p.name ? it->second : std::nullopt)));
        } else {
            tags.push_back(t);
        }
        if (p.depth == row1 && p.matrix) {  // Display HUD's slot, one row lower: the new row's
            Matrix lower = *p.matrix;
            lower.ty += pitch;
            tags.push_back(
                Changed(t.code, Rewrite(t, p, added, {}, lower, p.name ? std::optional<std::string>(RowName(2)) : std::nullopt)));
        }
    }
    env.tags = std::move(tags);

    const std::size_t panel_at = FindSprite(m, *panel->character);
    Sprite window = ReadSprite(m.tags[panel_at]);
    const std::int64_t window_split = split - panel->matrix->ty;  // in the window's own space
    // Each part by its edges (its top, not its origin: the right-hand stone is the left one flipped, scale -1, its
    // origin its lower corner), decided from the first frame, per depth: under Display HUD -> one row lower; across
    // it -> the background and lines (narrower than a row) one row taller, their top kept; pictures as they are.
    struct Grow {
        double by;
        std::int64_t top;  // the part's own top (unscaled), kept in place
    };
    std::set<std::uint16_t> lower_parts;
    std::map<std::uint16_t, Grow> grown;
    std::optional<std::uint16_t> back;
    std::int64_t tallest = 0;
    struct Seen {
        std::uint16_t depth;
        Span span;
        double sy;
        std::int64_t top, bottom, width;
    };
    std::vector<Seen> seen;
    for (const Tag& t : window.tags) {
        if (t.code == kShowFrame) break;
        if (!IsPlace(t)) continue;
        const Place p = ReadPlace(t);
        if (!p.character || !p.matrix) continue;
        const auto s = CharacterSpan(m, *p.character);
        const double sx = p.matrix->scaled ? Fixed(p.matrix->sx, p.matrix->scale_bits) : 1;
        const double sy = p.matrix->scaled ? Fixed(p.matrix->sy, p.matrix->scale_bits) : 1;
        if (!s) {
            if (p.matrix->ty > window_split) lower_parts.insert(p.depth);
            continue;
        }
        const std::int64_t top = p.matrix->ty + std::llround(std::min(sy * double(s->top), sy * double(s->bottom)));
        const std::int64_t bottom = p.matrix->ty + std::llround(std::max(sy * double(s->top), sy * double(s->bottom)));
        seen.push_back({p.depth, *s, sy, top, bottom, std::llround(std::abs(sx) * double(s->right - s->left))});
        if (bottom - top > tallest) tallest = bottom - top, back = p.depth;
    }
    if (!back || tallest <= pitch) throw Error("EnvironmentSetting's window has no background to grow");
    for (const Seen& s : seen) {
        if (s.top > window_split) {
            lower_parts.insert(s.depth);
        } else if (s.bottom > window_split && s.sy > 0 && (s.depth == *back || s.width < pitch)) {
            grown[s.depth] = Grow{double(s.bottom - s.top + pitch) / double(s.bottom - s.top), s.span.top};
        }
    }
    if (!grown.count(*back)) throw Error("EnvironmentSetting's window background does not reach under Display HUD");
    std::vector<Tag> parts;
    for (const Tag& t : window.tags) {
        const Place p = IsPlace(t) ? ReadPlace(t) : Place{};
        if (!IsPlace(t) || !p.matrix) {
            parts.push_back(t);
        } else if (const auto g = grown.find(p.depth); g != grown.end()) {
            const Matrix taller = ScaledY(*p.matrix, g->second.by, g->second.top);
            parts.push_back(Changed(t.code, Rewrite(t, p, {}, {}, taller, {})));
        } else if (lower_parts.count(p.depth)) {
            Matrix lower = *p.matrix;
            lower.ty += pitch;
            parts.push_back(Changed(t.code, Rewrite(t, p, {}, {}, lower, {})));
        } else {
            parts.push_back(t);
        }
    }
    window.tags = std::move(parts);
    m.tags[env_at] = WriteSprite(env);
    m.tags[panel_at] = WriteSprite(window);
}
}  // namespace

Bytes OptionMovie(const Bytes& gfx) {
    Movie m = ReadMovie(gfx);
    if (Placed(m, "EnvironmentSetting")) EnvironmentRow(m);
    for (const Section& s : kSections)
        if (Placed(m, s.name)) throw Error(std::string("the movie already has ") + s.name);
    const Donor d = ReadDonor(m);
    std::uint16_t next = HighestCharacter(m);
    std::vector<Tag> sprites, placements;
    std::vector<std::pair<std::uint16_t, std::string>> classes;
    std::uint16_t depth = kFirstSectionDepth;
    for (const Section& s : kSections) {
        if (std::to_string(s.title).size() + 11 != d.title_field.size() || s.rows < 1 || s.rows > 8)
            throw Error(std::string("section ") + s.name + " does not fit the donor");
        const std::uint16_t id = ++next, title = ++next;
        sprites.push_back(WriteSprite(TitleSprite(d, s, title)));
        sprites.push_back(WriteSprite(SectionSprite(d, s, id, title)));
        placements.push_back(HiddenPlacement(id, depth++, d.placed, s.name));
        if (d.class_name) classes.emplace_back(id, *d.class_name);
    }
    if (depth > 500) throw Error("the sections' depths reach Top's");
    m.tags.insert(m.tags.begin() + std::ptrdiff_t(d.tag) + 1, sprites.begin(), sprites.end());
    const std::size_t top = FindPlaced(m, "Top");
    m.tags.insert(m.tags.begin() + std::ptrdiff_t(top) + 1, placements.begin(), placements.end());
    if (!classes.empty()) AddClasses(m, classes);
    AddScrollBar(m);
    return WriteMovie(m);
}

namespace {
// A matrix stretched across: x scaled by `by`, y and the position as they were.
Matrix ScaledX(Matrix m, double by) {
    const double sx = m.scaled ? Fixed(m.sx, m.scale_bits) : 1, sy = m.scaled ? Fixed(m.sy, m.scale_bits) : 1;
    const std::int64_t x = std::llround(sx * by * 65536), y = std::llround(sy * 65536);
    m.scaled = true;
    m.scale_bits = std::max(BitsFor(x), BitsFor(y));
    const std::uint64_t mask = (std::uint64_t(1) << m.scale_bits) - 1;
    m.sx = std::uint32_t(std::uint64_t(x) & mask), m.sy = std::uint32_t(std::uint64_t(y) & mask);
    return m;
}
// A sprite part's placement (PlaceObject2 or 3, placing a character) with the instance name `name` when it has none.
Tag NamedPart(const Tag& t, const std::optional<std::string>& name) {
    const Place p = ReadPlace(t);
    if (!p.character || (p.flags & (0x01 | 0x80))) throw Error("ItemDetailText's parts are not 1.09's");
    if (!name || p.name) return t;
    Bytes body = t.body;
    body[0] |= 0x20;
    body.insert(body.begin() + std::ptrdiff_t(p.name_at), name->begin(), name->end());
    body.insert(body.begin() + std::ptrdiff_t(p.name_at + name->size()), 0);
    return Changed(t.code, std::move(body));
}
// The same part placed hidden (PlaceObject3 with visible 0), named `name` when it has no name of its own.
Tag HiddenPart(const Tag& t, const std::optional<std::string>& name) {
    const Tag named = NamedPart(t, name);
    Bytes body = named.body;
    if (named.code == kPlace2) {
        body.insert(body.begin() + 1, 0x20);
    } else {
        if (body[1] & (0x20 | 0x40)) throw Error("ItemDetailText's parts are not 1.09's");
        body[1] |= 0x20;
    }
    body.push_back(0);  // not visible
    body.push_back(0);
    return Changed(kPlace3, std::move(body));
}
// A filled rectangle (DefineShape) from (0, 0), `width` x `height` twips, in `rgb` (white: a mask).
Tag RectShape(std::uint16_t id, std::int64_t width, std::int64_t height, std::uint32_t rgb = 0xffffff) {
    Bytes body;
    PutLe(body, id, 2);
    BitWriter bounds;
    const int n = std::max(BitsFor(width), BitsFor(height));
    bounds.Put(std::uint64_t(n), 5);
    for (const std::int64_t v : {std::int64_t(0), width, std::int64_t(0), height})
        bounds.Put(std::uint64_t(v) & ((std::uint64_t(1) << n) - 1), n);
    Append(body, bounds.out);
    body.insert(body.end(), {0x01, 0x00, std::uint8_t(rgb >> 16), std::uint8_t(rgb >> 8), std::uint8_t(rgb),
                             0x00});  // one solid fill, no line styles
    BitWriter shape;
    shape.Put(1, 4);  // fill index bits
    shape.Put(0, 4);  // line index bits
    shape.Put(0b000011, 6);  // a style change: fill style 0 and a move
    shape.Put(1, 5), shape.Put(0, 1), shape.Put(0, 1);  // move to (0, 0)
    shape.Put(1, 1);  // fill style 0 = 1
    const int bits = std::max({BitsFor(width), BitsFor(-width), BitsFor(height), BitsFor(-height)});
    const auto edge = [&](bool vertical, std::int64_t delta) {
        shape.Put(0b11, 2);
        shape.Put(std::uint64_t(bits - 2), 4);
        shape.Put(0, 1);  // a horizontal or vertical line
        shape.Put(vertical ? 1 : 0, 1);
        shape.Put(std::uint64_t(delta) & ((std::uint64_t(1) << bits) - 1), bits);
    };
    edge(false, width), edge(true, height), edge(false, -width), edge(true, -height);
    shape.Put(0, 6);  // the end
    Append(body, shape.out);
    return Changed(2, std::move(body));
}
// An edit text (DefineEditText) as character `id`, its bounds `by` twips wider (the right edge moved).
Tag WiderEditText(const Movie& m, std::uint16_t of, std::uint16_t id, std::int64_t by) {
    for (const Tag& t : m.tags) {
        if (t.code != 37 || U16(t.body, 0) != of) continue;
        BitReader r{t.body, 2};
        const int n = int(r.Unsigned(5));
        const std::int64_t left = r.Signed(n), right = r.Signed(n), top = r.Signed(n), bottom = r.Signed(n);
        const std::size_t rest = r.End();
        const int bits = std::max({BitsFor(left), BitsFor(right + by), BitsFor(top), BitsFor(bottom)});
        BitWriter w;
        w.Put(std::uint64_t(bits), 5);
        for (const std::int64_t v : {left, right + by, top, bottom}) w.Put(std::uint64_t(v) & ((std::uint64_t(1) << bits) - 1), bits);
        Bytes body;
        PutLe(body, id, 2);
        Append(body, w.out);
        body.insert(body.end(), t.body.begin() + std::ptrdiff_t(rest), t.body.end());
        return Changed(37, std::move(body));
    }
    throw Error("no edit text " + std::to_string(of) + " in the movie");
}
// A copy of sprite `of` as `id`, `by` twips wider: each wide part (over 100 px) stretched across, each narrow part
// that sits right of 100 px moved right; `character` (if set) placed in place of the sprite's only character.
Sprite WiderSprite(const Movie& m, std::uint16_t of, std::uint16_t id, std::int64_t by,
                   std::optional<std::uint16_t> character = std::nullopt) {
    Sprite s = ReadSprite(m.tags[FindSprite(m, of)]);
    s.id = id;
    for (Tag& t : s.tags) {
        if (!IsPlace(t)) continue;
        const Place p = ReadPlace(t);
        if (!p.character || !p.matrix || p.matrix->rotated) throw Error("ItemDetailText's parts are not 1.09's");
        if (character) {
            t = Changed(t.code, Rewrite(t, p, {}, character, {}, {}));
            continue;
        }
        const auto span = CharacterSpan(m, *p.character);
        if (!span) throw Error("ItemDetailText's parts are not 1.09's");
        const std::int64_t width = span->right - span->left;
        if (width > 100 * 20) {
            t = Changed(t.code, Rewrite(t, p, {}, {}, ScaledX(*p.matrix, double(width + by) / double(width)), {}));
        } else if (p.matrix->tx > 100 * 20) {
            Matrix at = *p.matrix;
            at.tx += by;
            t = Changed(t.code, Rewrite(t, p, {}, {}, at, {}));
        }
    }
    return s;
}
}  // namespace

namespace {
// A matrix with the scale set to (sx, sy), its position as it was.
Matrix WithScale(Matrix m, double sx, double sy) {
    const std::int64_t x = std::llround(sx * 65536), y = std::llround(sy * 65536);
    m.scaled = true;
    m.scale_bits = std::max(BitsFor(x), BitsFor(y));
    const std::uint64_t mask = (std::uint64_t(1) << m.scale_bits) - 1;
    m.sx = std::uint32_t(std::uint64_t(x) & mask), m.sy = std::uint32_t(std::uint64_t(y) & mask);
    return m;
}
// A copy of edit text `of` (DefineEditText) as `id` with the initial text `text`.
Tag EditTextWithText(const Movie& m, std::uint16_t of, std::uint16_t id, const std::string& text) {
    for (const Tag& t : m.tags) {
        if (t.code != 37 || U16(t.body, 0) != of) continue;
        const Bytes& b = t.body;
        BitReader r{b, 2};
        const int n = int(r.Unsigned(5));
        for (int i = 0; i < 4; ++i) r.Signed(n);
        std::size_t at = r.End();
        const std::size_t flags_at = at;
        const std::uint8_t f1 = b[at], f2 = b[at + 1];
        at += 2;
        if (f1 & 0x01) at += 2;  // the font
        if (f2 & 0x80) {         // the font class
            while (at < b.size() && b[at]) ++at;
            ++at;
        }
        if ((f1 & 0x01) || (f2 & 0x80)) at += 2;  // the height
        if (f1 & 0x04) at += 4;                   // the colour
        if (f1 & 0x02) at += 2;                   // the most characters
        if (f2 & 0x20) at += 9;                   // the layout
        while (at < b.size() && b[at]) ++at;      // the variable name
        if (at >= b.size()) throw Error("the list's text boxes are not 1.09's");
        Bytes body(b.begin(), b.begin() + std::ptrdiff_t(at + 1));
        SetLe(body, 0, id, 2);
        body[flags_at] |= 0x80;
        if (!text.empty() && text[0] == '<') body[flags_at + 1] |= 0x02;  // HTML text
        body.insert(body.end(), text.begin(), text.end());
        body.push_back(0);
        return Changed(37, std::move(body));
    }
    throw Error("no edit text " + std::to_string(of) + " in the movie");
}
// A placement (PlaceObject3) of character `id` at `depth`, named `name` if set, hidden unless `visible`.
Tag Placement(std::uint16_t id, std::uint16_t depth, const Matrix& at, const std::optional<std::string>& name,
              bool visible) {
    Bytes b{std::uint8_t(0x02 | 0x04 | (name ? 0x20 : 0)), std::uint8_t(visible ? 0x00 : 0x20)};
    PutLe(b, depth, 2);
    PutLe(b, id, 2);
    Append(b, WriteMatrix(at));
    if (name) {
        b.insert(b.end(), name->begin(), name->end());
        b.push_back(0);
    }
    if (!visible) b.push_back(0);
    return Changed(kPlace3, std::move(b));
}
Matrix At(double x, double y) {
    Matrix m;
    m.tx = std::llround(x * 20), m.ty = std::llround(y * 20);
    return m;
}
// A character's bounds, twips: a shape's (DefineShape 1-4) own; a sprite's, its first frame's parts' (placed
// without rotation).
struct Box {
    std::int64_t x0, x1, y0, y1;
};
double ScaleOf(std::uint32_t raw, int bits) {
    const std::int64_t v = bits && (raw >> (bits - 1) & 1) ? std::int64_t(raw) - (std::int64_t(1) << bits)
                                                            : std::int64_t(raw);
    return double(v) / 65536;
}
Box Bounds(const Movie& m, std::uint16_t id) {
    for (const Tag& t : m.tags) {
        if ((t.code != 2 && t.code != 22 && t.code != 32 && t.code != 83) || U16(t.body, 0) != id) continue;
        BitReader r{t.body, 2};
        const int n = int(r.Unsigned(5));
        Box b{};
        b.x0 = r.Signed(n), b.x1 = r.Signed(n), b.y0 = r.Signed(n), b.y1 = r.Signed(n);
        return b;
    }
    const Sprite s = ReadSprite(m.tags[FindSprite(m, id)]);
    std::optional<Box> all;
    for (const Tag& t : s.tags) {
        if (t.code == kShowFrame) break;
        if (!IsPlace(t)) continue;
        const Place p = ReadPlace(t);
        if (!p.character) continue;
        const Matrix at = p.matrix.value_or(Matrix{});
        if (at.rotated) throw Error("a rotated part in character " + std::to_string(id));
        const double sx = at.scaled ? ScaleOf(at.sx, at.scale_bits) : 1, sy = at.scaled ? ScaleOf(at.sy, at.scale_bits) : 1;
        const Box c = Bounds(m, *p.character);
        const std::int64_t xa = at.tx + std::llround(c.x0 * sx), xb = at.tx + std::llround(c.x1 * sx);
        const std::int64_t ya = at.ty + std::llround(c.y0 * sy), yb = at.ty + std::llround(c.y1 * sy);
        const Box b{std::min(xa, xb), std::max(xa, xb), std::min(ya, yb), std::max(ya, yb)};
        all = all ? Box{std::min(all->x0, b.x0), std::max(all->x1, b.x1), std::min(all->y0, b.y0),
                        std::max(all->y1, b.y1)}
                  : b;
    }
    if (!all) throw Error("character " + std::to_string(id) + " has no parts");
    return *all;
}
}  // namespace

// The Chalice glyph screen's description panel: a second copy of ItemDetailText (the sprite placed at root depth 1)
// at depth 2, named GlyphDetailText, with only its right-hand column (name, underline, ruled lines, description) on a
// background of its own (the panel's, narrowed: the edges kept, the middle stretched), moved right so it sits beside
// the item cards. It follows ItemDetailText's FadeIn / Loop / FadeOut moves.
//
// Which panel shows is set on the parts inside the two panels, never on the panels: a display object changed by code
// no longer follows the timeline's fade. GlyphDetailText's parts start hidden (the glyph screen shows them), and the
// unnamed parts of both panels get names (GlyphPart_<depth>) so that bbport_glyph_screen.cpp can reach them.
// ItemDetailText otherwise stays as it is.
Bytes GlyphPanelMovie(const Bytes& gfx) {
    constexpr std::uint16_t kPanelDepth = 1, kGlyphDepth = 2;
    // px: the panel moved right so the column's frame (from x 440 in the panel) starts at ~948 on screen
    constexpr double kShift = 298;
    // px, in the panel: the new background from 419 (on screen ~927: the same gap after the cards as the item status
    // panel's, measured) to 1309 (on screen ~1817, where the stats panel ends, measured); the game panel's right edge
    // is at 1162, so the column's parts are 147 px wider than the game's
    constexpr double kBackLeft = 419, kBackRight = 1309, kGameRight = 1162;
    constexpr std::int64_t kWider = std::int64_t((kBackRight - kGameRight) * 20);  // twips
    // the column's parts by depth: 1 the background, 15 / 19 the long ornaments along the top and bottom (narrowed to
    // the background, the same margins), 23 the name's underline, 34 the ruled lines, 74 DetailDesc, 76 ItemName
    const std::set<std::uint16_t> keep = {1, 15, 19, 23, 34, 74, 76};
    Movie m = ReadMovie(gfx);
    if (Placed(m, "GlyphDetailText")) throw Error("the movie already has GlyphDetailText");
    const std::size_t first = FindPlaced(m, "ItemDetailText");
    const Place panel = ReadPlace(m.tags[first]);
    if (panel.depth != kPanelDepth || !panel.character || !panel.matrix) throw Error("ItemDetailText is not 1.09's");
    for (const Tag& t : m.tags)
        if (IsPlace(t) && ReadPlace(t).depth == kGlyphDepth) throw Error("the movie already uses depth 2");
    const std::size_t sprite_at = FindSprite(m, *panel.character);
    if (sprite_at > first) throw Error("ItemDetailText's timeline is not 1.09's");
    Sprite game = ReadSprite(m.tags[sprite_at]);
    if (game.frames != 1) throw Error("ItemDetailText's sprite is not 1.09's");
    const auto part_name = [](std::uint16_t depth) { return "GlyphPart_" + std::to_string(depth); };

    // The background: the panel's (fill, left edge, right edge, top and bottom lines) with the fill and the lines
    // stretched to the new width and the edges at their own width.
    std::optional<std::uint16_t> back_id;
    for (const Tag& t : game.tags)
        if (IsPlace(t) && ReadPlace(t).depth == 1) back_id = ReadPlace(t).character;
    if (!back_id) throw Error("ItemDetailText has no background");
    Sprite back = ReadSprite(m.tags[FindSprite(m, *back_id)]);
    back.id = std::uint16_t(HighestCharacter(m) + 1);
    // 1 the fill and 7 / 9 the lines run from x 5.05 to 1157.05; 3 the left edge (serrated, 5.05 wide) at 0; 5 the
    // right edge (a plain line: in the game it meets the stats panel) at 1157.05. Ours has the left edge mirrored on
    // the right too.
    constexpr double kFillLeft = 5.05, kFillFull = 1152, kEdge = 5.05;
    constexpr double kEdges = 2 * kEdge;
    std::optional<Place> left_edge;
    const double fill = (kBackRight - kBackLeft) - kEdges;
    const double by = fill / kFillFull;
    std::set<std::uint16_t> back_parts;
    for (Tag& t : back.tags) {
        if (!IsPlace(t)) continue;
        const Place p = ReadPlace(t);
        if (!p.matrix || p.matrix->rotated) throw Error("ItemDetailText's background is not 1.09's");
        back_parts.insert(p.depth);
        if (p.depth == 1 || p.depth == 7 || p.depth == 9) {
            t = Changed(t.code, Rewrite(t, p, {}, {}, ScaledX(*p.matrix, by), {}));
        } else if (p.depth == 3) {
            left_edge = p;
        } else if (p.depth == 5) {
            if (!left_edge || !left_edge->character || left_edge->matrix->scaled) throw Error("ItemDetailText's background is not 1.09's");
            Matrix at = ScaledX(*left_edge->matrix, -1);
            at.tx = std::llround((kFillLeft + fill + kEdge) * 20);
            at.ty = left_edge->matrix->ty;
            t = Changed(t.code, Rewrite(t, p, {}, left_edge->character, at, {}));
        }
    }
    if (back_parts != std::set<std::uint16_t>{1, 3, 5, 7, 9}) throw Error("ItemDetailText's background is not 1.09's");

    // The column's own parts, kWider wider than the game's (which stay as they are): the name's underline (one shape,
    // stretched), the ruled lines (the horizontal lines stretched, the right-hand line moved) and the name and
    // description text boxes (their edit texts' bounds widened).
    std::map<std::uint16_t, std::uint16_t> wider;  // the game's part -> the column's
    std::vector<Tag> wider_tags;
    std::optional<std::uint16_t> name_text;  // ItemName's edit text (the gold one)
    std::uint16_t next = back.id;
    for (const Tag& t : game.tags) {
        if (!IsPlace(t)) continue;
        const Place p = ReadPlace(t);
        if (!p.character) continue;
        if (p.depth == 23 || p.depth == 34) {
            wider[*p.character] = ++next;
            wider_tags.push_back(WriteSprite(WiderSprite(m, *p.character, next, kWider)));
        } else if (p.depth == 74 || p.depth == 76) {
            // a text box: a sprite holding one edit text (Value_0)
            const Sprite box = ReadSprite(m.tags[FindSprite(m, *p.character)]);
            std::optional<std::uint16_t> text;
            for (const Tag& c : box.tags)
                if (IsPlace(c)) text = ReadPlace(c).character;
            if (!text) throw Error("ItemDetailText's text boxes are not 1.09's");
            if (p.depth == 76) name_text = text;
            const std::uint16_t text_id = ++next;
            wider_tags.push_back(WiderEditText(m, *text, text_id, kWider));
            wider[*p.character] = ++next;
            wider_tags.push_back(WriteSprite(WiderSprite(m, *p.character, next, kWider, text_id)));
        }
    }
    if (wider.size() != 4) throw Error("ItemDetailText's column is not 1.09's");

    // GlyphDetailText: the column, every part hidden and named.
    Sprite column = game;
    column.id = std::uint16_t(next + 1);
    column.tags.clear();
    std::size_t kept = 0;
    for (const Tag& t : game.tags) {
        if (!IsPlace(t)) {
            if (t.code != kShowFrame && t.code != kEnd) throw Error("ItemDetailText's sprite is not 1.09's");
            column.tags.push_back(t);
            continue;
        }
        const Place p = ReadPlace(t);
        if (!keep.count(p.depth)) continue;
        ++kept;
        if (p.depth == 1) {
            if (!p.matrix || p.matrix->scaled || p.matrix->rotated || p.matrix->tx || p.matrix->ty)
                throw Error("ItemDetailText's background is not 1.09's");
            Matrix at = *p.matrix;
            at.tx = std::llround(kBackLeft * 20);
            const Tag moved = Changed(t.code, Rewrite(t, p, {}, back.id, at, {}));
            column.tags.push_back(HiddenPart(moved, part_name(p.depth)));
        } else if (p.depth == 15 || p.depth == 19) {
            const auto span = p.matrix && p.character ? CharacterSpan(m, *p.character) : std::nullopt;
            if (!span || p.matrix->rotated) throw Error("ItemDetailText's ornaments are not 1.09's");
            const double width = double(span->right - span->left) / 20;
            const double margin_left = double(p.matrix->tx) / 20, margin_right = kGameRight - margin_left - width;
            Matrix at = ScaledX(*p.matrix, (kBackRight - kBackLeft - margin_left - margin_right) / width);
            at.tx = std::llround((kBackLeft + margin_left) * 20);
            const Tag narrowed = Changed(t.code, Rewrite(t, p, {}, {}, at, {}));
            column.tags.push_back(HiddenPart(narrowed, part_name(p.depth)));
        } else {
            const Tag own = Changed(t.code, Rewrite(t, p, {}, wider.at(*p.character), {}, {}));
            column.tags.push_back(HiddenPart(own, p.name ? std::nullopt : std::optional<std::string>(part_name(p.depth))));
        }
    }
    if (kept != keep.size()) throw Error("ItemDetailText's sprite is not 1.09's");

    // Two rows under the description box, GlyphDepth and GlyphArea (filled at run time), laid out as the game's
    // No. Held / Stored rows (Item_*_TP_Base): the label Text_0, the right-aligned value Value_0 and the held rows'
    // curled underline, at the held rows' positions from the row's left: the value's right end 400, the underline
    // -13.65 (its left) and 27.25 down. The Area row is at the second held row's height (605.85), the Depth row 63
    // above it (8 more than the held rows' 55) so the Area icon clears the Depth underline. In front, as in the chalice
    // status panel's rows, the Depth / Area icon (32 x 32), 6 px up so it clears its own underline (its curl starts 32
    // down), with the label 12 px after it; the label and value 3 px up. The row's left is set so the icon lines up
    // with the description text.
    //
    // The icons are cells of the menu icon sheet MENU_Icon_07001 (the movie's image MENU_DummyIcon_07001 is that whole
    // sheet: 32 x 32 cells, 32 across; icon id 7000 + row * 32 + column, as the game's icon lookup 0x1ad3020 has it):
    // Depth 7078 (row 2, column 14), Area 7077 (row 2, column 13). Each is the sheet behind a 32 x 32 mask, moved so
    // the cell shows.
    constexpr double kRowX = 449, kRowsY = 542.85, kRowPitch = 63, kLabelX = 44, kValueRight = 400, kValueWidth = 171.05;
    constexpr double kTextY = 3, kIconY = -6;
    constexpr int kIconSize = 32;
    constexpr int kIconCells[2][2] = {{14, 2}, {13, 2}};  // column, row: Depth, Area
    constexpr double kLineX = -13.65, kLineY = 27.25;
    std::optional<Tag> label, value, line;
    for (const Tag& t : game.tags) {
        if (!IsPlace(t)) continue;
        const Place p = ReadPlace(t);
        if (p.name == "Item_0_TP_Base") {
            for (const Tag& c : ReadSprite(m.tags[FindSprite(m, *p.character)]).tags) {
                if (!IsPlace(c)) continue;
                const Place q = ReadPlace(c);
                if (q.name == "Text_0") label = c;
                if (q.name == "Value_0") value = c;
            }
        }
        if (p.depth == 26) line = t;  // the held row's underline
    }
    if (!label || !value || !line) throw Error("ItemDetailText's rows are not 1.09's");
    const auto moved = [](const Tag& t, double x, double y, std::optional<std::uint16_t> depth) {
        const Place p = ReadPlace(t);
        Matrix at = p.matrix.value_or(Matrix{});
        at.tx = std::llround(x * 20), at.ty = std::llround(y * 20);
        return Changed(t.code, Rewrite(t, p, depth, {}, at, {}));
    };
    std::uint16_t sheet = 0;
    for (const auto& [id, name] : SymbolClasses(m))
        if (name == "MENU_DummyIcon_07001") sheet = id;
    if (!sheet) throw Error("ItemDetailText has no MENU_DummyIcon_07001");
    std::vector<Tag> row_defs;
    std::vector<Tag> rows;
    std::uint16_t next_id = column.id;
    const std::uint16_t mask = ++next_id;
    row_defs.push_back(RectShape(mask, kIconSize * 20, kIconSize * 20));
    const char* names[] = {"GlyphDepth", "GlyphArea"};
    for (int i = 0; i < 2; ++i) {
        Sprite icon{++next_id, 1, {}};
        Bytes clip{0x02 | 0x04 | 0x40};  // the mask: character, matrix, clip depth
        PutLe(clip, 1, 2);
        PutLe(clip, mask, 2);
        Append(clip, WriteMatrix(Matrix{}));
        PutLe(clip, 2, 2);
        icon.tags.push_back(Changed(kPlace2, clip));
        Bytes image{0x06, 0x10};  // the sheet: an image character (PlaceObject3), as the game's icon parts place it
        PutLe(image, 2, 2);
        PutLe(image, sheet, 2);
        Matrix at;
        at.tx = -std::int64_t(kIconCells[i][0]) * kIconSize * 20, at.ty = -std::int64_t(kIconCells[i][1]) * kIconSize * 20;
        Append(image, WriteMatrix(at));
        icon.tags.push_back(Changed(kPlace3, image));
        icon.tags.push_back(Tag{kShowFrame, {}, {}});
        icon.tags.push_back(Tag{kEnd, {}, {}});
        Sprite row{++next_id, 1, {}};
        Bytes icon_place{0x02 | 0x04};
        PutLe(icon_place, 1, 2);
        PutLe(icon_place, icon.id, 2);
        Matrix icon_at;
        icon_at.ty = std::llround(kIconY * 20);
        Append(icon_place, WriteMatrix(icon_at));
        row.tags.push_back(Changed(kPlace2, icon_place));
        row.tags.push_back(moved(*label, kLabelX, kTextY, std::uint16_t(3)));
        row.tags.push_back(moved(*value, kValueRight - kValueWidth, kTextY, std::uint16_t(5)));
        row.tags.push_back(moved(*line, kLineX, kLineY, std::uint16_t(7)));
        row.tags.push_back(Tag{kShowFrame, {}, {}});
        row.tags.push_back(Tag{kEnd, {}, {}});
        Matrix place;
        place.tx = std::llround(kRowX * 20), place.ty = std::llround((kRowsY + i * kRowPitch) * 20);
        rows.push_back(HiddenPlacement(row.id, std::uint16_t(80 + 2 * i), place, names[i]));
        row_defs.push_back(WriteSprite(icon));
        row_defs.push_back(WriteSprite(row));
    }
    // GlyphKeyboard (hidden): the controller's keyboard for the search bar, shown in the column's place while a
    // controller types (the column's parts hidden but its background). A title ("Enter a Glyph Code", the name's
    // gold text) over a line, then a 6 x 6 grid of keys a..z, 0..9 (Key_0 .. Key_35) and under it Delete (Key_36)
    // and Done (Key_37). A key: a 1 px border, its face, its letter (centred) and Hot, its highlight (a 3 px frame,
    // hidden). Sizes and colours from the agreed mock (work/mockups/keyboard-A-right-panel-grid).
    {
        if (!name_text) throw Error("ItemDetailText's name is not 1.09's");
        constexpr double kKey = 70, kGap = 10, kWideH = 56, kHot = 3, kTitleX = 31, kTitleY = 26, kLineY = 77,
                         kLineInset = 20, kGridY = 100;
        constexpr std::uint32_t kBorderRgb = 0x4e4b46, kFaceRgb = 0x242220, kHotRgb = 0xd6bc82, kLineRgb = 0x78746c;
        const Box panel_box = Bounds(m, *back_id);
        const double top = double(panel_box.y0) / 20;
        const double grid_w = 6 * kKey + 5 * kGap;
        const double gx = (kBackLeft + kBackRight - grid_w) / 2;
        const double gy = top + kGridY;
        const auto twips = [](double px) { return std::int64_t(std::llround(px * 20)); };
        // the name's edit text: its height, for the letters' vertical centre
        std::int64_t text_top = 0, text_bottom = 0;
        for (const Tag& t : m.tags) {
            if (t.code != 37 || U16(t.body, 0) != *name_text) continue;
            BitReader r{t.body, 2};
            const int n = int(r.Unsigned(5));
            r.Signed(n), r.Signed(n);
            text_top = r.Signed(n), text_bottom = r.Signed(n);
        }
        const double text_h = double(text_bottom - text_top) / 20;
        // shapes and the Hot frames, by size
        struct KeySize {
            double w, h;
            std::uint16_t border, face, hot;
        };
        const auto key_size = [&](double w, double h) {
            KeySize k{w, h, ++next_id, ++next_id, 0};
            row_defs.push_back(RectShape(k.border, twips(w), twips(h), kBorderRgb));
            row_defs.push_back(RectShape(k.face, twips(w - 2), twips(h - 2), kFaceRgb));
            const std::uint16_t edge_h = ++next_id, edge_v = ++next_id;
            row_defs.push_back(RectShape(edge_h, twips(w), twips(kHot), kHotRgb));
            row_defs.push_back(RectShape(edge_v, twips(kHot), twips(h), kHotRgb));
            Sprite hot{++next_id, 1, {}};
            hot.tags.push_back(Placement(edge_h, 1, At(0, 0), std::nullopt, true));
            hot.tags.push_back(Placement(edge_h, 2, At(0, h - kHot), std::nullopt, true));
            hot.tags.push_back(Placement(edge_v, 3, At(0, 0), std::nullopt, true));
            hot.tags.push_back(Placement(edge_v, 4, At(w - kHot, 0), std::nullopt, true));
            hot.tags.push_back(Tag{kShowFrame, {}, {}});
            hot.tags.push_back(Tag{kEnd, {}, {}});
            row_defs.push_back(WriteSprite(hot));
            k.hot = hot.id;
            return k;
        };
        const double qwerty_w = 10 * kKey + 9 * kGap;
        const KeySize small = key_size(kKey, kKey), wide = key_size((grid_w - kGap) / 2, kWideH),
                      qwerty_wide = key_size((qwerty_w - kGap) / 2, kWideH);
        // an edit text of the name's, `w` wide, with `html`
        const auto text_of = [&](double w, const std::string& html) {
            const std::uint16_t sized = ++next_id;
            std::int64_t left = 0, right = 0;
            for (const Tag& t : m.tags) {
                if (t.code != 37 || U16(t.body, 0) != *name_text) continue;
                BitReader r{t.body, 2};
                const int n = int(r.Unsigned(5));
                left = r.Signed(n), right = r.Signed(n);
            }
            Movie one;
            one.tags.push_back(WiderEditText(m, *name_text, sized, twips(w) - (right - left)));
            Tag t = EditTextWithText(one, sized, sized, html);
            return std::make_pair(t, left);
        };
        // the name's look: bold gold (as the game's own panel text, all <b>)
        const auto html = [](int size, const std::string& align, const std::string& text) {
            return "<p align=\"" + align + "\"><font size=\"" + std::to_string(size) +
                   "\" color=\"#a0895f\" letterSpacing=\"0.000000\" kerning=\"1\"><b>" + text + "</b></font></p>";
        };
        Sprite board{++next_id, 1, {}};
        std::uint16_t depth = 1;
        // the title and its line
        {
            auto [title, left] = text_of(kBackRight - kBackLeft - 2 * kTitleX, html(22, "left", "Enter a Glyph Code"));
            row_defs.push_back(title);
            board.tags.push_back(Placement(U16(title.body, 0), depth++,
                                           At(kBackLeft + kTitleX - double(left) / 20, top + kTitleY - double(text_top) / 20),
                                           std::nullopt, true));
            const std::uint16_t line = ++next_id;
            row_defs.push_back(RectShape(line, twips(kBackRight - kBackLeft - 2 * kLineInset), 20, kLineRgb));
            board.tags.push_back(Placement(line, depth++, At(kBackLeft + kLineInset, top + kLineY), std::nullopt, true));
        }
        // a view (Grid, Qwerty): its keys Key_0 .. Key_35 (its letters' order), Delete (Key_36) and Done (Key_37), and
        // right of the title its toggle's hint, as the game's button prompts show one (the key guide's own markup,
        // "<font color='#A5A58C'>%s：%s</font>" with "<img src='%s' vspace='-8'>": KG_R_U is Triangle, the
        // engine's key prompts redraw it for the device in use).
        Sprite* view = nullptr;
        std::uint16_t view_depth = 1;
        const auto key = [&](int index, const KeySize& k, double x, double y, const std::string& label, int size) {
            auto [text, left] = text_of(k.w, html(size, "center", label));
            row_defs.push_back(text);
            Sprite one{++next_id, 1, {}};
            one.tags.push_back(Placement(k.border, 1, At(0, 0), std::nullopt, true));
            one.tags.push_back(Placement(k.face, 2, At(1, 1), std::nullopt, true));
            one.tags.push_back(Placement(U16(text.body, 0), 3,
                                         At(-double(left) / 20, (k.h - text_h) / 2 - double(text_top) / 20),
                                         std::nullopt, true));
            one.tags.push_back(Placement(k.hot, 4, At(0, 0), std::string("Hot"), false));
            one.tags.push_back(Tag{kShowFrame, {}, {}});
            one.tags.push_back(Tag{kEnd, {}, {}});
            row_defs.push_back(WriteSprite(one));
            view->tags.push_back(Placement(one.id, view_depth++, At(x, y), "Key_" + std::to_string(index), true));
        };
        const auto make_view = [&](const char* name, bool visible, const std::string& hint,
                                   const std::vector<std::pair<std::string, double>>& rows_of, double width,
                                   const KeySize& wide_key) {
            Sprite v{++next_id, 1, {}};
            view = &v;
            view_depth = 1;
            const double hint_w = 300;
            auto [hint_text, left] = text_of(
                hint_w, "<p align=\"right\"><font size=\"22\" color=\"#A5A58C\" letterSpacing=\"0.000000\" kerning=\"1\">"
                        "<img src='KG_R_U' vspace='-8'>\xef\xbc\x9a" + hint + "</font></p>");
            row_defs.push_back(hint_text);
            v.tags.push_back(Placement(U16(hint_text.body, 0), view_depth++,
                                       At(kBackRight - kTitleX - hint_w - double(left) / 20, top + kTitleY - double(text_top) / 20),
                                       std::nullopt, true));
            const double x0 = (kBackLeft + kBackRight - width) / 2;
            int index = 0;
            double y = gy;
            for (const auto& [row, shift] : rows_of) {
                for (std::size_t c = 0; c < row.size(); ++c)
                    key(index++, small, x0 + (double(c) + shift) * (kKey + kGap), y, row.substr(c, 1), 28);
                y += kKey + kGap;
            }
            const double wy = y + 4;
            key(36, wide_key, x0, wy, "Delete", 22);
            key(37, wide_key, x0 + wide_key.w + kGap, wy, "Done", 22);
            v.tags.push_back(Tag{kShowFrame, {}, {}});
            v.tags.push_back(Tag{kEnd, {}, {}});
            row_defs.push_back(WriteSprite(v));
            board.tags.push_back(Placement(v.id, depth++, At(0, 0), std::string(name), visible));
            view = nullptr;
        };
        make_view("Grid", true, "QWERTY",
                  {{"abcdef", 0}, {"ghijkl", 0}, {"mnopqr", 0}, {"stuvwx", 0}, {"yz0123", 0}, {"456789", 0}}, grid_w, wide);
        make_view("Qwerty", false, "A-Z Grid", {{"1234567890", 0}, {"qwertyuiop", 0}, {"asdfghjkl", 0.5}, {"zxcvbnm", 1.5}},
                  qwerty_w, qwerty_wide);
        board.tags.push_back(Tag{kShowFrame, {}, {}});
        board.tags.push_back(Tag{kEnd, {}, {}});
        row_defs.push_back(WriteSprite(board));
        rows.push_back(HiddenPlacement(board.id, 90, Matrix{}, "GlyphKeyboard"));
    }
    {
        const auto frame = std::find_if(column.tags.begin(), column.tags.end(), [](const Tag& t) { return t.code == kShowFrame; });
        column.tags.insert(frame, rows.begin(), rows.end());
    }

    // ItemDetailText: its unnamed parts named, nothing else changed.
    for (Tag& t : game.tags)
        if (IsPlace(t) && !ReadPlace(t).name) t = NamedPart(t, part_name(ReadPlace(t).depth));

    std::vector<Tag> tags;
    std::size_t moves = 0;
    for (std::size_t i = 0; i < m.tags.size(); ++i) {
        const Tag& t = m.tags[i];
        if (i == sprite_at) {
            tags.push_back(WriteSprite(game));
            tags.push_back(WriteSprite(back));
            tags.insert(tags.end(), wider_tags.begin(), wider_tags.end());
            tags.insert(tags.end(), row_defs.begin(), row_defs.end());
            tags.push_back(WriteSprite(column));
            continue;
        }
        tags.push_back(t);
        if (!IsPlace(t)) continue;
        const Place p = ReadPlace(t);
        if (p.depth != kPanelDepth || !p.matrix) continue;
        Matrix at = *p.matrix;
        at.tx += std::llround(kShift * 20);
        if (i == first) {
            tags.push_back(Changed(t.code, Rewrite(t, p, kGlyphDepth, column.id, at, std::string("GlyphDetailText"))));
        } else {
            if (p.character || p.name) throw Error("ItemDetailText's moves are not 1.09's");
            tags.push_back(Changed(t.code, Rewrite(t, p, kGlyphDepth, {}, at, {})));
            ++moves;
        }
    }
    if (moves < 12) throw Error("ItemDetailText's timeline is not 1.09's");
    m.tags = std::move(tags);
    return WriteMovie(m);
}


// The Chalice glyph screen's card list: ItemList (its one sprite) gets, beside the game's NormalItem (left as it is),
// the glyph screen's own list parts, all hidden (the glyph screen shows them and hides NormalItem; every other screen
// keeps NormalItem):
//   GlyphListBack    the list's frame (NormalItem's first part), where NormalItem has it;
//   GlyphNormalItem  NormalItem's rows and scroll bar without the frame and the last row (Item_0_0 .. Item_5_0), one
//                    row lower; the scroll bar shortened to the six rows (its first part, the ornaments along the
//                    top and bottom of the whole list, kept where it was);
//   GlyphSearch      the search bar in the top row's place, over a card's footprint: a dark field with a thin light
//                    border, a caption ("Search by Glyph Code": a card's small grey text, smaller) and the typed text
//                    Query (a card name's gold text) over an underline, the card's focus highlight Cursor and
//                    Active, the bar being typed in (a steady gold frame and underline; both hidden). Sizes and colours from the agreed mock (work/mockups/3-card-height-bar-with-caption).
// The glyph screen's list widget is made on "ItemList/GlyphNormalItem" (gpu/shim/bbport_glyph_screen.cpp).
Bytes GlyphListMovie(const Bytes& gfx) {
    constexpr double kPitch = 93.85, kCard = 90;  // the rows' spacing; a card's height
    Movie m = ReadMovie(gfx);
    const std::size_t list_place = FindPlaced(m, "ItemList");
    const Place list_at = ReadPlace(m.tags[list_place]);
    if (!list_at.character) throw Error("ItemList is not 1.09's");
    const std::size_t list_sprite_at = FindSprite(m, *list_at.character);
    if (list_sprite_at > list_place) throw Error("ItemList is not 1.09's");
    Sprite list = ReadSprite(m.tags[list_sprite_at]);
    std::optional<Place> normal_at;
    for (const Tag& t : list.tags) {
        if (!IsPlace(t)) continue;
        const Place p = ReadPlace(t);
        if (p.name == "GlyphNormalItem") throw Error("the movie already has GlyphNormalItem");
        if (p.name != "NormalItem") throw Error("ItemList is not 1.09's");
        normal_at = p;
    }
    if (!normal_at || !normal_at->character || list.frames != 1 ||
        (normal_at->matrix && (normal_at->matrix->tx || normal_at->matrix->ty || normal_at->matrix->scaled ||
                               normal_at->matrix->rotated)))
        throw Error("ItemList is not 1.09's");
    const Sprite normal = ReadSprite(m.tags[FindSprite(m, *normal_at->character)]);

    std::uint16_t next = HighestCharacter(m);
    std::vector<Tag> defs;
    std::optional<Place> back, bar_at, card;
    double last_row_y = -1;
    int rows = 0;
    Sprite items{++next, normal.frames, {}};
    for (const Tag& t : normal.tags) {
        if (!IsPlace(t)) {
            items.tags.push_back(t);
            continue;
        }
        const Place p = ReadPlace(t);
        if (!p.character || !p.matrix || p.matrix->rotated) throw Error("NormalItem is not 1.09's");
        const int row = p.name ? RowIndex(*p.name) : -1;
        if (p.depth == 1 && !p.name) {
            back = p;
        } else if (row >= 0) {
            ++rows;
            if (row == 0 && !p.matrix->tx && !p.matrix->ty) card = p;
            if (row == 6) {
                last_row_y = double(p.matrix->ty) / 20;
                continue;  // the bar takes a row
            }
            items.tags.push_back(t);
        } else if (p.name == "ScrollBarV") {
            bar_at = p;
        } else {
            throw Error("NormalItem is not 1.09's");
        }
    }
    if (!back || !bar_at || !card || rows != 7 || std::abs(last_row_y - 6 * kPitch) > 0.1 || bar_at->matrix->scaled)
        throw Error("NormalItem is not 1.09's");

    // The scroll bar, scaled from the first row's top to six rows' height; its first part (the list's ornaments) put
    // back where it was, at its own size.
    const double scale = (5 * kPitch + kCard) / (6 * kPitch + kCard);
    Sprite bar = ReadSprite(m.tags[FindSprite(m, *bar_at->character)]);
    bar.id = ++next;
    const double bar_y = double(bar_at->matrix->ty) / 20, new_bar_y = bar_y * scale;  // new_bar_y: in GlyphNormalItem
    bool ornaments = false;
    for (Tag& t : bar.tags) {
        if (!IsPlace(t)) continue;
        const Place p = ReadPlace(t);
        if (p.depth != 1) continue;
        if (!p.matrix || p.matrix->scaled || p.matrix->rotated) throw Error("ScrollBarV is not 1.09's");
        const double list_y = bar_y + double(p.matrix->ty) / 20;  // where it is in ItemList
        Matrix at = WithScale(*p.matrix, 1, 1 / scale);
        at.ty = std::llround((list_y - kPitch - new_bar_y) / scale * 20);
        t = Changed(t.code, Rewrite(t, p, {}, {}, at, {}));
        ornaments = true;
    }
    if (!ornaments) throw Error("ScrollBarV is not 1.09's");
    defs.push_back(WriteSprite(bar));
    {
        Matrix at = WithScale(*bar_at->matrix, 1, scale);
        at.ty = std::llround(new_bar_y * 20);
        const auto end = std::find_if(items.tags.begin(), items.tags.end(), [](const Tag& t) { return t.code == kShowFrame; });
        items.tags.insert(end, Placement(bar.id, bar_at->depth, at, std::string("ScrollBarV"), true));
    }
    defs.push_back(WriteSprite(items));

    // The search bar, from the card's parts: its background (depth 1), Param_0_T's text (the small grey one),
    // ItemName's text (the gold one) and the focus highlight Cursor.
    const Sprite card_sprite = ReadSprite(m.tags[FindSprite(m, *card->character)]);
    std::optional<Place> card_back, cursor, name_box, small_box;
    for (const Tag& t : card_sprite.tags) {
        if (!IsPlace(t)) continue;
        const Place p = ReadPlace(t);
        if (p.depth == 1 && !p.name) card_back = p;
        if (p.name == "Cursor") cursor = p;
        if (p.name == "ItemName") name_box = p;
        if (p.name == "Param_0_T") small_box = p;
    }
    if (!card_back || !card_back->character || !cursor || !cursor->matrix || !name_box || !small_box)
        throw Error("the list's card is not 1.09's");
    const auto only_text = [&](const Place& box) {
        std::optional<std::uint16_t> text;
        for (const Tag& t : ReadSprite(m.tags[FindSprite(m, *box.character)]).tags)
            if (IsPlace(t)) text = ReadPlace(t).character;
        if (!text) throw Error("the list's card is not 1.09's");
        return *text;
    };
    const std::uint16_t caption_text = ++next, query_text = ++next;
    defs.push_back(EditTextWithText(m, only_text(*small_box), caption_text, "Search by Glyph Code"));
    defs.push_back(EditTextWithText(m, only_text(*name_box), query_text,
                                    "<p align=\"left\"><font size=\"22\" color=\"#a0895f\" letterSpacing=\"0.000000\" "
                                    "kerning=\"1\"><b></b></font></p>"));
    // The bar covers the card background's footprint (the shape's bounds where the card places it).
    const Matrix back_at = card_back->matrix.value_or(Matrix{});
    if (back_at.scaled || back_at.rotated) throw Error("the list's card is not 1.09's");
    const Box shape = Bounds(m, *card_back->character);
    const double bx = double(back_at.tx + shape.x0) / 20, by = double(back_at.ty + shape.y0) / 20;
    const double bw = double(shape.x1 - shape.x0) / 20, bh = double(shape.y1 - shape.y0) / 20;
    // px in the bar, from its top left: the border's width, the caption and the typed text from the left, the
    // caption's and the typed text's tops, the underline (inset from both sides)
    constexpr double kBorder = 2, kTextX = 17, kCaptionY = 13, kCaptionScale = 0.66, kQueryY = 41, kLineY = 70,
                     kLineInset = 14;
    constexpr std::uint32_t kBorderRgb = 0x78746c, kFieldRgb = 0x181716, kLineRgb = 0x46433e;
    const std::uint16_t border = ++next, field = ++next, line = ++next;
    const auto twips = [](double px) { return std::int64_t(std::llround(px * 20)); };
    defs.push_back(RectShape(border, twips(bw), twips(bh), kBorderRgb));
    defs.push_back(RectShape(field, twips(bw - 2 * kBorder), twips(bh - 2 * kBorder), kFieldRgb));
    defs.push_back(RectShape(line, twips(bw - 2 * kLineInset), 20, kLineRgb));
    Sprite search{++next, 1, {}};
    search.tags.push_back(Placement(border, 1, At(bx, by), std::nullopt, true));
    search.tags.push_back(Placement(field, 2, At(bx + kBorder, by + kBorder), std::nullopt, true));
    search.tags.push_back(Placement(line, 3, At(bx + kLineInset, by + kLineY), std::nullopt, true));
    search.tags.push_back(Placement(caption_text, 4,
                                    WithScale(At(bx + kTextX, by + kCaptionY), kCaptionScale, kCaptionScale),
                                    std::string("Caption"), true));
    search.tags.push_back(Placement(query_text, 5, At(bx + kTextX, by + kQueryY), std::string("Query"), true));
    // Active (hidden): the bar while it is typed in, a steady frame and underline in the typed text's gold over the
    // border and the underline (the pulsing Cursor is the bar's focus only).
    constexpr std::uint32_t kActiveRgb = 0xa0895f;
    constexpr double kActiveLine = 2;
    const std::uint16_t edge_h = ++next, edge_v = ++next, active_line = ++next;
    defs.push_back(RectShape(edge_h, twips(bw), twips(kBorder), kActiveRgb));
    defs.push_back(RectShape(edge_v, twips(kBorder), twips(bh), kActiveRgb));
    defs.push_back(RectShape(active_line, twips(bw - 2 * kLineInset), twips(kActiveLine), kActiveRgb));
    Sprite active{++next, 1, {}};
    active.tags.push_back(Placement(edge_h, 1, At(bx, by), std::nullopt, true));
    active.tags.push_back(Placement(edge_h, 2, At(bx, by + bh - kBorder), std::nullopt, true));
    active.tags.push_back(Placement(edge_v, 3, At(bx, by), std::nullopt, true));
    active.tags.push_back(Placement(edge_v, 4, At(bx + bw - kBorder, by), std::nullopt, true));
    active.tags.push_back(Placement(active_line, 5, At(bx + kLineInset, by + kLineY), std::nullopt, true));
    active.tags.push_back(Tag{kShowFrame, {}, {}});
    active.tags.push_back(Tag{kEnd, {}, {}});
    defs.push_back(WriteSprite(active));
    search.tags.push_back(Placement(active.id, 6, At(0, 0), std::string("Active"), false));
    search.tags.push_back(Placement(*cursor->character, 7, *cursor->matrix, std::string("Cursor"), false));
    search.tags.push_back(Tag{kShowFrame, {}, {}});
    search.tags.push_back(Tag{kEnd, {}, {}});
    defs.push_back(WriteSprite(search));

    // GlyphNoMatch (hidden): the line a search with no matching card shows in the list's place ("No Chalice Glyphs
    // match ...", set by the screen): the card name's text, as wide as the bar less its margins, centred, in the
    // game's grey (the stats' #a0a08c). From the agreed mock (work/mockups/filter-E2-no-match).
    constexpr double kNoMatchY = 40;  // px under the first card's top
    const std::uint16_t no_match = ++next;
    std::int64_t name_left = 0, name_right = 0, name_top = 0;
    for (const Tag& t : m.tags) {
        if (t.code != 37 || U16(t.body, 0) != only_text(*name_box)) continue;
        BitReader r{t.body, 2};
        const int n = int(r.Unsigned(5));
        name_left = r.Signed(n), name_right = r.Signed(n), name_top = r.Signed(n);
    }
    {
        Movie one;
        one.tags.push_back(
            WiderEditText(m, only_text(*name_box), no_match, twips(bw - 2 * kTextX) - (name_right - name_left)));
        defs.push_back(EditTextWithText(one, no_match, no_match,
                                        "<p align=\"center\"><font size=\"22\" color=\"#a0a08c\" "
                                        "letterSpacing=\"0.000000\" kerning=\"1\"><b></b></font></p>"));
    }

    // ItemList: the glyph screen's parts after NormalItem, hidden.
    const auto frame = std::find_if(list.tags.begin(), list.tags.end(), [](const Tag& t) { return t.code == kShowFrame; });
    const std::vector<Tag> parts = {
        Placement(*back->character, 3, *back->matrix, std::string("GlyphListBack"), false),
        Placement(items.id, 5, At(0, kPitch), std::string("GlyphNormalItem"), false),
        Placement(search.id, 7, At(0, 0), std::string("GlyphSearch"), false),
        Placement(no_match, 9,
                  At(bx + kTextX - double(name_left) / 20, by + kPitch + kNoMatchY - double(name_top) / 20),
                  std::string("GlyphNoMatch"), false),
    };
    list.tags.insert(frame, parts.begin(), parts.end());

    std::vector<Tag> tags;
    for (std::size_t i = 0; i < m.tags.size(); ++i) {
        if (i == list_sprite_at) {
            tags.insert(tags.end(), defs.begin(), defs.end());
            tags.push_back(WriteSprite(list));
            continue;
        }
        tags.push_back(m.tags[i]);
    }
    m.tags = std::move(tags);
    return WriteMovie(m);
}

// The Chalice glyph screen's tab icons. An item screen's tab tile (inventory.gfx sprite 228, ItemCategory's
// Item_0_N) shows one frame per tab kind: the frame number is the tab descriptor's +8 (gotoAndStop), each frame a
// shape filled from the menu sheet MENU_Common_00100 (80 px cells). The glyph screen's Testing and Your Dungeons tabs
// get two frames of their own after the game's 60: grey copies of the in-game menu's sepia System (gear) and Status
// (figure) icons, made here from the player's own common.tpf.dcx, toned to the grey tab icons beside them (Farming,
// Equipment and Blood Gems: frames 2, 4 and 8). The game's frames stay as they are.
namespace {
constexpr int kCell = 80;
struct Picture {  // kCell x kCell RGBA
    std::vector<std::uint8_t> px = std::vector<std::uint8_t>(kCell * kCell * 4);
    std::uint8_t* at(int x, int y) { return &px[(std::size_t(y) * kCell + x) * 4]; }
    const std::uint8_t* at(int x, int y) const { return &px[(std::size_t(y) * kCell + x) * 4]; }
};

// One 80 px cell of a PS4-tiled BC1 texture (TPF format 0): 4x4 blocks, 8x8-block tiles in Morton order, tiles
// row-major.
Picture SheetCell(const Bytes& tpf, const std::string& name, int x0, int y0) {
    if (!Has(tpf, 0, "TPF\0", 4)) throw Error("common.tpf.dcx holds no TPF");
    const std::uint32_t count = U32(tpf, 8);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t h = 0x10 + std::size_t(i) * 0x24;
        if (h + 0x24 > tpf.size()) break;
        const std::size_t name_at = U32(tpf, h + 0x18);
        std::string n;
        for (std::size_t p = name_at; p + 1 < tpf.size() && (tpf[p] || tpf[p + 1]); p += 2) n.push_back(char(tpf[p]));
        if (n != name) continue;
        const std::size_t off = U32(tpf, h), size = U32(tpf, h + 4);
        const int w = U16(tpf, h + 0xc), hh = U16(tpf, h + 0xe);
        if (tpf[h + 8] != 0 || w % 32 || x0 < 0 || y0 < 0 || x0 + kCell > w || y0 + kCell > hh ||
            off > tpf.size() || size > tpf.size() - off)
            throw Error(name + " is not 1.09's BC1 sheet");
        const int bw = w / 4;
        Picture out;
        for (int by = 0; by < kCell / 4; ++by)
            for (int bx = 0; bx < kCell / 4; ++bx) {
                const int x = x0 / 4 + bx, y = y0 / 4 + by;
                int k = 0;
                for (int b = 0; b < 3; ++b) k |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1);
                const std::size_t block = off + (std::size_t((y / 8) * ((bw + 7) / 8) + x / 8) * 64 + k) * 8;
                if (block + 8 > off + size) throw Error(name + " is shorter than its size");
                const std::uint16_t c0 = U16(tpf, block), c1 = U16(tpf, block + 2);
                const std::uint32_t bits = U32(tpf, block + 4);
                const auto rgb = [](std::uint16_t c) {
                    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
                    return std::array<int, 4>{(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255};
                };
                const auto a = rgb(c0), b = rgb(c1);
                std::array<std::array<int, 4>, 4> pal{a, b};
                for (int c = 0; c < 3; ++c) {
                    pal[2][c] = c0 > c1 ? (2 * a[c] + b[c] + 1) / 3 : (a[c] + b[c]) / 2;
                    pal[3][c] = c0 > c1 ? (a[c] + 2 * b[c] + 1) / 3 : 0;
                }
                pal[2][3] = 255, pal[3][3] = c0 > c1 ? 255 : 0;
                for (int i = 0; i < 16; ++i) {
                    const auto& p = pal[(bits >> (2 * i)) & 3];
                    std::uint8_t* d = out.at(bx * 4 + i % 4, by * 4 + i / 4);
                    for (int c = 0; c < 4; ++c) d[c] = std::uint8_t(p[c]);
                }
            }
        return out;
    }
    throw Error("common.tpf.dcx has no " + name);
}

// Pillow's RGB -> L (ITU-R 601-2 luma, fixed point), as the approved mockups were made with.
int Luma(const std::uint8_t* p) { return (p[0] * 19595 + p[1] * 38470 + p[2] * 7471 + 0x8000) >> 16; }
using Plane = std::vector<int>;  // kCell x kCell values
Plane LumaPlane(const Picture& s) {
    Plane l(kCell * kCell);
    for (int i = 0; i < kCell * kCell; ++i) l[i] = Luma(&s.px[std::size_t(i) * 4]);
    return l;
}
// Mean and population standard deviation (Pillow's ImageStat).
std::pair<double, double> MeanSpread(const Plane& l) {
    double sum = 0, sum2 = 0;
    for (const int v : l) sum += v, sum2 += double(v) * v;
    const double n = double(l.size()), mean = sum / n;
    return {mean, std::sqrt(std::max(0.0, (sum2 - sum * sum / n) / n))};
}
// Python's round(): halves to even.
int RoundHalfEven(double v) { return int(std::nearbyint(v)); }
int Clamp255(int v) { return std::clamp(v, 0, 255); }

// The grey tab icons' tone: luminance mean / spread, and per brightness band (<60, <120, the rest) the average colour
// offset from the pixel's luma (R - L, G - L, B - L): their slight blue-grey.
struct Tone {
    double mean = 0, spread = 0;
    double offs[3][3] = {};
};
Tone ToneOf(const std::vector<Picture>& refs) {
    Tone t;
    for (const Picture& r : refs) {
        const auto [m, s] = MeanSpread(LumaPlane(r));
        t.mean += m / double(refs.size()), t.spread += s / double(refs.size());
        double sums[3][3] = {};
        int counts[3] = {};
        for (int i = 0; i < kCell * kCell; ++i) {
            const std::uint8_t* p = &r.px[std::size_t(i) * 4];
            const int band_luma = (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000;
            const int band = band_luma < 60 ? 0 : band_luma < 120 ? 1 : 2;
            const double luma = (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000.0;
            for (int c = 0; c < 3; ++c) sums[band][c] += p[c] - luma;
            ++counts[band];
        }
        for (int b = 0; b < 3; ++b)
            for (int c = 0; c < 3; ++c) t.offs[b][c] += sums[b][c] / std::max(counts[b], 1) / double(refs.size());
    }
    return t;
}
// The colour offset at luminance v, blended between the band centres (30, 90, 188).
std::array<double, 3> OffsetAt(double v, const Tone& t) {
    constexpr double centres[3] = {30, 90, 188};
    if (v <= centres[0]) return {t.offs[0][0], t.offs[0][1], t.offs[0][2]};
    if (v >= centres[2]) return {t.offs[2][0], t.offs[2][1], t.offs[2][2]};
    const int i = v < centres[1] ? 0 : 1;
    const double f = (v - centres[i]) / (centres[i + 1] - centres[i]);
    std::array<double, 3> o{};
    for (int c = 0; c < 3; ++c) o[c] = t.offs[i][c] * (1 - f) + t.offs[i + 1][c] * f;
    return o;
}
// Each pixel's colour from its luma through `tone_of` (luma -> toned luminance) and the tint at that luminance.
template <class F>
Picture Tinted(const Picture& src, const Plane& luma, const Tone& t, F tone_of) {
    std::array<std::array<std::uint8_t, 3>, 256> lut{};
    for (int v = 0; v < 256; ++v) {
        const double l = tone_of(v);
        const auto o = OffsetAt(l, t);
        for (int c = 0; c < 3; ++c) lut[v][c] = std::uint8_t(Clamp255(RoundHalfEven(l + o[c])));
    }
    Picture out = src;
    for (int i = 0; i < kCell * kCell; ++i)
        for (int c = 0; c < 3; ++c) out.px[std::size_t(i) * 4 + c] = lut[luma[i]][c];
    return out;
}
// Sepia -> the grey tabs' tone: luminance matched to their mean / spread, tinted by brightness.
Picture Greyed(const Picture& src, const Tone& t) {
    const Plane luma = LumaPlane(src);
    const auto [mean, spread] = MeanSpread(luma);
    const double k = t.spread / std::max(spread, 1.0);
    return Tinted(src, luma, t, [&](int v) { return std::clamp((v - mean) * k + t.mean, 0.0, 255.0); });
}
constexpr int kRing = 12;  // a tile's ornamented background: the outer band of the cell
double RingMean(const Picture& p) {
    double sum = 0;
    int n = 0;
    for (int y = 0; y < kCell; ++y)
        for (int x = 0; x < kCell; ++x)
            if (x < kRing || y < kRing || x >= kCell - kRing || y >= kCell - kRing) sum += Luma(p.at(x, y)), ++n;
    return sum / n;
}
// Greyed, the background ring at the grey tabs' brightness, and the tones above `knee` compressed by `ratio`.
Picture GreyedKnee(const Picture& src, const std::vector<Picture>& refs, const Tone& t, double knee, double ratio) {
    const Picture base = Greyed(src, t);
    double target = 0;
    for (const Picture& r : refs) target += RingMean(r) / double(refs.size());
    const double lift = target - RingMean(base);
    return Tinted(base, LumaPlane(base), t, [&](int v) {
        double l = v + lift;
        if (l > knee) l = knee + (l - knee) * ratio;
        return std::clamp(l, 0.0, 255.0);
    });
}

// Pillow's rank filters (MaxFilter / MinFilter): the square window over the image with its edge pixels repeated.
Plane Rank(const Plane& in, int size, bool most) {
    const int r = size / 2;
    Plane out(in.size());
    for (int y = 0; y < kCell; ++y)
        for (int x = 0; x < kCell; ++x) {
            int v = most ? 0 : 255;
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    const int s = in[std::clamp(y + dy, 0, kCell - 1) * kCell + std::clamp(x + dx, 0, kCell - 1)];
                    v = most ? std::max(v, s) : std::min(v, s);
                }
            out[y * kCell + x] = v;
        }
    return out;
}
// Pillow's GaussianBlur: three extended box blurs (BoxBlur.c), all the rows' passes, then all the columns'.
void BoxBlurLine(const int* in, int* out, int stride, int r, double radius) {
    const int last = kCell - 1;
    const std::uint32_t ww = std::uint32_t(double(1u << 24) / (radius * 2 + 1));
    const std::uint32_t fw = ((1u << 24) - std::uint32_t(r * 2 + 1) * ww) / 2;
    const int edge_a = std::min(r + 1, kCell), edge_b = std::max(kCell - r - 1, 0);
    const auto at = [&](int i) { return std::uint32_t(in[i * stride]); };
    std::uint32_t acc = at(0) * std::uint32_t(r + 1);
    for (int x = 0; x < edge_a - 1; ++x) acc += at(x);
    acc += at(last) * std::uint32_t(r - edge_a + 1);
    const auto save = [&](int x, std::uint32_t bulk) { out[x * stride] = int((bulk + (1u << 23)) >> 24); };
    if (edge_a <= edge_b) {
        for (int x = 0; x < edge_a; ++x) {
            acc += at(x + r) - at(0);
            save(x, acc * ww + (at(0) + at(x + r + 1)) * fw);
        }
        for (int x = edge_a; x < edge_b; ++x) {
            acc += at(x + r) - at(x - r - 1);
            save(x, acc * ww + (at(x - r - 1) + at(x + r + 1)) * fw);
        }
        for (int x = edge_b; x <= last; ++x) {
            acc += at(last) - at(x - r - 1);
            save(x, acc * ww + (at(x - r - 1) + at(last)) * fw);
        }
    } else {
        for (int x = 0; x < edge_b; ++x) {
            acc += at(x + r) - at(0);
            save(x, acc * ww + (at(0) + at(x + r + 1)) * fw);
        }
        for (int x = edge_b; x < edge_a; ++x) {
            acc += at(last) - at(0);
            save(x, acc * ww + (at(0) + at(last)) * fw);
        }
        for (int x = edge_a; x <= last; ++x) {
            acc += at(last) - at(x - r - 1);
            save(x, acc * ww + (at(x - r - 1) + at(last)) * fw);
        }
    }
}
Plane GaussianBlur(Plane p, double radius) {
    constexpr int passes = 3;
    const double sigma2 = radius * radius / passes;
    const double big = std::sqrt(12.0 * sigma2 + 1.0);
    const double small = std::floor((big - 1.0) / 2.0);
    double a = (2 * small + 1) * (small * (small + 1) - 3 * sigma2);
    a /= 6 * (sigma2 - (small + 1) * (small + 1));
    const double box = small + a;
    const int whole = int(box);
    Plane next(p.size());
    for (int pass = 0; pass < passes; ++pass) {
        for (int y = 0; y < kCell; ++y) BoxBlurLine(&p[y * kCell], &next[y * kCell], 1, whole, box);
        p.swap(next);
    }
    for (int pass = 0; pass < passes; ++pass) {
        for (int x = 0; x < kCell; ++x) BoxBlurLine(&p[x], &next[x], kCell, whole, box);
        p.swap(next);
    }
    return p;
}
// The Status icon's pale figure: luma above 150 inside its area, holes closed, the edge softened.
Plane FigureMask(const Picture& status) {
    constexpr int kThreshold = 150, kBox[4] = {16, 4, 66, 80};
    const Plane luma = LumaPlane(status);
    Plane m(luma.size(), 0);
    for (int y = kBox[1]; y < kBox[3]; ++y)
        for (int x = kBox[0]; x < kBox[2]; ++x)
            if (luma[y * kCell + x] > kThreshold) m[y * kCell + x] = 255;
    return GaussianBlur(Rank(Rank(m, 5, true), 3, false), 1.2);
}
// Pillow's composite (paste through an L mask): over * m + under * (255 - m), divided by 255 with rounding.
Picture Composite(const Picture& over, const Picture& under, const Plane& mask) {
    Picture out = under;
    for (int i = 0; i < kCell * kCell; ++i)
        for (int c = 0; c < 4; ++c) {
            const std::size_t at = std::size_t(i) * 4 + c;
            const std::uint32_t t =
                std::uint32_t(under.px[at]) * std::uint32_t(255 - mask[i]) + std::uint32_t(over.px[at]) * std::uint32_t(mask[i]) + 128;
            out.px[at] = std::uint8_t(((t >> 8) + t) >> 8);
        }
    return out;
}

// The two pictures, from the sheet: [0] System (gear) for Testing, [1] Status (figure) for Your Dungeons.
std::array<Picture, 2> GlyphTabPictures(const Bytes& tpf) {
    const std::string sheet = "MENU_Common_00100";
    const std::vector<Picture> refs = {SheetCell(tpf, sheet, 80, 0), SheetCell(tpf, sheet, 240, 0),
                                       SheetCell(tpf, sheet, 480, 0)};
    const Tone tone = ToneOf(refs);
    const Picture system = Greyed(SheetCell(tpf, sheet, 0, 240), tone);
    const Picture status_src = SheetCell(tpf, sheet, 800, 160);
    // The figure's whites quieted, the tile background as light as the other tiles; then the figure alone lifted.
    const Picture status_quiet = GreyedKnee(status_src, refs, tone, 150, 0.45);
    Picture lighter = status_quiet;
    for (int i = 0; i < kCell * kCell; ++i)
        for (int c = 0; c < 3; ++c) {
            std::uint8_t& v = lighter.px[std::size_t(i) * 4 + c];
            v = std::uint8_t(RoundHalfEven(255 * std::pow(v / 255.0, 0.72)));
        }
    return {system, Composite(lighter, status_quiet, FigureMask(status_src))};
}

// DefineBitsLossless2 (tag 36): a 32-bit ARGB bitmap, premultiplied, zlib-packed.
Tag BitmapTag(std::uint16_t id, const Picture& p) {
    Bytes argb;
    argb.reserve(p.px.size());
    for (int i = 0; i < kCell * kCell; ++i) {
        const std::uint8_t* s = &p.px[std::size_t(i) * 4];
        const int a = s[3];
        argb.push_back(std::uint8_t(a));
        for (int c = 0; c < 3; ++c) argb.push_back(std::uint8_t((s[c] * a + 127) / 255));
    }
    uLongf packed = compressBound(uLong(argb.size()));
    Bytes z(packed);
    if (compress2(z.data(), &packed, argb.data(), uLong(argb.size()), 9) != Z_OK) throw Error("zlib");
    Bytes body;
    PutLe(body, id, 2);
    body.push_back(5);  // 32-bit ARGB
    PutLe(body, kCell, 2);
    PutLe(body, kCell, 2);
    body.insert(body.end(), z.begin(), z.begin() + std::ptrdiff_t(packed));
    return Changed(36, std::move(body));
}
// A kCell-pixel square shape filled with bitmap `bitmap` at 1:1, with the fill type the game's tab pictures use.
Tag BitmapSquare(std::uint16_t id, std::uint16_t bitmap, std::uint8_t fill_type) {
    const std::int64_t side = kCell * 20;
    Bytes body;
    PutLe(body, id, 2);
    BitWriter bounds;
    const int n = BitsFor(side);
    bounds.Put(std::uint64_t(n), 5);
    for (const std::int64_t v : {std::int64_t(0), side, std::int64_t(0), side})
        bounds.Put(std::uint64_t(v) & ((std::uint64_t(1) << n) - 1), n);
    Append(body, bounds.out);
    body.push_back(1);  // one fill style
    body.push_back(fill_type);
    PutLe(body, bitmap, 2);
    Matrix fill;  // 20 twips a bitmap pixel
    fill.scaled = true, fill.sx = fill.sy = 20u << 16, fill.scale_bits = BitsFor(std::int64_t(20) << 16);
    Append(body, WriteMatrix(fill));
    body.push_back(0);  // no line styles
    BitWriter shape;
    shape.Put(1, 4);  // fill index bits
    shape.Put(0, 4);  // line index bits
    shape.Put(0b000011, 6);  // a style change: fill style 0 and a move
    shape.Put(1, 5), shape.Put(0, 1), shape.Put(0, 1);  // move to (0, 0)
    shape.Put(1, 1);  // fill style 0 = 1
    const int bits = std::max(BitsFor(side), BitsFor(-side));
    const auto edge = [&](bool vertical, std::int64_t delta) {
        shape.Put(0b11, 2);
        shape.Put(std::uint64_t(bits - 2), 4);
        shape.Put(0, 1);
        shape.Put(vertical ? 1 : 0, 1);
        shape.Put(std::uint64_t(delta) & ((std::uint64_t(1) << bits) - 1), bits);
    };
    edge(false, side), edge(true, side), edge(false, -side), edge(true, -side);
    shape.Put(0, 6);  // the end
    Append(body, shape.out);
    return Changed(2, std::move(body));
}
// PlaceObject2 that swaps the character on `depth` (move + character), as the tab tile's own frames do.
Tag SwapOnDepth(std::uint16_t depth, std::uint16_t id) {
    Bytes b{0x01 | 0x02};
    PutLe(b, depth, 2);
    PutLe(b, id, 2);
    return Changed(kPlace2, std::move(b));
}
}  // namespace

Bytes GlyphTabsMovie(const Bytes& gfx, const Bytes& common_tpf_dcx) {
    constexpr std::uint16_t kTabTile = 228, kGameFrames = 60, kIconDepth = 1;
    Movie m = ReadMovie(gfx);
    const std::size_t tile_at = FindSprite(m, kTabTile);
    Sprite tile = ReadSprite(m.tags[tile_at]);
    if (tile.frames != kGameFrames || tile.tags.empty() || tile.tags.back().code != kEnd)
        throw Error("inventory.gfx's tab tile is not 1.09's");
    // The fill type of the game's own tab pictures: the shape on the picture depth in the last frame.
    std::optional<std::uint16_t> last_picture;
    for (const Tag& t : tile.tags)
        if (IsPlace(t)) {
            const Place p = ReadPlace(t);
            if (p.depth == kIconDepth && p.character) last_picture = *p.character;
        }
    std::optional<std::uint8_t> fill_type;
    for (const Tag& t : m.tags)
        if (t.code == 2 && last_picture && t.body.size() > 12 && U16(t.body, 0) == *last_picture) {
            BitReader r{t.body, 2};
            const int n = int(r.Unsigned(5));
            const std::size_t after = 2 + (5 + 4 * std::size_t(n) + 7) / 8;
            if (after + 1 < t.body.size() && t.body[after] == 1) fill_type = t.body[after + 1];
        }
    if (!fill_type || *fill_type < 0x40 || *fill_type > 0x43) throw Error("inventory.gfx's tab pictures are not 1.09's");
    const auto pictures = GlyphTabPictures(DcxRead(common_tpf_dcx).data);
    std::uint16_t next = HighestCharacter(m);
    std::vector<Tag> defs;
    std::vector<std::uint16_t> shapes;
    for (const Picture& p : pictures) {
        const std::uint16_t bitmap = ++next, shape = ++next;
        defs.push_back(BitmapTag(bitmap, p));
        defs.push_back(BitmapSquare(shape, bitmap, *fill_type));
        shapes.push_back(shape);
    }
    tile.tags.pop_back();  // the End tag
    for (const std::uint16_t s : shapes) {
        tile.tags.push_back(SwapOnDepth(kIconDepth, s));
        tile.tags.push_back(Changed(kShowFrame, {}));
    }
    tile.tags.push_back(Changed(kEnd, {}));
    tile.frames = std::uint16_t(kGameFrames + shapes.size());
    std::vector<Tag> tags;
    for (std::size_t i = 0; i < m.tags.size(); ++i) {
        if (i == tile_at) {
            tags.insert(tags.end(), defs.begin(), defs.end());
            tags.push_back(WriteSprite(tile));
            continue;
        }
        tags.push_back(m.tags[i]);
    }
    m.tags = std::move(tags);
    return WriteMovie(m);
}

Bytes AltarScripts(const Bytes& file) {
    Dcx dcx = DcxRead(file);
    Binder scripts = BinderRead(dcx.data);
    bool found = false;
    for (BinderFile& f : scripts.files) {
        std::string name;
        for (std::size_t i = 0; i < f.name.size(); i += scripts.wide_names ? 2 : 1)
            name.push_back(scripts.wide_names && i + 1 < f.name.size() && f.name[i + 1] ? '?' : char(f.name[i]));
        if (name.size() < 11 || name.compare(name.size() - 11, 11, "t242307.esd")) continue;
        std::string why;
        if (!rebirth_script(f.data, &why)) throw Error(why);
        found = true;
    }
    if (!found) throw Error("the altar's scripts have no t242307.esd");
    dcx.data = BinderWrite(scripts);
    return DcxWrite(dcx);
}
}
