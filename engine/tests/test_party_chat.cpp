// SPDX-License-Identifier: GPL-3.0-or-later
// Party text chat (gpu/shim/party/party_chat.cpp): sanitising, EVENT party_chat JSON, the rate limits, the word masking
// with the offensive-name filter, and the line log.
// cmake --build out/gpu --target party-chat-test && out/gpu/party-chat-test.exe
#include "party/party_chat.h"

#include <cassert>
#include <cstdio>
#include <string>

namespace C = party::chat;

static void TestSanitise() {
    assert(C::Sanitise("Help!") == "Help!");
    assert(C::Sanitise("  a   b \t c  ") == "a b c");
    assert(C::Sanitise("") == "");
    assert(C::Sanitise("   ") == "");
    // Controls, zero width, bidi, soft hyphen, tags, private use and combining marks go.
    assert(C::Sanitise("a\x01\x7f" "b") == "ab");
    assert(C::Sanitise("a\xE2\x80\x8B" "b") == "ab");                 // U+200B zero width space
    assert(C::Sanitise("a\xE2\x80\xAE" "b\xE2\x81\xA6") == "ab");     // U+202E RLO, U+2066 LRI
    assert(C::Sanitise("a\xC2\xAD" "b") == "ab");                     // soft hyphen
    assert(C::Sanitise("a\xEF\xBB\xBF" "b") == "ab");                 // BOM
    assert(C::Sanitise("a\xF3\xA0\x81\x81" "b") == "ab");             // tag character
    assert(C::Sanitise("a\xEE\x80\x80" "b") == "ab");                 // private use
    assert(C::Sanitise("e\xCC\x81\xCC\x82\xCC\x83") == "e");          // combining marks (zalgo)
    assert(C::Sanitise("a\xC2\xA0" "b") == "a b");                    // no-break space -> a space
    // Real text survives: Cyrillic, accents, CJK.
    assert(C::Sanitise("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82") == "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
    assert(C::Sanitise("caf\xC3\xA9") == "caf\xC3\xA9");
    // Malformed UTF-8 is refused whole: lone continuation, truncated, overlong, surrogate, > U+10FFFF.
    assert(C::Sanitise("a\x80" "b").empty());
    assert(C::Sanitise("a\xE2\x82").empty());
    assert(C::Sanitise("\xC0\xAF").empty());
    assert(C::Sanitise("\xED\xA0\x80").empty());
    assert(C::Sanitise("\xF4\x90\x80\x80").empty());
    // The cap is in characters and never cuts a character.
    std::string long_ascii(300, 'x');
    assert(C::Sanitise(long_ascii).size() == std::size_t(C::kMaxChars));
    std::string long_ru;
    for (int i = 0; i < 300; ++i) long_ru += "\xD1\x8F";
    const std::string cut = C::Sanitise(long_ru);
    assert(C::CharCount(cut) == std::size_t(C::kMaxChars) && cut.size() == 2 * std::size_t(C::kMaxChars));
    assert(C::Sanitise("abcdef", 3) == "abc");
    assert(C::Sanitise("ab cd", 3) == "ab");  // the space and "c" would be 4
    // Presets are all fine as sent.
    for (int i = 0; i < C::kPresetCount; ++i) {
        const std::string p = C::Preset(i);
        assert(!p.empty() && C::Sanitise(p) == p);
        assert(C::MaskWords(p) == p);
    }
    assert(std::string(C::Preset(-1)).empty() && std::string(C::Preset(C::kPresetCount)).empty());
    assert(int(C::PresetList().size()) == C::kPresetCount);
}

static void TestJson() {
    C::ChatEvent e;
    e.text = "Follow me";
    e.t = 12345;
    C::ChatEvent back;
    std::string err;
    assert(C::FromJson(C::ToJson(e), &back, &err));
    assert(back.text == "Follow me" && back.t == 12345 && back.slot == -1 && back.who.empty());
    e.slot = 2;
    e.who = "Hunter_1";
    assert(C::FromJson(C::ToJson(e), &back, &err) && back.slot == 2 && back.who == "Hunter_1");
    // Unicode text round-trips through the JSON escaping.
    e.text = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xE2\x80\xA6";
    assert(C::FromJson(C::ToJson(e), &back, &err) && back.text == e.text);
    // The receiver sanitises again: invisible characters in a peer's text never reach the log.
    assert(C::FromJson("{\"text\":\"a\\u202eb\\u200bc\"}", &back, &err) && back.text == "abc");
    // Refused: no text, empty after sanitising, not a string, bad name / slot / time, not JSON, too long.
    assert(!C::FromJson("{}", &back, &err));
    assert(!C::FromJson("{\"text\":\"\"}", &back, &err));
    assert(!C::FromJson("{\"text\":\"\\u200b \\u202e\"}", &back, &err));
    assert(!C::FromJson("{\"text\":5}", &back, &err));
    assert(!C::FromJson("{\"text\":\"x\",\"who\":\"a b\"}", &back, &err));
    assert(!C::FromJson("{\"text\":\"x\",\"who\":\"ABCDEFGHIJKLMNOPQ\"}", &back, &err));
    assert(!C::FromJson("{\"text\":\"x\",\"slot\":8}", &back, &err));
    assert(!C::FromJson("{\"text\":\"x\",\"slot\":1.5}", &back, &err));
    assert(!C::FromJson("{\"text\":\"x\",\"t\":-1}", &back, &err));
    assert(!C::FromJson("not json", &back, &err));
    assert(!C::FromJson("[1]", &back, &err));
    assert(!C::FromJson("{\"text\":\"" + std::string(1100, 'x') + "\"}", &back, &err));
    // A long text is cut, not refused.
    assert(C::FromJson("{\"text\":\"" + std::string(500, 'x') + "\"}", &back, &err) && back.text.size() == 120);
}

static void TestRate() {
    C::Rate r(C::kRelayBurst, C::kRelayWindowS);
    int ok = 0;
    for (int i = 0; i < 10; ++i) ok += r.Take(1, 100.0);
    assert(ok == C::kRelayBurst);
    assert(!r.Take(1, 100.5));       // refilled 0.2 of a line
    assert(r.Take(1, 103.0));        // 2.5 s: one line back
    assert(r.Take(2, 100.0));        // another slot has its own bucket
    assert(!r.Take(-1, 1.0) && !r.Take(C::kMaxSlots, 1.0));
    C::SendGap g;
    assert(g.Take(10000));
    assert(!g.Take(10000 + C::kSendGapMs - 1));
    assert(g.Take(10000 + C::kSendGapMs));
}

static void TestMask() {
    // The shared name filter's words (data/name_filter): "cunt" is a strict-tier token.
    assert(C::MaskWords("hello there friend") == "hello there friend");
    assert(C::MaskWords("") == "");
    assert(C::MaskWords("you cunt, stop") == "you ****, stop");
    assert(C::MaskWords("Cunt!") == "****!");
    assert(C::MaskWords("c u n t") == "* * * *");                   // spaced letters: the 4 word run
    assert(C::MaskWords("go c.u.n.t go") == "go ******* go");       // separators folded by the normaliser
    // Not masked: other words, the Bloodborne vocabulary, the presets, names of places.
    const char* fine[] = {"Help!", "Follow me", "Wait", "Boss here", "Thanks", "Going to the Dream",
                          "the Hunter's Dream is up ahead", "Father Gascoigne is here", "Scunthorpe", "I'll kill the beast",
                          "use the lamp at Central Yharnam", "Cleric Beast down, Blood Starved next", "gg 1 2 3"};
    for (const char* t : fine) assert(C::MaskWords(t) == t);
    // The length and the surrounding punctuation are kept.
    assert(C::MaskWords("(cunt)") == "(****)");
}

static void TestLog() {
    C::ClearAll();
    C::Line l;
    l.until_ms = 1000;
    l.text = "a";
    for (int i = 0; i < 10; ++i) {
        l.slot = i % 4;
        l.text = std::to_string(i);
        l.until_ms = 1000 + i;
        C::Push(l);
    }
    auto lines = C::Read(0);
    assert(int(lines.size()) == C::kLogLines);
    assert(lines.front().text == "4" && lines.back().text == "9");  // the oldest went
    assert(lines[0].seq < lines[1].seq);
    lines = C::Read(1007);  // 1004..1006 expired
    assert(lines.size() == 2 && lines[0].text == "8");
    C::ClearAll();
    assert(C::Read(0).empty());
    assert(C::Alpha(0) == 0.0f && C::Alpha(C::kFadeMs) == 1.0f && C::Alpha(C::kFadeMs / 2) > 0.45f);
    assert(C::LifeLeft(0, 0) == C::kLifeMs);
    assert(C::LifeLeft(1000, 1000 + C::kLifeMs) == 0);
    assert(C::LifeLeft(1000, 1000 + 4000) == C::kLifeMs - 4000);
    assert(C::LifeLeft(10000, 4000) == 0);
}

int main() {
    TestSanitise();
    TestJson();
    TestRate();
    TestMask();
    TestLog();
    std::printf("party chat tests passed\n");
    return 0;
}
