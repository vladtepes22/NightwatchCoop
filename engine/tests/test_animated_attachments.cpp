// SPDX-License-Identifier: GPL-3.0-or-later
// Nightwatch feature animated_attachments: start/features.h (the json, merge, out file) and
// gpu/shim/party/animated_attachments_rules.h (the engine's reading of that file, the slot pick, the resource path).
// Build and run: ninja -C out/gpu animated-attachments-test && out/gpu/animated-attachments-test.exe
#include "start/features.h"
#include "gpu/shim/party/animated_attachments_rules.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace nwfeatures;
namespace aa = coop::attachments;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
        }                                                                                                              \
    } while (0)

// The contract of the Serpentry mod (bb-modding, 20 Nov 2026).
static const char* kSerpentry =
    R"({"attachments": [{"speffect": 9111, "part": "hd_a_9761"}, {"speffect": 9111, "part": "hd_a_9762"}]})";

static void ManifestUse() {
    Manifest m;
    std::string error;
    CHECK(ParseManifest(R"({"id":"serpentry","requires":"0.3.3","features":["animated_attachments"]})", &m, &error));
    CHECK(m.Uses(kAnimatedAttachments));
    bool known = false;
    for (const std::string& f : Known()) known = known || f == kAnimatedAttachments;
    CHECK(known);
}

static void Json() {
    std::vector<AttachmentRow> rows;
    std::string error;
    CHECK(ParseAnimatedAttachments(kSerpentry, &rows, &error) && rows.size() == 2 && rows[0].speffect == 9111 &&
          rows[0].part == "HD_A_9761" && rows[1].part == "HD_A_9762");
    CHECK(ParseAnimatedAttachments(R"({"version":1,"attachments":[]})", &rows, &error) && rows.empty());
    CHECK(ParseAnimatedAttachments("\xEF\xBB\xBF{\"attachments\":[]}", &rows, &error));  // BOM
    CHECK(!ParseAnimatedAttachments(R"({"version":2,"attachments":[]})", &rows, &error));
    CHECK(!ParseAnimatedAttachments(R"({"rows":[]})", &rows, &error) && error.find("attachments") != std::string::npos);
    CHECK(!ParseAnimatedAttachments("[1]", &rows, &error));
    CHECK(!ParseAnimatedAttachments("not json", &rows, &error));
    CHECK(!ParseAnimatedAttachments(R"({"attachments":[{"speffect":0,"part":"hd_a_9761"}]})", &rows, &error) &&
          error.find("row 1") != std::string::npos && rows.empty());
    CHECK(!ParseAnimatedAttachments(R"({"attachments":[{"speffect":"9111","part":"hd_a_9761"}]})", &rows, &error));
    CHECK(!ParseAnimatedAttachments(R"({"attachments":[{"speffect":9111}]})", &rows, &error));
    CHECK(!ParseAnimatedAttachments(R"({"attachments":[{"speffect":9111,"part":""}]})", &rows, &error));
    CHECK(!ParseAnimatedAttachments(R"({"attachments":[{"speffect":9111,"part":"../hd_a_9761"}]})", &rows, &error));
    CHECK(!ParseAnimatedAttachments(R"({"attachments":[{"speffect":9111,"part":"hd_a_9761.partsbnd"}]})", &rows, &error));
    CHECK(!ParseAnimatedAttachments(R"({"attachments":[{"speffect":9111,"part":"abcdefghijabcdefghijabcdefghijabc"}]})",
                                    &rows, &error));  // 33 characters
    CHECK(!ParseAnimatedAttachments(
              R"({"attachments":[{"speffect":9111,"part":"hd_a_9761"},{"speffect":9111,"part":"HD_A_9761"}]})", &rows,
              &error) &&
          error.find("second time") != std::string::npos);
    CHECK(ParseAnimatedAttachments(R"({"attachments":[{"speffect":2147483647,"part":"x_1","note":"y"}]})", &rows, &error) &&
          rows[0].part == "X_1");
    std::string many = R"({"attachments":[)";
    for (int i = 0; i < 65; ++i) many += std::string(i ? "," : "") + R"({"speffect":)" + std::to_string(i + 1) + R"(,"part":"p"})";
    many += "]}";
    CHECK(!ParseAnimatedAttachments(many, &rows, &error));
}

static void MergeAndEngine() {
    std::vector<AttachmentRow> a, b;
    std::string error;
    CHECK(ParseAnimatedAttachments(kSerpentry, &a, &error));
    CHECK(ParseAnimatedAttachments(R"({"attachments":[{"speffect":500,"part":"hd_a_1"},{"speffect":9111,"part":"hd_a_9761"}]})",
                                   &b, &error));
    AttachmentMerged merged;
    AddAnimatedAttachments(&merged, "serpentry", a);
    AddAnimatedAttachments(&merged, "empty", {});
    AddAnimatedAttachments(&merged, "other", b);
    CHECK(merged.names.size() == 2 && merged.rows.size() == 3);
    CHECK(merged.rows[0].part == "HD_A_9762" && merged.rows[1].speffect == 500 && merged.rows[2].part == "HD_A_9761");
    const std::string text = AnimatedAttachmentsFileText(merged);
    CHECK(text.rfind("# animated_attachments", 0) == 0);
    CHECK(text.find("serpentry, other") != std::string::npos);
    CHECK(text.find("\n9111 HD_A_9762\n500 HD_A_1\n9111 HD_A_9761\n") != std::string::npos);
    CHECK(AnimatedAttachmentsFileText(AttachmentMerged{}).empty());

    std::vector<aa::Row> rows;
    std::size_t bad = 0;
    CHECK(aa::ParseTable(text, &rows) && rows.size() == 3 && rows[0].speffect == 9111 && rows[0].part == "HD_A_9762" &&
          rows[2].part == "HD_A_9761");
    CHECK(aa::ParseTable("", &rows) && rows.empty());
    CHECK(aa::ParseTable("# x\n\r\n  \n9111 HD_A_9761 # c\r\n", &rows) && rows.size() == 1);
    CHECK(!aa::ParseTable("9111 HD_A_9761\n9111 hd_a_9761\n", &rows, &bad) && bad == 2 && rows.empty());
    CHECK(!aa::ParseTable("9111\n", &rows, &bad) && bad == 1);
    CHECK(!aa::ParseTable("0 HD_A_1\n", &rows));
    CHECK(!aa::ParseTable("2147483648 HD_A_1\n", &rows));
    CHECK(!aa::ParseTable("-5 HD_A_1\n", &rows));
    CHECK(!aa::ParseTable("5 HD_A_1 extra\n", &rows));
    CHECK(!aa::ParseTable("5 HD/A\n", &rows));
    std::string lots;
    for (int i = 1; i <= 65; ++i) lots += std::to_string(i) + " P\n";
    CHECK(!aa::ParseTable(lots, &rows, &bad) && bad == 65);
}

static void Pick() {
    std::vector<aa::Row> rows;
    CHECK(aa::ParseTable("9111 HD_A_9761\n9111 HD_A_9762\n500 HD_A_1\n600 HD_A_2\n", &rows));
    std::set<int> on{9111};
    int dropped = -1;
    auto has = [&](std::int32_t sp) { return on.count(sp) > 0; };
    std::vector<int> w = aa::Wanted(rows, has, &dropped);
    CHECK(w.size() == 2 && w[0] == 0 && w[1] == 1 && dropped == 0);
    on = {9111, 500};
    w = aa::Wanted(rows, has, &dropped);
    CHECK(w.size() == 2 && w[0] == 0 && w[1] == 1 && dropped == 1);  // only two slots: table order wins
    on = {600, 500};
    w = aa::Wanted(rows, has, &dropped);
    CHECK(w.size() == 2 && w[0] == 2 && w[1] == 3 && dropped == 0);
    on = {};
    CHECK(aa::Wanted(rows, has).empty());
}

static void Path() {
    CHECK(aa::ResourcePath("HD_A_9761", false) == u"parts:/HD_A_9761.partsbnd");
    CHECK(aa::ResourcePath("HD_A_9761", true) == u"parts:/HD_A_9761_L.partsbnd");
    CHECK(aa::ValidPart("HD_A_9761") && !aa::ValidPart("hd_a_9761") && !aa::ValidPart("") && !aa::ValidPart("A.B"));
}

int main() {
    ManifestUse();
    Json();
    MergeAndEngine();
    Pick();
    Path();
    std::printf("animated-attachments-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
