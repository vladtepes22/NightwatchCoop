// SPDX-License-Identifier: GPL-3.0-or-later
// guest_stick=X:Z[:R[:STEPS]] (party_director test step): a real left-stick walk of the guest to
// (X, Z) within R m (1.0), giving up after STEPS stick steps (40); or guest_stick=obj:ENTITY:DIST:ANGLE
// [:R[:STEPS]] to the point DIST m from a map object. host_route= / guest_route=X:Y:Z/...: a walk
// along waypoints. Header-only for the unit test.
#pragma once

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

inline bool ParseStickArgs(const std::string& args, float* x, float* z, float* r, int* steps) {
    float px = 0, pz = 0, pr = 1.0f;
    int ps = 40;
    char tail = 0;
    const int n = std::sscanf(args.c_str(), "%f:%f:%f:%d%c", &px, &pz, &pr, &ps, &tail);
    if (n < 2 || n > 4 || pr <= 0.0f || ps < 1 || ps > 2000) {
        return false;
    }
    *x = px;
    *z = pz;
    *r = pr;
    *steps = ps;
    return true;
}

inline bool ParseStickObjArgs(const std::string& args, int* entity, float* dist, float* angle, float* r, int* steps) {
    if (args.rfind("obj:", 0) != 0) {
        return false;
    }
    int e = 0, ps = 40;
    float d = 0, a = 0, pr = 1.0f;
    char tail = 0;
    const int n = std::sscanf(args.c_str() + 4, "%d:%f:%f:%f:%d%c", &e, &d, &a, &pr, &ps, &tail);
    if (n < 3 || n > 5 || e <= 0 || pr <= 0.0f || ps < 1 || ps > 2000) {
        return false;
    }
    *entity = e;
    *dist = d;
    *angle = a;
    *r = pr;
    *steps = ps;
    return true;
}

/// host_route= / guest_route=X:Z/X:Z/... or X:Y:Z/X:Y:Z/...: the waypoints {x, y, z} of a stick walk
/// along a real path (e.g. a navmesh route: tools/party/nav_route.py); y = NaN when not given (no
/// hop to that waypoint possible). At least one, at most 2000.
inline bool ParseRouteArgs(const std::string& args, std::vector<std::array<float, 3>>* out) {
    std::vector<std::array<float, 3>> pts;
    std::size_t at = 0;
    while (at <= args.size()) {
        const std::size_t end = args.find('/', at);
        const std::string one = args.substr(at, end == std::string::npos ? std::string::npos : end - at);
        float a = 0, b = 0, c = 0;
        char tail = 0;
        const int n = std::sscanf(one.c_str(), "%f:%f:%f%c", &a, &b, &c, &tail);
        if (n < 2 || n > 3 || pts.size() >= 2000) {
            return false;
        }
        pts.push_back(n == 3 ? std::array<float, 3>{a, b, c} : std::array<float, 3>{a, std::nanf(""), b});
        if (end == std::string::npos) {
            break;
        }
        at = end + 1;
    }
    if (pts.empty()) {
        return false;
    }
    *out = std::move(pts);
    return true;
}
