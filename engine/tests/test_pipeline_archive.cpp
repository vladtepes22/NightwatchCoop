// SPDX-License-Identifier: GPL-2.0-or-later
// Deterministic malformed-cache checks. No device or files required.
#include <cassert>
#include <limits>
#include "common/serdes.h"

int main() {
    using namespace Serialization;
    Archive data;
    Writer writer{data};
    writer.Write(std::vector<u32>{1, 2, 3});
    // Start a fresh archive cursor.
    Archive input{data.TakeOff()};
    Reader input_reader{input};
    std::vector<u32> values{99};
    input_reader.Read(values);
    assert((values == std::vector<u32>{1, 2, 3}));

    const auto rejects = [](auto write, auto read) {
        Archive encoded;
        Writer out{encoded};
        write(out);
        Archive input{encoded.TakeOff()};
        Reader in{input};
        bool rejected = false;
        try {
            read(in);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected);
    };
    rejects([](Writer& w) { w.Write(size_t{8}); },
            [](Reader& r) { std::vector<u32> v; r.Read(v); });
    rejects([](Writer& w) { w.Write(std::numeric_limits<size_t>::max()); },
            [](Reader& r) { std::string s; r.Read(s); });
    rejects([](Writer& w) { w.Write(u8{1}); },
            [](Reader& r) { u64 v; r.Read(v); });
    // The SRT walker must check its full size before registering executable code.
    rejects([](Writer& w) { w.Write(u8{1}); },
            [](Reader& r) { r.ar.Require(2); });

    // bbport (0.9.37 beta 6): writes past room an earlier Grow left add only what is missing. The
    // fetch shader data grew the archive first and every later write added its full size on top:
    // vertex shader records ended in zero bytes the reader took for one more part, and their
    // pipelines were left out of the start's warm-up.
    Archive grown;
    Writer grown_writer{grown};
    grown.Grow(6);
    grown_writer.Write(u32{1});
    grown_writer.Write(u64{2});
    assert(grown.TakeOff().size() == 12);
}
