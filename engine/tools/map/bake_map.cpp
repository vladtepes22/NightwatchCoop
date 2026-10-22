// SPDX-License-Identifier: GPL-2.0-or-later
// Map baker: compiles the area maps made by the Hunter's Atlas generator (run on the player's
// PC, from their game files) into the vector format the in-game map draws. Runs offline, once.
//
// Adapted from Hunter's Atlas (github.com/MikeTat1988/HuntersAtlas, GPL-2.0-or-later):
// source/compact/bake_vectors.cpp, plus the floor grid and primary floor of
// source/atlas2/bake.cpp. Atlas computes those in a raster stage whose tiles the vector map never
// reads; here they come straight from the source map, with the same results byte for byte.
//
//   bb-map-bake <maps dir> <output dir> [jobs]
//
// <maps dir> holds atlas.json, features.json and one <region>.json per area. The output dir must
// not exist: the areas are baked into "<output>.partial" and renamed on success. Prints
// "MAP_AREA_READY AreaNN" per area and "MAP_READY" at the end.
#include "core.h"
#include "bbport_atlas_interiors.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

namespace {
using namespace Atlas2;
using BbAtlasStyle::Line;

// Reveal regions (fog of war) a piece of geometry needs: the first region each point is in.
std::vector<int> Requirements(const Json& rules, const std::vector<std::array<float, 3>>& points) {
    std::set<int> result;
    for (auto p : points) {
        const int id = Region(rules, p[0], p[1], p[2]);
        if (id != -2) result.insert(id);
    }
    return {result.begin(), result.end()};
}

std::vector<int> BoxRequirements(const Json& rules, float x0, float z0, float x1, float z1, float y) {
    return Requirements(rules, {{x0, y, z0}, {x1, y, z1}, {(x0 + x1) * .5f, y, (z0 + z1) * .5f}});
}

std::vector<int> LineRequirements(const Json& rules, Line l, float y) {
    return BoxRequirements(rules, std::min(l.a.x, l.b.x), std::min(l.a.z, l.b.z),
                           std::max(l.a.x, l.b.x), std::max(l.a.z, l.b.z), y);
}

// Vertex layout read by the map's vertex shader (20 bytes).
struct Vertex {
    float x, z, u, v;
    uint32_t colour;
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    Json groups = Json::array();  // [first index, index count, required reveal regions]

    void Group(std::vector<int> required, uint32_t count) {
        if (!count) return;
        if (!groups.empty() && groups.back()[2] == required)
            groups.back()[1] = groups.back()[1].get<uint32_t>() + count;
        else
            groups.push_back({uint32_t(indices.size()) - count, count, required});
    }
    void Triangle(std::array<float, 6> v, std::array<float, 3> y, std::vector<int> required) {
        const uint32_t base = uint32_t(vertices.size());
        for (int k = 0; k < 3; ++k) vertices.push_back({v[k * 2], -v[k * 2 + 1], y[k], 0, 0xffffffffu});
        for (int k = 0; k < 3; ++k) indices.push_back(base + k);
        Group(std::move(required), 3);
    }
    // A stroke as a 4-vertex fan per end; the shader extrudes it along (u, v) by the line width.
    void Stroke(Line line, std::vector<int> required, int wear1 = 0, int wear2 = 0) {
        const float dx = line.b.x - line.a.x, dz = -(line.b.z - line.a.z), length = std::hypot(dx, dz);
        if (length < 1e-8) return;
        const float nx = -dz / length, nz = dx / length;
        const uint32_t base = uint32_t(vertices.size());
        for (auto p : {line.a, line.b})
            for (int k = 0; k < 4; ++k) {
                const float sign = k < 2 ? 1.f : -1.f;
                const uint32_t alpha = (k == 0 || k == 3) ? 0 : 255;
                vertices.push_back({p.x, -p.z, sign * nx, sign * nz,
                                    uint32_t(wear1) | (uint32_t(wear2) << 8) | (alpha << 24)});
            }
        for (int k = 0; k < 3; ++k)
            for (int i : {k, k + 4, k + 5, k, k + 5, k + 1}) indices.push_back(base + i);
        Group(std::move(required), 18);
    }
    Json Pack() const {
        std::vector<uint8_t> v(vertices.size() * sizeof(Vertex)), i(indices.size() * 4);
        if (!v.empty()) std::memcpy(v.data(), vertices.data(), v.size());
        if (!i.empty()) std::memcpy(i.data(), indices.data(), i.size());
        return {{"vertices", Json::binary(v)}, {"indices", Json::binary(i)}, {"groups", groups}};
    }
};

// "A3Z1", raw size, zlib(msgpack). Returns the file's SHA-256 for the manifest.
std::string Store(const fs::path& path, const Json& value) {
    const auto raw = Json::to_msgpack(value);
    uLongf size = compressBound(uLong(raw.size()));
    std::vector<unsigned char> data = {'A', '3', 'Z', '1'};
    U32(data, uint32_t(raw.size()));
    data.resize(size + 8);
    if (compress2(data.data() + 8, &size, raw.data(), uLong(raw.size()), 9) != Z_OK)
        throw std::runtime_error("compression failed");
    data.resize(size + 8);
    Write(path, data);
    return Hash(data);
}

// One area: base.a3z (floors, outlines, walls, interiors), hatch-2..17.a3z (decorative strokes
// per zoom level), grid.a3z (floor lookup by position) and manifest.json.
void BakeArea(const Json& source, const Json& rules, const fs::path& out) {
    fs::create_directories(out);
    Json files = Json::object();
    std::vector<BbAtlasStyle::Triangle> all;
    std::vector<BbAtlasStyle::FloorIndex> per;
    std::vector<std::vector<BbAtlasInteriors::Ring>> rings;
    std::vector<std::vector<Line>> walls;
    Grid grid;
    Json layers = Json::array();  // manifest rows
    double largest = 0;
    int primary = -1;  // the floor with the largest footprint, shown first
    Json base = {{"schema", 1}, {"layers", Json::array()}};

    for (size_t li = 0; li < source.at("layers").size(); ++li) {
        const auto& layer = source["layers"][li];
        const float anchor = layer.at("anchor");
        Mesh floors, outlines, wall;
        std::vector<BbAtlasStyle::Triangle> tris;
        std::vector<BbAtlasInteriors::Ring> rs;
        std::vector<Line> ws;
        float low = anchor, high = anchor;
        double footprint = 0;
        grid.anchors.push_back(anchor);

        for (size_t i = 0; i < layer.at("triangles").size(); ++i) {
            const auto v = layer["triangles"][i].get<std::array<float, 6>>();
            const auto y = layer["heights"][i].get<std::array<float, 3>>();
            for (auto h : y) {
                low = std::min(low, h);
                high = std::max(high, h);
            }
            const float x0 = std::min({v[0], v[2], v[4]}), z0 = std::min({v[1], v[3], v[5]});
            const float x1 = std::max({v[0], v[2], v[4]}), z1 = std::max({v[1], v[3], v[5]});
            floors.Triangle(v, y, Requirements(rules, {{(v[0] + v[2] + v[4]) / 3, (y[0] + y[1] + y[2]) / 3, (v[1] + v[3] + v[5]) / 3},
                                                       {v[0], y[0], v[1]}, {v[2], y[1], v[3]}, {v[4], y[2], v[5]},
                                                       {x0, anchor, z0}, {x1, anchor, z1},
                                                       {(x0 + x1) * .5f, anchor, (z0 + z1) * .5f}}));
            tris.push_back({{{{v[0], v[1]}, {v[2], v[3]}, {v[4], v[5]}}}});
            grid.surfaces.push_back({v, y, int(li)});
            footprint += std::abs((v[2] - v[0]) * (v[5] - v[1]) - (v[4] - v[0]) * (v[3] - v[1])) * .5;
        }
        for (const auto& ring : layer.at("outlines")) {
            BbAtlasInteriors::Ring r;
            for (const auto& p : ring) r.push_back({p[0], p[1]});
            for (size_t k = 0; k < r.size(); ++k) {
                const Line l{r[k], r[(k + 1) % r.size()]};
                outlines.Stroke(l, LineRequirements(rules, l, anchor));
            }
            rs.push_back(std::move(r));
        }
        for (const auto& w : layer.at("walls")) {
            const Line l{{w[0], w[1]}, {w[2], w[3]}};
            wall.Stroke(l, LineRequirements(rules, l, anchor));
            ws.push_back(l);
        }
        base["layers"].push_back({{"anchor", anchor}, {"span", layer["span"]}, {"low", low}, {"high", high},
                                  {"floors", floors.Pack()}, {"outlines", outlines.Pack()}, {"walls", wall.Pack()}});
        layers.push_back({{"height", layer.at("height")}, {"span", layer.at("span")}, {"anchor", anchor}, {"footprint", footprint}});
        if (footprint > largest) {
            largest = footprint;
            primary = int(li);
        }
        all.insert(all.end(), tris.begin(), tris.end());
        per.emplace_back();
        per.back().Build(std::move(tris));
        rings.push_back(std::move(rs));
        walls.push_back(std::move(ws));
    }
    grid.Build();
    files["grid.a3z"] = Store(out / "grid.a3z", grid.ToJson());

    BbAtlasStyle::CutCache cut;
    cut.Build(std::move(all), rings.size());
    BbAtlasInteriors::Cache interiors;
    interiors.Build(rings, per);
    for (size_t li = 0; li < rings.size(); ++li) {
        Mesh fills;
        const float y = base["layers"][li]["anchor"];
        for (const auto& t : interiors.layers[li]) {
            const auto& p = t.p;
            fills.Triangle({p[0].x, p[0].z, p[1].x, p[1].z, p[2].x, p[2].z}, {y, y, y},
                           BoxRequirements(rules, std::min({p[0].x, p[1].x, p[2].x}), std::min({p[0].z, p[1].z, p[2].z}),
                                           std::max({p[0].x, p[1].x, p[2].x}), std::max({p[0].z, p[1].z, p[2].z}), y));
        }
        base["layers"][li]["interiors"] = fills.Pack();
    }
    files["base.a3z"] = Store(out / "base.a3z", base);
    base = nullptr;

    // The map's zoom range (.15..25) uses hatch levels 2..15; 16 and 17 cover large UI scales.
    for (int lod = 2; lod <= 17; ++lod) {
        Json hatch_layers = Json::array();
        for (size_t li = 0; li < rings.size(); ++li) {
            Mesh hatch, interior;
            const float y = source["layers"][li]["anchor"];
            for (auto l : cut.Get(li, lod, walls[li])) hatch.Stroke(l, LineRequirements(rules, l, y), 23 + l.wear % 30, 0);
            for (auto l : interiors.Get(li, lod)) interior.Stroke(l, LineRequirements(rules, l, y), 28 + l.wear % 20, 15 + l.wear % 13);
            hatch_layers.push_back({{"cut", hatch.Pack()}, {"interior", interior.Pack()}});
            cut.layers[li][lod].strokes.clear();
            interiors.hatches[li][lod].lines.clear();
        }
        const auto name = "hatch-" + std::to_string(lod) + ".a3z";
        files[name] = Store(out / name, {{"schema", 1}, {"layers", hatch_layers}});
    }

    WriteJson(out / "manifest.json", {{"schema", 4}, {"region_id", source.at("id")}, {"bounds", source.at("bounds")},
                                      {"primary", primary}, {"layers", layers}, {"files", files}});
}

int Run(int argc, char** argv) {
    if (argc != 3 && argc != 4) throw std::runtime_error("usage: bb-map-bake <maps dir> <output dir> [jobs]");
    const fs::path maps = argv[1], output = argv[2];
    fs::path partial = output;
    partial += ".partial";
    if (fs::exists(output)) throw std::runtime_error("output already exists: " + output.string());
    fs::remove_all(partial);  // left over from an interrupted bake

    const auto atlas = ReadJson(maps / "atlas.json"), features = ReadJson(maps / "features.json");
    const auto& records = atlas.at("maps");
    if (records.empty() || records.size() > 99) throw std::runtime_error("atlas.json: unexpected area count");

    // Large areas peak around 1-2 GB, so the job count stays small by default.
    unsigned jobs = argc == 4 ? unsigned(std::stoul(argv[3])) : std::min(4u, std::max(1u, std::thread::hardware_concurrency() / 2));
    jobs = std::clamp(jobs, 1u, unsigned(records.size()));

    std::vector<std::string> labels(records.size()), regions(records.size());
    for (size_t i = 0; i < records.size(); ++i) {
        char label[8];
        std::snprintf(label, sizeof label, "Area%02zu", i + 1);
        labels[i] = label;
        regions[i] = records[i].at("id").get<std::string>();
        if (regions[i].find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
            throw std::runtime_error("atlas.json: bad area id");
    }

    std::atomic<size_t> next{0};
    std::mutex lock;
    std::string failure;
    auto worker = [&] {
        for (size_t i; (i = next++) < records.size();) {
            {
                std::lock_guard guard(lock);
                if (!failure.empty()) return;
            }
            try {
                const auto source = ReadJson(maps / (regions[i] + ".json"), 512 * 1024 * 1024);
                if (source.at("id") != regions[i]) throw std::runtime_error("id mismatch");
                BakeArea(source, features.at("areas").at(regions[i]), partial / labels[i]);
                std::lock_guard guard(lock);
                std::printf("MAP_AREA_READY %s\n", labels[i].c_str());
                std::fflush(stdout);
            } catch (const std::exception& e) {
                std::lock_guard guard(lock);
                if (failure.empty()) failure = labels[i] + " (" + regions[i] + "): " + e.what();
            }
        }
    };
    std::vector<std::thread> threads;
    for (unsigned j = 0; j < jobs; ++j) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    if (!failure.empty()) {
        std::error_code ignored;
        fs::remove_all(partial, ignored);
        throw std::runtime_error(failure);
    }

    // The index pins every area's manifest; the game checks the index's own hash from its env.
    Json index = {{"schema", 1}, {"format", "compiled-vector-v1"}, {"areas", Json::array()}};
    for (size_t i = 0; i < records.size(); ++i)
        index["areas"].push_back({{"area", labels[i]}, {"region_id", regions[i]},
                                  {"manifest_sha256", Hash(Read(partial / labels[i] / "manifest.json"))}});
    WriteJson(partial / "atlas2-index.json", index);
    fs::rename(partial, output);
    std::printf("MAP_READY %zu\n", records.size());
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        return Run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "MAP_BAKE_ERROR: %s\n", e.what());
        return 1;
    }
}
