// SPDX-License-Identifier: GPL-3.0-or-later
// Test walker over the game's own navmesh (docs/party/walker.md). Header-only, no game access, so the unit test
// (tests/test_nav_walk.cpp) runs the planner and the steering on synthetic meshes.
//
//   NavMesh      the per-area cache written by tools/party/nav_cache.py (hkaiNavMesh faces and their links), face
//                lookup (point in polygon, snap), A* over faces and a funnel (string pull) with an agent margin.
//   CamModel     stick angle from the camera yaw; the camera offset is taken from the camera (player - camera
//                position) and, when no camera is readable, learned from the observed motion.
//   Walker       pure pursuit on the planned corridor with a continuous stick, slew-limited turns, ease-off at the
//                goal, replan off the corridor, stall recovery (replan, back up and strafe, roll, last resort a
//                counted warp onto a path point).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace navwalk {

constexpr double kPi = 3.14159265358979323846;

struct V3 {
    float x = 0, y = 0, z = 0;
};

inline double Wrap(double a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}
inline double Dist2(const V3& a, const V3& b) { return std::hypot(double(a.x) - b.x, double(a.z) - b.z); }
inline double Dist3(const V3& a, const V3& b) {
    return std::sqrt(std::pow(double(a.x) - b.x, 2) + std::pow(double(a.y) - b.y, 2) + std::pow(double(a.z) - b.z, 2));
}

enum class PlanResult { Ok, NoMesh, StartOff, GoalOff, NoRoute };
inline const char* PlanName(PlanResult r) {
    switch (r) {
        case PlanResult::Ok: return "ok";
        case PlanResult::NoMesh: return "no navmesh for this area";
        case PlanResult::StartOff: return "start is off the navmesh";
        case PlanResult::GoalOff: return "goal is off the navmesh";
        case PlanResult::NoRoute: return "no route (goal on another navmesh island)";
    }
    return "?";
}

struct PlanInfo {
    double snap_start = 0, snap_goal = 0;  // m the start / goal were moved onto the mesh
    V3 goal_on_mesh;
    std::size_t faces = 0;
    double length = 0;
};

class NavMesh {
public:
    /// Builds from polygons (tests, tools): each face is a convex polygon (counter-clockwise or not) and links are
    /// derived from shared vertex pairs.
    void FromPolys(const std::vector<std::vector<V3>>& polys) {
        verts_.clear(), idx_.clear(), faces_.clear(), link_start_.clear(), links_.clear();
        std::unordered_map<std::uint64_t, int> vkey;
        auto vid = [&](const V3& p) {
            const std::uint64_t k = (std::uint64_t(std::llround(p.x * 100)) & 0x1fffff) << 42 |
                                    (std::uint64_t(std::llround(p.y * 100)) & 0x1fffff) << 21 |
                                    (std::uint64_t(std::llround(p.z * 100)) & 0x1fffff);
            auto it = vkey.find(k);
            if (it != vkey.end()) return it->second;
            verts_.push_back(p);
            return vkey[k] = int(verts_.size()) - 1;
        };
        std::vector<std::vector<int>> pv;
        for (const auto& p : polys) {
            std::vector<int> v;
            for (const V3& q : p) v.push_back(vid(q));
            pv.push_back(v);
        }
        std::unordered_map<std::uint64_t, std::pair<int, int>> edge;  // sorted vertex pair -> face, edge index
        std::vector<std::vector<Link>> lk(pv.size());
        for (std::size_t f = 0; f < pv.size(); ++f) {
            Face fc;
            fc.first = std::uint32_t(idx_.size());
            fc.count = std::uint32_t(pv[f].size());
            for (int v : pv[f]) idx_.push_back(std::uint32_t(v));
            faces_.push_back(fc);
            for (std::size_t k = 0; k < pv[f].size(); ++k) {
                const int a = pv[f][k], b = pv[f][(k + 1) % pv[f].size()];
                const std::uint64_t key = (std::uint64_t(std::min(a, b)) << 32) | std::uint32_t(std::max(a, b));
                auto it = edge.find(key);
                if (it == edge.end()) {
                    edge[key] = {int(f), int(k)};
                } else {
                    const int g = it->second.first;
                    lk[f].push_back({std::uint32_t(g), verts_[a], verts_[b]});
                    lk[g].push_back({std::uint32_t(f), verts_[a], verts_[b]});
                }
            }
        }
        link_start_.push_back(0);
        for (auto& l : lk) {
            for (const Link& x : l) links_.push_back(x);
            link_start_.push_back(std::uint32_t(links_.size()));
        }
        Finish();
    }

    /// The BBNV cache file (tools/party/nav_cache.py).
    bool Load(const std::string& path, std::string* err = nullptr) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            if (err) *err = "cannot open " + path;
            return false;
        }
        std::vector<std::uint8_t> buf;
        std::uint8_t tmp[1 << 16];
        std::size_t n;
        while ((n = std::fread(tmp, 1, sizeof tmp, f)) > 0) buf.insert(buf.end(), tmp, tmp + n);
        std::fclose(f);
        return LoadBuffer(buf.data(), buf.size(), err);
    }

    bool LoadBuffer(const std::uint8_t* b, std::size_t n, std::string* err = nullptr) {
        auto bad = [&](const char* m) {
            if (err) *err = m;
            return false;
        };
        if (n < 24 || std::memcmp(b, "BBNV", 4) != 0) return bad("not a BBNV file");
        std::uint32_t h[5];
        std::memcpy(h, b + 4, sizeof h);
        if (h[0] != 1) return bad("BBNV version");
        const std::size_t nv = h[1], nf = h[2], nl = h[3], ni = h[4];
        const std::size_t need = 24 + nv * 12 + nf * 12 + ni * 4 + (nf + 1) * 4 + nl * 28 + 4;
        if (n < need) return bad("BBNV truncated");
        const std::uint8_t* p = b + 24;
        verts_.resize(nv);
        std::memcpy(verts_.data(), p, nv * 12);
        p += nv * 12;
        faces_.resize(nf);
        map_of_.assign(nf, 0);
        for (std::size_t i = 0; i < nf; ++i) {
            std::uint32_t t[3];
            std::memcpy(t, p, 12);
            p += 12;
            faces_[i].first = t[0];
            faces_[i].count = t[1];
            map_of_[i] = std::uint16_t(t[2]);
        }
        idx_.resize(ni);
        std::memcpy(idx_.data(), p, ni * 4);
        p += ni * 4;
        link_start_.resize(nf + 1);
        std::memcpy(link_start_.data(), p, (nf + 1) * 4);
        p += (nf + 1) * 4;
        links_.resize(nl);
        for (std::size_t i = 0; i < nl; ++i) {
            std::uint32_t fc;
            float q[6];
            std::memcpy(&fc, p, 4);
            std::memcpy(q, p + 4, 24);
            p += 28;
            links_[i] = {fc, {q[0], q[1], q[2]}, {q[3], q[4], q[5]}};
        }
        for (const Face& fc : faces_)
            if (std::size_t(fc.first) + fc.count > ni) return bad("BBNV bad face");
        for (const Link& l : links_)
            if (l.face >= nf) return bad("BBNV bad link");
        Finish();
        return true;
    }

    std::size_t FaceCount() const { return faces_.size(); }
    bool Empty() const { return faces_.empty(); }

    /// Face containing (x, z) whose surface is closest to y (within `max_dy`), or -1. The height is interpolated
    /// over the polygon's triangle fan.
    int FaceAt(const V3& p, double max_dy = 2.5, float* y_out = nullptr) const {
        // Pieces of the game's navmesh overlap (a face inside another piece's face): among the faces at the point whose
        // surface is as near in height as the nearest one (+0.4 m), take the best linked one.
        struct C {
            std::uint32_t f;
            double dy, y;
        };
        std::vector<C> c;
        double min_dy = 1e30;
        ForCell(p.x, p.z, [&](std::uint32_t f) {
            double y;
            if (Contains(f, p.x, p.z, &y) && std::fabs(y - p.y) < max_dy) {
                c.push_back({f, std::fabs(y - p.y), y});
                min_dy = std::min(min_dy, std::fabs(y - p.y));
            }
        });
        int best = -1;
        double best_score = 0;
        for (const C& x : c) {
            if (x.dy > min_dy + 0.4) continue;
            // links first, then the larger polygon (the small overlapping patch is the odd one)
            const double score = (link_start_[x.f + 1] - link_start_[x.f]) * 1000.0 + Area(x.f);
            if (best < 0 || score > best_score) {
                best = int(x.f);
                best_score = score;
                if (y_out) *y_out = float(x.y);
            }
        }
        return best;
    }

    /// The point of the mesh closest to p within `radius` m (3D-weighted), or -1.
    int Snap(const V3& p, double radius, V3* on) const {
        int best = -1;
        double bd = radius;
        const int c = int(std::ceil(radius / kCell));
        for (int dx = -c; dx <= c; ++dx)
            for (int dz = -c; dz <= c; ++dz) {
                auto it = grid_.find(CellKey(CellOf(p.x) + dx, CellOf(p.z) + dz));
                if (it == grid_.end()) continue;
                for (std::uint32_t f : it->second) {
                    V3 q;
                    const double d = ClosestOnFace(f, p, &q);
                    if (d < bd) {
                        bd = d;
                        best = int(f);
                        *on = q;
                    }
                }
            }
        return best;
    }

    /// Plans start -> goal and appends the funnelled path (start .. goal, on the mesh) to `out`.
    PlanResult Plan(const V3& start, const V3& goal, std::vector<V3>* out, PlanInfo* info = nullptr,
                    double start_snap = 2.5, double goal_snap = 4.0, bool goal_y_known = true) const {
        if (Empty()) return PlanResult::NoMesh;
        V3 s = start, g = goal;
        int fs = FaceAt(start, 2.5, &s.y);
        double snap_s = 0;
        if (fs < 0) {
            fs = Snap(start, start_snap, &s);
            snap_s = fs < 0 ? 0 : Dist3(start, s);
        }
        if (fs < 0) return PlanResult::StartOff;
        int fg;
        double snap_g = 0;
        if (goal_y_known) {
            fg = FaceAt(goal, 2.5, &g.y);
            if (fg < 0) {
                fg = Snap(goal, goal_snap, &g);
                snap_g = fg < 0 ? 0 : Dist3(goal, g);
            }
        } else {  // y unknown (hand-written X:Z waypoints): the floor nearest the start height, else any
            fg = FaceAt({goal.x, start.y, goal.z}, 6.0, &g.y);
            if (fg < 0) {
                fg = Snap({goal.x, start.y, goal.z}, goal_snap, &g);
                snap_g = fg < 0 ? 0 : Dist3({goal.x, start.y, goal.z}, g);
            }
            if (fg < 0) {  // no floor near the start's height at that X:Z (a boss arena on another storey): any floor of the column
                fg = FaceAt({goal.x, start.y, goal.z}, 400.0, &g.y);
                snap_g = 0;
            }
        }
        if (fg < 0) return PlanResult::GoalOff;
        std::vector<int> corridor;
        if (!Astar(fs, fg, s, g, &corridor)) return PlanResult::NoRoute;
        std::vector<V3> pts = Funnel(s, g, corridor);
        if (info) {
            info->snap_start = snap_s;
            info->snap_goal = snap_g;
            info->goal_on_mesh = g;
            info->faces = corridor.size();
            info->length = 0;
            for (std::size_t i = 1; i < pts.size(); ++i) info->length += Dist3(pts[i - 1], pts[i]);
        }
        out->insert(out->end(), pts.begin(), pts.end());
        return PlanResult::Ok;
    }

    /// True when the segment a-b lies on the mesh (any piece: pieces overlap) with `clear` m of mesh to both sides and no
    /// height step above 0.5 m between samples.
    bool Walkable(const V3& a, const V3& b, double clear = 0.35) const {
        const double len = Dist2(a, b);
        if (len < 1e-4) return true;
        const int n = int(std::ceil(len / 0.4));
        const double dx = (double(b.x) - a.x) / len, dz = (double(b.z) - a.z) / len;
        double prev_y = a.y;
        for (int i = 0; i <= n; ++i) {
            const double t = double(i) / n;
            V3 q{float(a.x + (double(b.x) - a.x) * t), float(a.y + (double(b.y) - a.y) * t), float(a.z + (double(b.z) - a.z) * t)};
            float y;
            if (FaceAt(q, 0.8, &y) < 0 || std::fabs(y - prev_y) > 0.5) return false;
            prev_y = y;
            if (clear > 0) {
                for (int side : {-1, 1}) {
                    V3 e{float(q.x - dz * clear * side), y, float(q.z + dx * clear * side)};
                    if (FaceAt(e, 0.8) < 0) return false;
                }
            }
        }
        return true;
    }

    /// Greedy shortcut of a path: from each point, the farthest later point reachable by a walkable straight segment
    /// (points off the mesh, such as a lift car tail, are kept).
    std::vector<V3> Smooth(const std::vector<V3>& in) const {
        if (in.size() < 3) return in;
        std::vector<V3> out{in[0]};
        std::size_t i = 0;
        while (i + 1 < in.size()) {
            std::size_t best = i + 1;
            const std::size_t last = std::min(in.size() - 1, i + 40);
            for (std::size_t j = last; j > i + 1; --j) {
                if (Walkable(in[i], in[j])) {
                    best = j;
                    break;
                }
            }
            out.push_back(in[best]);
            i = best;
        }
        return out;
    }

    double agent_radius = 0.45;  // portal ends are pulled in by this much: the walk keeps off wall corners

private:
    struct Face {
        std::uint32_t first = 0, count = 0;
    };
    struct Link {
        std::uint32_t face;
        V3 a, b;
    };
    static constexpr double kCell = 8.0;

    std::vector<V3> verts_;
    std::vector<std::uint32_t> idx_;
    std::vector<Face> faces_;
    std::vector<std::uint16_t> map_of_;
    std::vector<std::uint32_t> link_start_;
    std::vector<Link> links_;
    std::vector<V3> cent_;
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> grid_;

    static int CellOf(double v) { return int(std::floor(v / kCell)); }
    static std::int64_t CellKey(int x, int z) { return (std::int64_t(x) << 32) ^ std::uint32_t(z); }
    const V3& Vtx(std::uint32_t f, std::uint32_t k) const { return verts_[idx_[faces_[f].first + k]]; }

    void Finish() {
        grid_.clear();
        cent_.assign(faces_.size(), {});
        for (std::uint32_t f = 0; f < faces_.size(); ++f) {
            float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
            V3 c;
            for (std::uint32_t k = 0; k < faces_[f].count; ++k) {
                const V3& v = Vtx(f, k);
                x0 = std::min(x0, v.x), x1 = std::max(x1, v.x), z0 = std::min(z0, v.z), z1 = std::max(z1, v.z);
                c.x += v.x, c.y += v.y, c.z += v.z;
            }
            const float n = float(std::max<std::uint32_t>(1, faces_[f].count));
            cent_[f] = {c.x / n, c.y / n, c.z / n};
            for (int cx = CellOf(x0); cx <= CellOf(x1); ++cx)
                for (int cz = CellOf(z0); cz <= CellOf(z1); ++cz) grid_[CellKey(cx, cz)].push_back(f);
        }
    }

    template <class F>
    void ForCell(double x, double z, F fn) const {
        auto it = grid_.find(CellKey(CellOf(x), CellOf(z)));
        if (it == grid_.end()) return;
        for (std::uint32_t f : it->second) fn(f);
    }

    double Area(std::uint32_t f) const {
        double a = 0;
        for (std::uint32_t k = 0; k < faces_[f].count; ++k) {
            const V3& p = Vtx(f, k);
            const V3& q = Vtx(f, (k + 1) % faces_[f].count);
            a += double(p.x) * q.z - double(q.x) * p.z;
        }
        return std::fabs(a) / 2;
    }

    bool Contains(std::uint32_t f, double x, double z, double* y) const {
        const V3& a = Vtx(f, 0);
        for (std::uint32_t k = 1; k + 1 < faces_[f].count; ++k) {
            const V3& b = Vtx(f, k);
            const V3& c = Vtx(f, k + 1);
            const double d = (double(b.z) - c.z) * (double(a.x) - c.x) + (double(c.x) - b.x) * (double(a.z) - c.z);
            if (std::fabs(d) < 1e-9) continue;
            const double l1 = ((double(b.z) - c.z) * (x - c.x) + (double(c.x) - b.x) * (z - c.z)) / d;
            const double l2 = ((double(c.z) - a.z) * (x - c.x) + (double(a.x) - c.x) * (z - c.z)) / d;
            const double l3 = 1 - l1 - l2;
            if (l1 >= -1e-6 && l2 >= -1e-6 && l3 >= -1e-6) {
                *y = l1 * a.y + l2 * b.y + l3 * c.y;
                return true;
            }
        }
        return false;
    }

    double ClosestOnFace(std::uint32_t f, const V3& p, V3* q) const {
        double y;
        if (Contains(f, p.x, p.z, &y)) {
            *q = {p.x, float(y), p.z};
            return std::fabs(y - p.y);
        }
        double bd = 1e30;
        for (std::uint32_t k = 0; k < faces_[f].count; ++k) {
            const V3& a = Vtx(f, k);
            const V3& b = Vtx(f, (k + 1) % faces_[f].count);
            const double ex = double(b.x) - a.x, ez = double(b.z) - a.z, l2 = ex * ex + ez * ez;
            double t = l2 < 1e-12 ? 0 : ((p.x - a.x) * ex + (p.z - a.z) * ez) / l2;
            t = std::clamp(t, 0.0, 1.0);
            const V3 c{float(a.x + ex * t), float(a.y + (double(b.y) - a.y) * t), float(a.z + ez * t)};
            const double d = Dist3(p, c);
            if (d < bd) {
                bd = d;
                *q = c;
            }
        }
        return bd;
    }

    bool Astar(int s, int g, const V3& sp, const V3& gp, std::vector<int>* out) const {
        if (s == g) {
            out->assign(1, s);
            return true;
        }
        using QE = std::pair<double, int>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
        std::unordered_map<int, double> dist;
        std::unordered_map<int, int> prev;
        std::unordered_set<int> closed;
        dist[s] = 0;
        pq.push({Dist3(sp, gp), s});
        std::size_t pops = 0;
        while (!pq.empty()) {
            const int u = pq.top().second;
            pq.pop();
            if (u == g) break;
            const double du = dist[u];
            if (!closed.insert(u).second) continue;  // stale entry
            if (++pops > 4000000) return false;
            for (std::uint32_t i = link_start_[std::size_t(u)]; i < link_start_[std::size_t(u) + 1]; ++i) {
                const int v = int(links_[i].face);
                const V3 mid{(links_[i].a.x + links_[i].b.x) / 2, (links_[i].a.y + links_[i].b.y) / 2,
                             (links_[i].a.z + links_[i].b.z) / 2};
                const double nd = du + Dist3(u == s ? sp : cent_[std::size_t(u)], mid) + Dist3(mid, cent_[std::size_t(v)]);
                auto it = dist.find(v);
                if (it == dist.end() || nd < it->second) {
                    dist[v] = nd;
                    prev[v] = u;
                    pq.push({nd + Dist3(cent_[std::size_t(v)], gp), v});
                }
            }
        }
        if (!dist.count(g)) return false;
        std::vector<int> rev{g};
        while (rev.back() != s) rev.push_back(prev[rev.back()]);
        out->assign(rev.rbegin(), rev.rend());
        return true;
    }

    static double Area2(const V3& a, const V3& b, const V3& c) {
        return (double(b.x) - a.x) * (double(c.z) - a.z) - (double(c.x) - a.x) * (double(b.z) - a.z);
    }

    /// String pulling (simple stupid funnel) over the corridor's portals, in the XZ plane.
    std::vector<V3> Funnel(const V3& s, const V3& g, const std::vector<int>& cor) const {
        struct Portal {
            V3 l, r;
        };
        std::vector<Portal> pt;
        pt.push_back({s, s});
        for (std::size_t i = 0; i + 1 < cor.size(); ++i) {
            const Link* lk = nullptr;
            for (std::uint32_t j = link_start_[std::size_t(cor[i])]; j < link_start_[std::size_t(cor[i]) + 1]; ++j)
                if (int(links_[j].face) == cor[i + 1]) {
                    lk = &links_[j];
                    break;
                }
            if (!lk) continue;
            V3 a = lk->a, b = lk->b;
            // pull the portal's ends in by the agent radius
            const double len = Dist2(a, b);
            const double m = std::min(agent_radius, len / 2);
            if (len > 1e-4 && m > 0) {
                const double t = m / len;
                const V3 a2{float(a.x + (b.x - a.x) * t), float(a.y + (b.y - a.y) * t), float(a.z + (b.z - a.z) * t)};
                const V3 b2{float(b.x + (a.x - b.x) * t), float(b.y + (a.y - b.y) * t), float(b.z + (a.z - b.z) * t)};
                a = a2, b = b2;
            }
            // left / right as seen when leaving this face through the portal (from its centroid to the portal's middle)
            const V3& c0 = cent_[std::size_t(cor[i])];
            const V3 mid{(a.x + b.x) / 2, (a.y + b.y) / 2, (a.z + b.z) / 2};
            if (Area2(c0, mid, a) < Area2(c0, mid, b)) std::swap(a, b);  // a = left (counter-clockwise side)
            pt.push_back({a, b});
        }
        pt.push_back({g, g});

        std::vector<V3> path{s};
        V3 apex = s, left = s, right = s;
        std::size_t ai = 0, li = 0, ri = 0;
        for (std::size_t i = 1; i < pt.size(); ++i) {
            const V3& nl = pt[i].l;
            const V3& nr = pt[i].r;
            // right side
            if (Area2(apex, right, nr) >= 0) {
                if (Dist2(apex, right) < 1e-6 || Area2(apex, left, nr) < 0) {
                    right = nr;
                    ri = i;
                } else {
                    path.push_back(left);
                    apex = left;
                    ai = li;
                    left = apex, right = apex;
                    li = ri = ai;
                    i = ai;
                    continue;
                }
            }
            // left side
            if (Area2(apex, left, nl) <= 0) {
                if (Dist2(apex, left) < 1e-6 || Area2(apex, right, nl) > 0) {
                    left = nl;
                    li = i;
                } else {
                    path.push_back(right);
                    apex = right;
                    ai = ri;
                    left = apex, right = apex;
                    li = ri = ai;
                    i = ai;
                    continue;
                }
            }
        }
        if (Dist3(path.back(), g) > 1e-4) path.push_back(g);
        return path;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Steering

/// Stick (0..255 per axis, 128 = centre) for a stick angle (0 = forward, + = right) and tilt 0..1.
struct Stick {
    int lx = 128, ly = 128;
};
inline Stick MakeStick(double angle, double tilt) {
    tilt = std::clamp(tilt, 0.0, 1.0);
    Stick s;
    s.lx = std::clamp(128 + int(std::lround(127 * tilt * std::sin(angle))), 0, 255);
    s.ly = std::clamp(128 - int(std::lround(127 * tilt * std::cos(angle))), 0, 255);
    return s;
}

/// World direction yaw (atan2(dx, dz)) the chr moves in for a stick angle: the camera yaw plus the stick angle.
/// The camera yaw is the direction the camera looks (player - camera position).
struct CamModel {
    // The stick is relative to the game's input camera. The render camera's matrix gives its yaw `raw` (and follows
    // every swing of the auto-following camera frame by frame), but after a ride, a warp or a cut the input camera
    // may differ from it by a while-constant amount (seen: up to 180 deg for seconds). So off = raw + resid, and
    // resid is trimmed from the observed motion (low-pass, any size): the matrix supplies the swings, the motion the
    // offset. With no camera at all, off is the observed offset alone.
    double off = 0;       // current offset (world yaw of stick-forward)
    bool have = false;    // an offset is known
    bool from_cam = false;
    bool mismatch = false;  // |resid| is large: the matrix and the motion disagree (a log flag, the walker keeps steering)
    double raw = 0;
    double resid = 0;
    double obs_off = 0;   // low-pass of the observed offset (fallback with no camera)
    int obs_n = 0, bad = 0, agree = 0, recovered = 0;

    /// The camera's view direction (render camera block: the matrix row at +0x30 of the block at config +0x50).
    void FromForward(double fx, double fz) {
        if (std::hypot(fx, fz) < 0.2) return;
        raw = std::atan2(fx, fz);
        off = Wrap(raw + resid);
        have = from_cam = true;
    }
    /// Fallback model: the camera orbits the player, so the view direction is player - camera.
    void FromCamera(const V3& cam, const V3& player) {
        if (Dist2(cam, player) < 0.3) return;
        FromForward(double(player.x) - cam.x, double(player.z) - cam.z);
    }
    /// A step of observed motion under a known stick angle (speed above ~1.5 m/s, stick tilt above 0.7).
    void Observe(double stick_angle, double vel_yaw, double speed = 4.0) {
        (void)speed;
        const double o = Wrap(vel_yaw - stick_angle);
        obs_off = obs_n == 0 ? o : Wrap(obs_off + 0.25 * Wrap(o - obs_off));
        ++obs_n;
        if (from_cam) {
            resid = Wrap(resid + 0.3 * Wrap(Wrap(o - raw) - resid));
            off = Wrap(raw + resid);
            const bool big = std::fabs(resid) > 0.45;
            if (big && !mismatch) mismatch = true;
            if (!big && mismatch) {
                mismatch = false;
                ++recovered;
            }
        } else if (obs_n >= 2) {
            off = obs_off;
            have = true;
        }
    }
    double StickAngle(double want_yaw) const { return Wrap(want_yaw - off); }
};

/// The walked polyline: progress by projection, lookahead point, remaining arc length.
class PathFollower {
public:
    void Set(std::vector<V3> p) {
        pts_ = std::move(p);
        total_ = 0;
        cum_.assign(pts_.size(), 0.0);
        for (std::size_t i = 1; i < pts_.size(); ++i) {
            cum_[i] = cum_[i - 1] + Dist3(pts_[i - 1], pts_[i]);
        }
        total_ = pts_.empty() ? 0 : cum_.back();
        progress_ = 0;
    }
    bool Empty() const { return pts_.size() < 1; }
    const std::vector<V3>& Points() const { return pts_; }
    double Total() const { return total_; }
    double Progress() const { return progress_; }
    double ArcAt(std::size_t i) const { return i < cum_.size() ? cum_[i] : total_; }
    double Remaining() const { return total_ - progress_; }
    double Cross() const { return cross_; }

    /// Projects p onto the path, only moving forward by at most `window` m of arc (and backward by 2 m).
    void Update(const V3& p, double window = 6.0) {
        if (pts_.size() < 2) {
            cross_ = pts_.empty() ? 0 : Dist2(p, pts_[0]);
            return;
        }
        double best = 1e30, best_arc = progress_;
        for (std::size_t i = 0; i + 1 < pts_.size(); ++i) {
            if (cum_[i + 1] < progress_ - 2.0) continue;
            if (cum_[i] > progress_ + window) break;
            const double ex = double(pts_[i + 1].x) - pts_[i].x, ez = double(pts_[i + 1].z) - pts_[i].z;
            const double l2 = ex * ex + ez * ez;
            double t = l2 < 1e-12 ? 0 : ((p.x - pts_[i].x) * ex + (p.z - pts_[i].z) * ez) / l2;
            t = std::clamp(t, 0.0, 1.0);
            const double qx = pts_[i].x + ex * t, qz = pts_[i].z + ez * t;
            const double d = std::hypot(p.x - qx, p.z - qz);
            const double arc = cum_[i] + (cum_[i + 1] - cum_[i]) * t;
            if (d < best - 1e-9 || (std::fabs(d - best) < 0.25 && arc > best_arc)) {
                best = d;
                best_arc = arc;
            }
        }
        cross_ = best;
        progress_ = std::max(progress_, std::min(best_arc, progress_ + window));  // never goes back
    }
    /// Point at arc length `s` along the path.
    V3 At(double s) const {
        if (pts_.empty()) return {};
        if (s <= 0) return pts_.front();
        if (s >= total_) return pts_.back();
        std::size_t i = 0;
        while (i + 2 < pts_.size() && cum_[i + 1] < s) ++i;
        const double l = cum_[i + 1] - cum_[i];
        const double t = l < 1e-9 ? 0 : (s - cum_[i]) / l;
        return {float(pts_[i].x + (pts_[i + 1].x - pts_[i].x) * t), float(pts_[i].y + (pts_[i + 1].y - pts_[i].y) * t),
                float(pts_[i].z + (pts_[i + 1].z - pts_[i].z) * t)};
    }
    V3 Lookahead(double dist) const { return At(progress_ + dist); }

private:
    std::vector<V3> pts_;
    std::vector<double> cum_;
    double total_ = 0, progress_ = 0, cross_ = 0;
};

/// No progress along the path for `limit` s -> stalled. Progress = new maximum arc length by `eps` m.
struct StallDetector {
    double limit = 1.5, eps = 0.4;
    double best = -1e9, since = 0;
    bool started = false;
    void Reset(double now, double progress) {
        best = progress, since = now, started = true;
    }
    bool Update(double now, double progress) {
        if (!started) Reset(now, progress);
        if (progress > best + eps) {
            best = progress;
            since = now;
        }
        return now - since >= limit;
    }
};

struct WalkConfig {
    double lookahead = 1.8;       // m of path ahead the stick aims at
    double lookahead_min = 0.9;
    double max_turn_rate = 4.5;   // rad/s the commanded world direction may turn
    double slow_dist = 4.0;       // ease off inside this remaining distance
    double min_tilt = 0.45;       // never below this while walking (a walk, not a creep)
    double replan_cross = 2.5;    // m off the corridor -> replan
    double stall_s = 1.5;
    double warp_max = 8.0;        // longest last-resort warp along the path (m)
    int max_warps = 3;
    int max_replans = 12;
    double timeout_s = 240;
};

struct WalkStats {
    int replans = 0, backups = 0, strafes = 0, rolls = 0, warps = 0, stalls = 0;
    double started = 0, ended = 0;
    double snap_goal = 0;
    double path_len = 0;
};

enum class WalkState { Idle, Walking, Backing, Strafing, Rolling, Done, Failed };

struct WalkOut {
    Stick stick;
    bool roll = false;       // tap Circle this tick
    bool warp = false;       // last resort: chr warp onto warp_to (navmesh point of the path, y + margin)
    V3 warp_to;
    WalkState state = WalkState::Idle;
    const char* event = nullptr;  // a one-line event to log (nullptr = none)
};

class Walker {
public:
    WalkConfig cfg;
    WalkStats stats;
    std::string fail_reason;

    /// goals: waypoints to pass in order (the last one is the goal); radius: arrival distance to the last goal.
    PlanResult Start(const NavMesh* mesh, const V3& from, std::vector<V3> goals, std::vector<bool> goal_y_known,
                     double radius, double now, bool allow_tail = false, bool tail_far = false) {
        mesh_ = mesh;
        allow_tail_ = allow_tail;
        tail_far_ = tail_far;
        goals_ = std::move(goals);
        goal_y_known_ = std::move(goal_y_known);
        goal_y_known_.resize(goals_.size(), true);
        radius_ = radius;
        stats = {};
        stats.started = now;
        fail_reason.clear();
        state_ = WalkState::Walking;
        dir_valid_ = false;
        recover_stage_ = 0;
        warps_ = 0;
        next_goal_ = 0;
        last_now_ = -1;
        paused_ = 0;
        const PlanResult r = PlanAll(from, 0);
        if (r != PlanResult::Ok) {
            state_ = WalkState::Failed;
            fail_reason = PlanName(r);
            return r;
        }
        stall_.limit = cfg.stall_s;
        stall_.Reset(now, 0);
        have_last_ = false;
        return r;
    }

    WalkState state() const { return state_; }
    /// No stall verdicts until `until` (walk clock): the player is held at a map seam.
    void HoldStall(double until) { hold_until_ = std::max(hold_until_, until); }
    const PathFollower& path() const { return path_; }
    CamModel& cam() { return cam_; }
    double speed() const { return speed_; }
    const V3& last_target() const { return last_target_; }

    WalkOut Tick(const V3& pos, double now, double dt) {
        WalkOut o;
        o.state = state_;
        if (state_ == WalkState::Done || state_ == WalkState::Failed || state_ == WalkState::Idle) return o;
        if (now - stats.started > cfg.timeout_s + paused_) return Fail(o, now, "timeout");
        // not ticked for a while (a load, a cutscene): no stall or speed from the gap
        if (last_now_ >= 0 && now - last_now_ > 0.5) {
            paused_ += now - last_now_;
            have_last_ = false;
            speed_ = 0;
            stall_.Reset(now, path_.Progress());
            dir_valid_ = false;
            o.event = "resumed after a pause";
        }
        last_now_ = now;
        // velocity (m/s) from the last two samples
        if (have_last_ && dt > 1e-3) {
            const double vx = (pos.x - last_pos_.x) / dt, vz = (pos.z - last_pos_.z) / dt;
            speed_ = std::hypot(vx, vz);
            if (speed_ > 1.5 && state_ == WalkState::Walking && last_tilt_ > 0.7 && !stalled_recent_) {
                cam_.Observe(last_stick_angle_, std::atan2(vx, vz), speed_);
            }
            vel_yaw_ = std::atan2(vx, vz);
        }
        last_pos_ = pos;
        have_last_ = true;
        path_.Update(pos);
        const V3& goal = goals_.back();
        const double to_goal = Dist2(pos, goal);
        // arrival: inside the radius of the real goal, or at the end of a path that ends where the goal snaps
        const bool end_reached = path_.Remaining() < 0.25;
        if (to_goal <= radius_ || (end_reached && to_goal <= radius_ + stats.snap_goal + 0.25)) {
            state_ = WalkState::Done;
            stats.ended = now;
            o.state = state_;
            o.event = "arrived";
            return o;
        }
        // off the corridor
        if (path_.Cross() > cfg.replan_cross) {
            if (!Replan(pos, o, now, "off the corridor")) return o;
        }
        // a map seam (the director saw the stage record change): the game holds the player there while the next map block
        // streams in; a player keeps the stick forward and waits, so no stall (and no back-up that undoes the crossing)
        if (now < hold_until_ && state_ == WalkState::Walking) stall_.Reset(now, path_.Progress());
        const bool stalled = stall_.Update(now, path_.Progress());
        if (state_ == WalkState::Walking && stalled) {
            ++stats.stalls;
            stalled_recent_ = true;
            WalkOut r = Recover(pos, now, o);
            stage_progress_ = path_.Progress();
            return r;
        }
        if (recover_stage_ > 0 && path_.Progress() > stage_progress_ + 2.0) recover_stage_ = 0;
        if (state_ != WalkState::Walking) {
            return RecoverStep(pos, now, o);
        }
        if (!stalled) stalled_recent_ = stalled_recent_ && now - recover_end_ < 1.0;
        // pure pursuit
        const double rem = path_.Remaining();
        const double look = std::clamp(cfg.lookahead * std::min(1.0, 0.4 + speed_ / 6.0), cfg.lookahead_min, cfg.lookahead);
        // the aim point may not cut a corner across something that is not walkable: shorten the lookahead until the
        // straight run to it lies on the mesh (a hairpin round a parapet, a door frame)
        V3 tgt = path_.Lookahead(std::min(look, rem));
        for (double f : {1.0, 0.65, 0.4, 0.25}) {
            tgt = path_.Lookahead(std::min(look * f, rem));
            if (mesh_->Walkable(pos, tgt, 0.3)) break;
        }
        last_target_ = tgt;
        if (Dist2(pos, tgt) < 0.2) tgt = goal;
        const double want = std::atan2(double(tgt.x) - pos.x, double(tgt.z) - pos.z);
        double cmd = want;
        if (dir_valid_) {
            const double d = Wrap(want - dir_);
            const double lim = cfg.max_turn_rate * std::max(dt, 0.0);
            cmd = Wrap(dir_ + std::clamp(d, -lim, lim));
        }
        dir_ = cmd;
        dir_valid_ = true;
        double tilt = 1.0;
        if (to_goal < cfg.slow_dist) tilt = std::clamp(to_goal / cfg.slow_dist, cfg.min_tilt, 1.0);
        // do not run flat out at a sharp corner: ease with the turn the lookahead still needs
        const double turn = std::fabs(Wrap(want - dir_));
        if (turn > 0.8) tilt = std::min(tilt, 0.6);
        return Emit(o, cam_.StickAngle(cmd), tilt);
    }

private:
    const NavMesh* mesh_ = nullptr;
    std::vector<V3> goals_;
    std::vector<bool> goal_y_known_;
    double radius_ = 1.0;
    bool allow_tail_ = false;  // a goal a little off the mesh (a lift car's trigger) is walked straight to (<= 2.5 m)
    bool tail_far_ = false;    // routes: a goal up to 10 m off the mesh (a lift car, a ladder top) too
    PathFollower path_;
    CamModel cam_;
    StallDetector stall_;
    WalkState state_ = WalkState::Idle;
    V3 last_pos_;
    bool have_last_ = false;
    double speed_ = 0, vel_yaw_ = 0, dir_ = 0, last_stick_angle_ = 0, last_tilt_ = 0;
    bool dir_valid_ = false, stalled_recent_ = false;
    int recover_stage_ = 0, warps_ = 0, side_ = 1;
    double stage_until_ = 0, recover_end_ = 0, roll_until_ = 0;
    std::size_t next_goal_ = 0;
    double last_now_ = -1, paused_ = 0;
    V3 last_target_;
    std::vector<std::size_t> goal_idx_;
    std::vector<double> goal_arc_;
    std::size_t goal_from_ = 0;

    WalkOut& Emit(WalkOut& o, double stick_angle, double tilt) {
        o.stick = MakeStick(stick_angle, tilt);
        last_stick_angle_ = stick_angle;
        last_tilt_ = tilt;
        o.state = state_;
        return o;
    }

    WalkOut& Fail(WalkOut& o, double now, const char* why) {
        state_ = WalkState::Failed;
        fail_reason = why;
        stats.ended = now;
        o.state = state_;
        o.event = why;
        o.stick = {};
        return o;
    }

    // Plans pos -> goals[from_goal..]; goals off the mesh before the last are dropped (hints only).
    PlanResult PlanAll(const V3& pos, std::size_t from_goal) {
        std::vector<V3> path;
        V3 cur = pos;
        PlanResult last = PlanResult::Ok;
        goal_idx_.assign(goals_.size(), 0);
        for (std::size_t i = from_goal; i < goals_.size(); ++i) {
            if (i > from_goal) goal_idx_[i - 1] = path.empty() ? 0 : path.size() - 1;
            PlanInfo pi;
            std::vector<V3> leg;
            const PlanResult r = mesh_->Plan(cur, goals_[i], &leg, &pi, 2.5, 4.0, goal_y_known_[i]);
            V3 g = goals_[i];
            if (!goal_y_known_[i]) g.y = cur.y;
            if (r != PlanResult::Ok) {
                // a route's off-mesh goal within 10 m of where the path is: a straight tail
                if (allow_tail_ && tail_far_ && r == PlanResult::GoalOff && Dist3(cur, g) <= 10.0) {
                    if (path.empty()) path.push_back(cur);
                    path.push_back(g);
                    cur = g;
                    if (i + 1 == goals_.size()) stats.snap_goal = 0;
                    continue;
                }
                if (i + 1 == goals_.size()) return r;
                last = r;
                continue;  // a hint waypoint off the mesh: skip it
            }
            if (i + 1 == goals_.size()) {
                stats.snap_goal = pi.snap_goal;
            }
            leg = mesh_->Smooth(leg);  // a route's waypoints are known-good spots: only inside a leg, never across one
            if (!path.empty() && !leg.empty()) leg.erase(leg.begin());
            path.insert(path.end(), leg.begin(), leg.end());
            cur = leg.empty() ? cur : leg.back();
            if (allow_tail_ && pi.snap_goal > 0.4 && pi.snap_goal <= (tail_far_ ? 10.0 : 2.5) && Dist3(cur, g) <= 10.0) {  // the goal is a little off the mesh: walk on to it
                path.push_back(g);
                cur = g;
                if (i + 1 == goals_.size()) stats.snap_goal = 0;
            }
        }
        (void)last;
        if (path.empty()) return PlanResult::NoRoute;
        goal_idx_[goals_.size() - 1] = path.size() - 1;
        goal_from_ = from_goal;
        path_.Set(goals_.size() - from_goal > 1 ? std::move(path) : mesh_->Smooth(path));
        stats.path_len = path_.Total();
        goal_arc_.assign(goals_.size(), 0.0);
        for (std::size_t i = from_goal; i < goals_.size(); ++i) goal_arc_[i] = path_.ArcAt(goal_idx_[i]);
        return PlanResult::Ok;
    }

    bool Replan(const V3& pos, WalkOut& o, double now, const char* why) {
        if (++stats.replans > cfg.max_replans) {
            Fail(o, now, "too many replans");
            return false;
        }
        const PlanResult r = PlanAll(pos, PassedGoals(pos));
        if (r != PlanResult::Ok) {
            Fail(o, now, PlanName(r));
            return false;
        }
        stall_.Reset(now, 0);
        o.event = why;
        return true;
    }

    // Goals already passed: every waypoint closer than 2.5 m along the walk is behind us.
    std::size_t PassedGoals(const V3& pos) {
        (void)pos;
        // a goal is passed when the walk's progress along the path is beyond it (not only when the player is near it)
        if (next_goal_ < goal_from_) next_goal_ = goal_from_;
        while (next_goal_ + 1 < goals_.size() && next_goal_ < goal_arc_.size() && goal_arc_[next_goal_] < path_.Progress() + 0.5) ++next_goal_;
        return next_goal_;
    }

    WalkOut Recover(const V3& pos, double now, WalkOut o) {
        ++recover_stage_;
        // 1: replan, 2: back up + strafe, 3: roll, 4: back up + strafe the other way, 5: warp, then fail
        if (recover_stage_ == 1) {
            if (Replan(pos, o, now, "stalled: replan")) {
                o.state = state_;
                return Emit(o, cam_.StickAngle(dir_valid_ ? dir_ : 0), 1.0);
            }
            return o;
        }
        if (recover_stage_ == 2 || recover_stage_ == 4) {
            side_ = recover_stage_ == 2 ? 1 : -1;
            state_ = WalkState::Backing;
            stage_until_ = now + 0.55;
            ++stats.backups;
            o.event = "stalled: back up and strafe";
            return RecoverStep(pos, now, o);
        }
        if (recover_stage_ == 3) {
            state_ = WalkState::Rolling;
            roll_until_ = now + 0.15;
            stage_until_ = now + 0.9;
            ++stats.rolls;
            o.event = "stalled: roll";
            return RecoverStep(pos, now, o);
        }
        // last resort
        if (warps_ >= cfg.max_warps) return Fail(o, now, "blocked: no way past");
        const double room = std::min(cfg.warp_max, path_.Remaining());
        if (room < 0.5) return Fail(o, now, "blocked near the goal");
        V3 to = path_.At(path_.Progress() + std::min(room, 4.0));
        to.y += 1.0f;  // navmesh y can sit under the floor: drop in from a metre above
        ++warps_;
        ++stats.warps;
        o.warp = true;
        o.warp_to = to;
        o.event = "stalled: WARP onto the path";
        recover_stage_ = 0;
        stall_.Reset(now, path_.Progress() + std::min(room, 4.0));
        recover_end_ = now;
        return o;
    }

    WalkOut RecoverStep(const V3& pos, double now, WalkOut o) {
        (void)pos;
        const double base = dir_valid_ ? dir_ : 0.0;
        if (state_ == WalkState::Backing) {
            if (now < stage_until_) {
                // first half back, second half sideways
                const bool back = now < stage_until_ - 0.3;
                const double a = back ? Wrap(base + kPi) : Wrap(base + side_ * kPi / 2);
                if (!back && !stage_flag_) {
                    ++stats.strafes;
                    stage_flag_ = true;
                }
                return Emit(o, cam_.StickAngle(a), 1.0);
            }
            return EndRecovery(now, o);
        }
        if (state_ == WalkState::Rolling) {
            if (now < stage_until_) {
                o.roll = now < roll_until_;
                return Emit(o, cam_.StickAngle(base), 1.0);
            }
            return EndRecovery(now, o);
        }
        return o;
    }

    WalkOut EndRecovery(double now, WalkOut o) {
        state_ = WalkState::Walking;
        stage_flag_ = false;
        recover_end_ = now;
        stall_.Reset(now, path_.Progress());
        o.state = state_;
        return o;
    }
    bool stage_flag_ = false;
    double stage_progress_ = 0;
    double hold_until_ = -1;
};

}  // namespace navwalk
