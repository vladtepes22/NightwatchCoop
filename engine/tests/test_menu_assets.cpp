// SPDX-License-Identifier: GPL-3.0-or-later
#include "../tools/engine/menu_assets.h"
#include <zlib.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace menu_assets;

Bytes operator+(Bytes a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

namespace {
void Le(Bytes& b, std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i) b.push_back(std::uint8_t(v >> (8 * i)));
}
Bytes Ascii(const std::string& s) { return Bytes(s.begin(), s.end()); }
Bytes Wide(const std::u16string& s) {
    Bytes b;
    for (char16_t c : s) Le(b, c, 2);
    return b;
}
std::size_t Count(const Bytes& b, const std::string& what) {
    std::size_t n = 0;
    for (auto it = b.begin(); (it = std::search(it, b.end(), what.begin(), what.end())) != b.end(); ++it) ++n;
    return n;
}

Binder Bundle(const Fmg& fmg) {
    Binder b;
    b.header.assign(0x40, 0);
    std::memcpy(b.header.data(), "BND4", 4);
    b.header[0x20] = 0x24;
    b.header[0x30] = 1;
    b.entry_bytes = 0x24;
    b.wide_names = true;
    for (std::uint32_t id : {200u, 201u, 78u, 204u, 30u, 999u}) {
        BinderFile f;
        f.flags = 0xffffffff00000040ull;
        f.id = id;
        f.name = Wide(u"N:\\msg\\测试" + std::u16string(1, char16_t(u'0' + id % 10)) + u".fmg");
        f.data = FmgWrite(fmg);
        b.files.push_back(f);
    }
    return b;
}

Bytes Packed(const Bytes& data) {
    Bytes h(0x4c, 0);
    std::memcpy(h.data(), "DCX\0", 4);
    std::memcpy(h.data() + 0x18, "DCS\0", 4);
    std::memcpy(h.data() + 0x24, "DCP\0DFLT", 8);
    std::memcpy(h.data() + 0x44, "DCA\0", 4);
    h[0x4b] = 8;
    return DcxWrite(menu_assets::Dcx{h, data});
}

Bytes Tag(int code, const Bytes& body) {
    Bytes t;
    if (body.size() < 0x3f) Le(t, code << 6 | body.size(), 2);
    else Le(t, code << 6 | 0x3f, 2), Le(t, body.size(), 4);
    t.insert(t.end(), body.begin(), body.end());
    return t;
}
Bytes Translate(int tx, int ty) {
    Bytes b(5, 0);
    std::uint64_t bits = 0;
    int at = 0;
    auto put = [&](std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i, ++at) bits |= (v >> i & 1) << (39 - at);
    };
    put(0, 2), put(16, 5), put(std::uint16_t(tx), 16), put(std::uint16_t(ty), 16);
    for (int i = 0; i < 5; ++i) b[i] = std::uint8_t(bits >> (32 - 8 * i));
    return b;
}
Bytes Place(int depth, int character, int tx, int ty, const std::string& name) {
    Bytes b{std::uint8_t((character ? 0x02 : 0x01) | 0x04 | (name.empty() ? 0 : 0x20))};
    Le(b, depth, 2);
    if (character) Le(b, character, 2);
    const Bytes m = Translate(tx, ty);
    b.insert(b.end(), m.begin(), m.end());
    if (!name.empty()) b.insert(b.end(), name.begin(), name.end()), b.push_back(0);
    return Tag(26, b);
}
Bytes Sprite(int id, int frames, const Bytes& tags) {
    Bytes b;
    Le(b, id, 2);
    Le(b, frames, 2);
    b.insert(b.end(), tags.begin(), tags.end());
    b.insert(b.end(), {0, 0});
    return Tag(39, b);
}
/// A shape whose bounds are 0..width x 0..height twips (only its bounds are read).
Bytes Shape(int id, int height, int width = 2000) {
    Bytes b;
    Le(b, id, 2);
    int at = 0;
    Bytes rect(10, 0);
    auto put = [&](std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i, ++at) rect[at / 8] |= std::uint8_t((v >> i & 1) << (7 - at % 8));
    };
    put(16, 5), put(0, 16), put(std::uint16_t(width), 16), put(0, 16), put(std::uint16_t(height), 16);
    return Tag(2, b + rect + Bytes{0, 0});
}
/// A placement flipped both ways (scale -1, -1), as 1.09's right-hand stone in the Environment window.
Bytes FlippedPlace(int depth, int character, int tx, int ty) {
    Bytes b{0x02 | 0x04};
    Le(b, depth, 2);
    Le(b, character, 2);
    Bytes m(9, 0);
    int at = 0;
    auto put = [&](std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i, ++at) m[at / 8] |= std::uint8_t((v >> i & 1) << (7 - at % 8));
    };
    put(1, 1), put(18, 5), put(std::uint64_t(-65536) & 0x3ffff, 18), put(std::uint64_t(-65536) & 0x3ffff, 18);
    put(0, 1), put(16, 5), put(std::uint16_t(tx), 16), put(std::uint16_t(ty), 16);
    m.resize(std::size_t(at + 7) / 8);
    return Tag(26, b + m);
}
/// Nightwatch: an Environment screen laid out as 1.09's (a window: a tall background, parts above and below
/// Display HUD, a flipped picture whose origin is below it but whose top is above, a thin line across it; five
/// rows and Defaults, two headers, the help line; a second frame moves the rows).
Bytes Environment(bool status_bar) {
    Bytes window = Place(1, 8, 0, -1000, "") + Place(2, 1, 0, -200, "") + Place(3, 1, 0, 1200, "") +
                   Place(4, 1, 0, 3000, "") + FlippedPlace(5, 10, 3000, 2500) + Place(6, 11, 0, 1000, "") + Tag(1, {});
    Bytes env = Place(3, 7, 0, 1000, "");
    for (int r = 0; r < 5; ++r) env = env + Place(64 + 9 * r, 1, 1840, 1500 + 1050 * r, "Item_" + std::to_string(r) + "_0");
    env = env + Place(109, 1, 10800, 9500, "Item_5_0") + Place(116, 1, 600, 1300, "StaticText_111010") +
          Place(117, 1, 600, 3600, "StaticText_111011");
    if (status_bar) env = env + Place(61, 1, 9000, 6600, "StatusBar");
    env = env + Tag(1, {});
    for (int r = 0; r < 5; ++r) env = env + Place(64 + 9 * r, 0, 1840, 1460 + 1050 * r, "");
    env = env + Tag(1, {});
    return Shape(8, 9000) + Shape(10, 2000) + Shape(11, 1500, 100) + Sprite(7, 1, window) + Sprite(9, 2, env);
}

struct At {
    int y = 0;
    double sy = 1;
};
/// The y (twips) and vertical scale of each placement in sprite `id` of a movie, by depth: its first frame.
std::map<int, At> PlacedY(const Bytes& movie, int id) {
    auto tags = [](const Bytes& b, std::size_t at, std::size_t end) {
        std::vector<std::pair<int, Bytes>> out;
        while (at + 2 <= end) {
            const int head = b[at] | b[at + 1] << 8;
            std::size_t n = head & 0x3f;
            at += 2;
            if (n == 0x3f) n = std::size_t(b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24), at += 4;
            out.emplace_back(head >> 6, Bytes(b.begin() + std::ptrdiff_t(at), b.begin() + std::ptrdiff_t(at + n)));
            at += n;
            if ((head >> 6) == 0) break;
        }
        return out;
    };
    std::map<int, At> ys;
    const std::size_t start = 8 + (5 + 4 * (movie[8] >> 3) + 7) / 8 + 4;
    for (const auto& [code, body] : tags(movie, start, movie.size())) {
        if (code != 39 || (body[0] | body[1] << 8) != id) continue;
        for (const auto& [c, p] : tags(body, 4, body.size())) {
            if (c == 1) return ys;
            if (c != 26 || !(p[0] & 0x04)) continue;
            int bit = 8 * (p[0] & 0x02 ? 5 : 3);
            auto get = [&](int n, bool sign) {
                std::int64_t v = 0;
                for (int i = 0; i < n; ++i, ++bit) v = v << 1 | (p[std::size_t(bit / 8)] >> (7 - bit % 8) & 1);
                return sign && n && (v >> (n - 1) & 1) ? v - (std::int64_t(1) << n) : v;
            };
            At a;
            if (get(1, false)) {
                const int n = int(get(5, false));
                get(n, true);
                a.sy = double(get(n, true)) / 65536.0;
            }
            if (get(1, false)) { const int n = int(get(5, false)); get(n, true), get(n, true); }
            const int n = int(get(5, false));
            get(n, true);
            a.y = int(get(n, true));
            ys[p[1] | p[2] << 8] = a;
        }
    }
    return ys;
}
Bytes Movie(bool environment = true, bool status_bar = true) {
    const Bytes tags = Tag(2, {1, 0, 0}) + (environment ? Environment(status_bar) : Bytes{});
    Bytes title = Place(1, 1, 40, 40, "StaticText_113020") + Tag(1, {});
    Bytes donor;
    for (int r = 0; r < 6; ++r) donor = donor + Place(56 + 9 * r, 1, 1840, 1060 + 1143 * r, "Item_" + std::to_string(r) + "_0");
    donor = donor + Place(110, 1, 10820, 9415, "Item_6_0");
    Bytes p3{0x06, 0x00, 117, 0, 2, 0};
    const Bytes m = Translate(8937, 980);
    p3.insert(p3.end(), m.begin(), m.end());
    donor = donor + Tag(70, p3) + Tag(1, {});
    for (int r = 0; r < 6; ++r) donor = donor + Place(56 + 9 * r, 0, 1840, 1080 + 1143 * r, "");
    donor = donor + Place(110, 0, 10820, 9435, "") + Tag(1, {});
    Bytes top;
    for (int r = 0; r < 5; ++r) top = top + Place(31 + 7 * r, 1, 1481, -20 + 1353 * r, "Item_" + std::to_string(r) + "_0");
    top = top + Tag(1, {});
    Bytes list = Place(59, 1, 6211, 361, "ScrollBarV") + Tag(1, {});
    Bytes classes;
    Le(classes, 1, 2), Le(classes, 3, 2);
    const std::string cls = "OptionSetting_fla._ControllSetting";
    classes.insert(classes.end(), cls.begin(), cls.end()), classes.push_back(0);
    Bytes body = tags + Sprite(2, 1, title) + Sprite(3, 2, donor) + Sprite(4, 1, list) + Sprite(5, 1, top) +
                 Place(382, 3, 10046, 4134, "ControllSetting") +
                 (environment ? Place(300, 9, 10046, 4094, "EnvironmentSetting") : Bytes{}) +
                 Place(500, 5, 13754, 6089, "Top") + Tag(76, classes) +
                 Tag(1, {}) + Tag(0, {});
    Bytes head = Ascii("GFX");
    head.push_back(11);
    Le(head, 0, 4);
    head.insert(head.end(), {0x08, 0x00});
    Le(head, 30 << 8, 2), Le(head, 1, 2);
    Bytes all = head + body;
    for (int i = 0; i < 4; ++i) all[4 + i] = std::uint8_t(all.size() >> (8 * i));
    return all;
}
}

int main(int argc, char** argv) {
    const auto english = PcMessages(), chinese = PcMessages(true), russian = PcMessages(Language::Russian);
    assert(english.size() == chinese.size() && english.size() == russian.size() && english.size() > 150);
    assert(LanguageOf("rusru") == Language::Russian && LanguageOf("zhocn") == Language::Chinese &&
           LanguageOf("enggb") == Language::English);
    assert(russian.at(110010).text == "Эффекты (ПК)" && russian.at(110015).help == "Закрыть игру и вернуться на рабочий стол");
    assert(russian.at(119100).text == "Вперёд" && russian.at(119215).text.rfind(". Enter", 0) == 0);
    assert(russian.at(920000).text.find('\n') != std::string::npos);
    assert(russian.at(117010).text == "1280x720" && russian.at(116030).text == "30");  // untranslated rows stay English
    assert(russian.at(116020).help.empty());
    std::size_t translated = 0;
    for (const auto& [id, m] : russian) translated += m.text != english.at(id).text;
    assert(translated > 120);  // the table is translated, not copied from English
    assert(english.at(110010).text == "PC Effects" && chinese.at(110010).text == "PC 特效");
    assert(chinese.at(110015).help == "关闭游戏并返回桌面。");
    assert(chinese.at(117010).text == "1280x720");
    assert(english.at(920000).text.find('\n') != std::string::npos);
    assert(chinese.at(119100).text == "向前移动" && chinese.at(119129).text == "调试菜单" && chinese.at(119204).text == "其他");
    assert(english.at(119215).text.front() == ' ');
    assert(KindOf(110006) == Kind::Menu && KindOf(920001) == Kind::Question && KindOf(14000336) == Kind::Event);
    assert(Utf16("a😀") == std::u16string(u"a\U0001F600") && Utf16("한글") == u"한글");

    Fmg fmg;
    fmg.header.assign(0x28, 0);
    fmg.header[2] = 2, fmg.header[8] = 1, fmg.header[0x14] = 0xff;
    fmg.text = {{42, u"原有文字 한글 😀"}, {43, std::nullopt}, {44, u"A가"}, {90, u"A가"}};
    const Bytes fmg_bytes = FmgWrite(fmg);
    const Fmg back = FmgRead(fmg_bytes);
    assert(back.text == fmg.text && back.header[0x14] == 0xff && FmgWrite(back) == fmg_bytes);
    assert(fmg_bytes[0x0c] == 2 && fmg_bytes[0x10] == 4);

    const Binder bundle = Bundle(fmg);
    const Bytes packed = BinderWrite(bundle);
    const Binder reread = BinderRead(packed);
    assert(reread.files.size() == 6 && reread.wide_names);
    for (std::size_t i = 0; i < 6; ++i) {
        const std::size_t e = 0x40 + i * 0x24;
        assert((packed[e + 0x18] & 15) == 0 && reread.files[i].name == bundle.files[i].name &&
               reread.files[i].data == bundle.files[i].data && reread.files[i].flags == bundle.files[i].flags);
    }
    assert(BinderWrite(reread) == packed);

    const Bytes dcx = Packed(packed);
    assert(DcxRead(dcx).data == packed);
    Bytes short_dcx = dcx;
    short_dcx[0x1f] ^= 1;
    bool refused = false;
    try { DcxRead(short_dcx); } catch (const Error&) { refused = true; }
    assert(refused);

    for (const Language language : {Language::English, Language::Chinese, Language::Russian}) {
        for (const bool in_dcx : {false, true}) {
            const Bytes made = MenuMessages(in_dcx ? dcx : packed, language);
            const Binder out = BinderRead(in_dcx ? DcxRead(made).data : made);
            assert(out.files.size() == 6);
            const auto& expect = language == Language::Chinese ? chinese : language == Language::Russian ? russian : english;
            for (const BinderFile& f : out.files) {
                if (f.id == 999) {
                    assert(f.data == bundle.files.back().data);
                    continue;
                }
                const Fmg got = FmgRead(f.data);
                assert(got.text.at(42) == fmg.text.at(42) && !got.text.at(43) && got.text.at(90) == u"A가");
                const Kind kind = f.id == 30 ? Kind::Event : (f.id == 78 || f.id == 204) ? Kind::Question : Kind::Menu;
                for (const auto& [id, m] : expect) {
                    if (KindOf(id) != kind) {
                        assert(!got.text.count(id));
                        continue;
                    }
                    const std::string& s = f.id == 201 ? m.help : m.text;
                    assert(got.text.at(id) == (s.empty() ? std::nullopt : std::optional<std::u16string>(Utf16(s))));
                }
            }
        }
    }
    Fmg taken = fmg;
    taken.text[110006] = u"mine";
    refused = false;
    try { MenuMessages(BinderWrite(Bundle(taken))); } catch (const Error&) { refused = true; }
    assert(refused);

    const Bytes movie = OptionMovie(Movie());
    for (const char* name : {"PCGraphics", "PCControls", "PCKeys", "PCEffects", "PCCamera", "PCEnhance", "PCColour"})
        assert(Count(movie, std::string(name) + '\0') == 1);
    for (const char* title : {"117020", "118020", "119020", "120020", "121020", "124020", "125020"})
        assert(Count(movie, std::string("StaticText_") + title) == 1);
    assert(Count(movie, "StaticText_113020") == 1);
    assert(Count(movie, "Item_7_0") == 2);
    // + 1: Environment's Defaults moved on to Item_6_0 (its rows Item_0_0 .. Item_5_0, the new one Item_2_0).
    assert(Count(movie, "Item_6_0") == 1 + 1 + 2 + 1);
    assert(Count(movie, "Item_3_0") == 1 + 1 + 7 + 1);
    assert(Count(movie, "Item_2_0") == Count(OptionMovie(Movie(false)), "Item_2_0") + 1);
    assert(Count(movie, "EnvironmentSetting") == 1 && Count(movie, "StaticText_111011") == 1);
    {   // The window: parts whose top is under Display HUD one row (1050) lower, the rest where they were; the
        // flipped picture by its top (its origin is below Display HUD) and at its size; the background and the
        // thin line across Display HUD one row taller, their tops in place.
        const auto before = PlacedY(Movie(), 7), after = PlacedY(movie, 7);
        assert(after.at(4).y == before.at(4).y + 1050 && after.at(3).y == before.at(3).y && after.at(2).y == before.at(2).y);
        assert(after.at(5).y == before.at(5).y && after.at(5).sy == before.at(5).sy);
        assert(after.at(1).y == before.at(1).y && std::abs(after.at(1).sy - (9000.0 + 1050) / 9000) < 1e-4);
        assert(after.at(6).y == before.at(6).y && std::abs(after.at(6).sy - (1500.0 + 1050) / 1500) < 1e-4);
        // The screen: the new row one row under Display HUD, the Sound rows and Defaults one row lower.
        const auto env = PlacedY(movie, 9), old = PlacedY(Movie(), 9);
        assert(old.at(73).y == 2550 && env.at(73).y == 2550 && old.size() + 1 == env.size());
        for (const auto& [depth, a] : env)
            if (!old.count(depth)) assert(a.y == 2550 + 1050);  // the new row
        for (int depth : {82, 91, 100, 109, 117, 61}) assert(env.at(depth).y == old.at(depth).y + 1050);
        for (int depth : {3, 64, 116}) assert(env.at(depth).y == old.at(depth).y);
    }
    refused = false;
    try { OptionMovie(Movie(true, false)); } catch (const Error&) { refused = true; }
    assert(refused);  // an Environment not laid out as 1.09's: no movie (the PC menus stay off)
    assert(Count(movie, "ScrollBarV") == 2);
    assert(Count(movie, "OptionSetting_fla._ControllSetting") == 8);
    assert(movie[4] == (movie.size() & 0xff) && movie[5] == (movie.size() >> 8 & 0xff));
    refused = false;
    try { OptionMovie(movie); } catch (const Error&) { refused = true; }
    assert(refused);

    if (argc > 1) {
        std::ifstream in(argv[1], std::ios::binary);
        const Bytes dump((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const Bytes made = OptionMovie(dump);
        assert(made.size() > dump.size() && Count(made, "StaticText_119020") == 1 && Count(made, "ScrollBarV") == 2);
        std::printf("the dump's movie: %zu -> %zu bytes\n", dump.size(), made.size());
    }
    std::puts("PASS: menu assets: PC strings, FMG, BND4, DCX, messages in every group, the option movie");
}
