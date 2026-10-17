#!/usr/bin/env python3
"""Walkable routes from the game's own navmeshes, for the B1 route walker (party_director test steps
host_route= / guest_route=, docs/party/travel.md 7.2).

Game data (read-only, untrusted; run with python -I). GAME = .../dvdroot_ps4.

  python -I tools/party/nav_route.py GAME m24_01_00_00,m24_00_00_00 [--step 5] -- FROM TO
      FROM / TO = X:Y:Z (map coordinates, e.g. a lamp's spot from ids_tool's return_spots). Prints the
      route (one line per waypoint with its map) and "ROUTE X:Y:Z/X:Y:Z/..." for the test step.
  python -I tools/party/nav_route.py GAME m24_01_00_00,m24_00_00_00 --borders
      Where the navmeshes of two maps (streamed blocks) meet: the places a walk changes block.

Each map's map/mAA_BB_CC_DD/mAA_BB_CC_DD.nvmhktbnd.dcx is a BND4 of Havok 2014 packfiles; each holds
one hkaiNavMesh (+0x10 faces {i32 startEdge, i32 startUserEdge, i16 numEdges, ...} of 16 bytes,
+0x20 edges {i32 a, i32 b, u32 oppositeEdge, u32 oppositeFace, ...} of 20 bytes, +0x30 vertices
hkVector4), in map (world) coordinates. Faces are linked by their opposite face inside one file;
across files (and maps) by boundary edges that lie on each other (0.5 m cells, 0.6 m in height).
A route is A* over face centroids. The navmesh is the enemies' one: closed gates and doors are not
in it (a route may run into a gate), and carts / enemies on the way block a stick walk (the walker
side-steps, then hops onto the next waypoint).
"""
import argparse, heapq, math, os, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flag_tool as ft  # noqa: E402


def _sections(b):
    nsec = struct.unpack_from("<i", b, 0x14)[0]
    secoff = struct.unpack_from("<h", b, 0x3e)[0]
    hdr = 0x40 + (secoff if secoff > 0 else 0)
    out = {}
    for i in range(nsec):
        o = hdr + i * 0x40
        tag = b[o:o + 19].split(b"\0")[0].decode()
        out[tag] = struct.unpack_from("<7I", b, o + 20)  # abs, local, global, virtual, exports, imports, end
    return out


def navmesh(b):
    """(faces, edges, vertices) of the packfile's hkaiNavMesh, or None."""
    if b[:8] != bytes.fromhex("57e0e05710c0c010"):
        return None
    secs = _sections(b)
    cn, da = secs["__classnames__"], secs["__data__"]
    base = da[0]
    loc = {}
    o = base + da[1]
    while o + 8 <= base + da[2]:
        s, d = struct.unpack_from("<ii", b, o)
        o += 8
        if s != -1:
            loc[s] = d
    o = base + da[3]
    while o + 12 <= base + da[4]:
        s, _sec, name = struct.unpack_from("<iii", b, o)
        o += 12
        if s == -1:
            continue
        if b[cn[0] + name:].split(b"\0")[0] != b"hkaiNavMesh":
            continue

        def arr(at, stride, fmt):
            n = struct.unpack_from("<i", b, base + at + 8)[0]
            if n <= 0 or at not in loc:
                return []
            return [struct.unpack_from(fmt, b, base + loc[at] + i * stride) for i in range(n)]

        return arr(s + 0x10, 16, "<iihhhH"), arr(s + 0x20, 20, "<iiIIBBH"), arr(s + 0x30, 16, "<4f")
    return None


def load(game, maps):
    """faces [(map, [(x, y, z)...])], adjacency {face: set(face)}."""
    faces, adj, border = [], {}, {}
    for m in maps:
        files = ft.bnd4(ft.read_maybe_dcx(Path(game) / "map" / m / f"{m}.nvmhktbnd.dcx"))
        for name, b in files.items():
            r = navmesh(b)
            if not r:
                continue
            fs, es, vs = r
            base = len(faces)
            for fi, f in enumerate(fs):
                faces.append((m, [vs[es[f[0] + k][0]][:3] for k in range(f[2])]))
                links = adj.setdefault(base + fi, set())
                for k in range(f[2]):
                    e = es[f[0] + k]
                    if e[3] != 0xFFFFFFFF and e[3] < len(fs):
                        links.add(base + e[3])
                        continue
                    a, c = vs[e[0]], vs[e[1]]
                    n = max(1, int(math.dist(a[:3], c[:3]) / 0.25))
                    for t in range(n + 1):
                        q = [a[j] + (c[j] - a[j]) * t / n for j in range(3)]
                        border.setdefault((round(q[0] * 2), round(q[2] * 2)), []).append((base + fi, q[1], name))
    for lst in border.values():
        for i in range(len(lst)):
            for j in range(i + 1, len(lst)):
                if lst[i][2] != lst[j][2] and abs(lst[i][1] - lst[j][1]) < 0.6:
                    adj[lst[i][0]].add(lst[j][0])
                    adj[lst[j][0]].add(lst[i][0])
    return faces, adj


def centroid(poly):
    return tuple(sum(v[i] for v in poly) / len(poly) for i in range(3))


def nearest(cents, p):
    return min(range(len(cents)), key=lambda i: (cents[i][0] - p[0]) ** 2 + 4 * (cents[i][1] - p[1]) ** 2 +
               (cents[i][2] - p[2]) ** 2)


def astar(cents, adj, s, g):
    dist, prev, pq = {s: 0.0}, {}, [(0.0, s)]
    while pq:
        _, u = heapq.heappop(pq)
        if u == g:
            break
        for v in adj.get(u, ()):
            nd = dist[u] + math.dist(cents[u], cents[v])
            if nd < dist.get(v, 1e18):
                dist[v], prev[v] = nd, u
                heapq.heappush(pq, (nd + math.dist(cents[v], cents[g]), v))
    if g not in dist:
        return None
    path = [g]
    while path[-1] != s:
        path.append(prev[path[-1]])
    return path[::-1]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("maps")
    ap.add_argument("src", nargs="?")
    ap.add_argument("dst", nargs="?")
    ap.add_argument("--step", type=float, default=5.0, help="m of route between waypoints (and at every map change)")
    ap.add_argument("--borders", action="store_true")
    a = ap.parse_args()
    faces, adj = load(a.game, a.maps.split(","))
    cents = [centroid(p) for _, p in faces]
    if a.borders:
        groups = []
        for u, vs in adj.items():
            for v in vs:
                if u < v and faces[u][0] != faces[v][0]:
                    c = cents[u]
                    for g in groups:
                        if abs(g[0][0] - c[0]) < 15 and abs(g[0][2] - c[2]) < 15 and abs(g[0][1] - c[1]) < 5:
                            g[1] += 1
                            break
                    else:
                        groups.append([c, 1, faces[u][0], faces[v][0]])
        for c, n, m1, m2 in groups:
            print("%.1f %.1f %.1f  %s <-> %s  (%d links)" % (c[0], c[1], c[2], m1, m2, n))
        return
    src = tuple(map(float, a.src.split(":")))
    dst = tuple(map(float, a.dst.split(":")))
    path = astar(cents, adj, nearest(cents, src), nearest(cents, dst))
    if not path:
        print("no route")
        sys.exit(1)
    pts = [cents[i] + (faces[i][0],) for i in path]
    out, acc = [pts[0]], 0.0
    for i in range(1, len(pts)):
        acc += math.dist(pts[i][:3], pts[i - 1][:3])
        if acc >= a.step or pts[i][3] != out[-1][3] or i == len(pts) - 1:
            out.append(pts[i])
            acc = 0.0
    total = sum(math.dist(pts[i][:3], pts[i - 1][:3]) for i in range(1, len(pts)))
    print("%d faces, %.1f m, %d waypoints" % (len(path), total, len(out)))
    for p in out:
        print("%.1f %.1f %.1f %s" % p)
    print("ROUTE " + "/".join("%.1f:%.1f:%.1f" % p[:3] for p in out[1:]))


if __name__ == "__main__":
    main()
