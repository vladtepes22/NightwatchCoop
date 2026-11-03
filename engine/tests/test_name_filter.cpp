// C++ half of the shared name-filter tests: tests/name_filter_vectors.txt (+ offensive, - fine) must give the same
// answers as scripts/namefilter.py (tests/test_name_filter.py runs the Python half and this binary).
// ninja -C out/gpu name-filter-test && out/gpu/name-filter-test tests/name_filter_vectors.txt
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include "party/name_filter.h"

int main(int argc, char** argv) {
    using namespace coop::namefilter;
    if (argc > 1 && std::string(argv[1]) == "--stdin") {  // differential mode: one name per line -> the tier or "-"
        std::string name;
        while (std::getline(std::cin, name)) {
            while (!name.empty() && name.back() == '\r') name.pop_back();
            const std::string r = Reason(name);
            std::printf("%s\n", r.empty() ? "-" : r.c_str());
        }
        return 0;
    }
    const char* path = argc > 1 ? argv[1] : "tests/name_filter_vectors.txt";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("cannot open %s\n", path);
        return 2;
    }
    std::string ln;
    int pos = 0, neg = 0, bad = 0;
    while (std::getline(in, ln)) {
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == '\n')) ln.pop_back();
        if (ln.size() < 3 || ln[0] == '#' || ln[1] != '\t') continue;
        const bool want = ln[0] == '+';
        const std::string name = ln.substr(2);
        const bool got = Offensive(name);
        (want ? pos : neg)++;
        if (got != want) {
            std::printf("MISMATCH want %s (%s), %zu bytes: line %d\n", want ? "offensive" : "fine", got ? "offensive" : "fine",
                        name.size(), pos + neg);
            ++bad;
        }
    }
    assert(MaskText("abc") == MaskText("abc"));
    assert(Masked("Gehrman") == "Gehrman");
    std::printf("name_filter vectors: %d positive, %d negative, %d wrong\n", pos, neg, bad);
    return bad ? 1 : 0;
}
