#!/usr/bin/env python3
"""Compact per-area navmesh cache for the test walker (gpu/shim/party/nav_walk.h, docs/party/walker.md).

  python -I tools/party/nav_cache.py GAME OUTDIR [AREA ...]      AREA = 24 (maps m24_*), default: all areas

Reads each map's mAA_BB_CC_DD.nvmhktbnd.dcx (game data, read-only, untrusted: Havok hkaiNavMesh packfiles, see
nav_route.py) and writes OUTDIR/nav_mAA.bin: faces (convex polygons), their vertices and the links between
faces with the portal (shared edge) of each link. Links inside one file use the face's opposite edge; links across
files and maps use boundary edges that lie on each other (0.5 m cells, 0.6 m in height); their portal is the
overlap sample point (a degenerate portal).

File (little endian):  "BBNV" u32 version(1) u32 nverts u32 nfaces u32 nlinks u32 nidx
  float[3] * nverts | per face: u32 first, u32 count, u32 map (index of mAA_BB_CC_DD, i.e. BB*... see maps) |
  u32 idx * nidx | per face u32 linkStart (nfaces+1 entries) | per link: u32 face, float[6] portal a,b
  then u32 nmaps, per map: 16 bytes zero padded name.
"""
import collections, math, os, re, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flag_tool as ft  # noqa: E402
import nav_route as nr  # noqa: E402
import ids_tool as it  # noqa: E402


def build(game, maps, offsets=None):
    faces, edges_of, border = [], [], {}   # faces: (map, [vertex idx]); links: face -> {nb: portal}
    verts, vkey = [], {}
    links = []

    def vid(p):
        k = (round(p[0], 3), round(p[1], 3), round(p[2], 3))
        if k not in vkey:
            vkey[k] = len(verts)
            verts.append(k)
        return vkey[k]

    for mi, m in enumerate(maps):
        spec = (offsets or {}).get(m, (0.0, 0.0, 0.0, 0.0))
        p = Path(game) / "map" / m / f"{m}.nvmhktbnd.dcx"
        if not p.exists():
            continue
        files = ft.bnd4(ft.read_maybe_dcx(p))
        for name, b in files.items():
            r = nr.navmesh(b)
            if not r:
                continue
            fs, es, vs = r
            if isinstance(spec, dict) and name in spec.get("drop", ()):
                continue
            off = spec["files"].get(name, spec["default"]) if isinstance(spec, dict) else spec
            ox, oy, oz = off[:3]
            yaw = math.radians(off[3]) if len(off) > 3 else 0.0
            cy, sy = math.cos(yaw), math.sin(yaw)
            # world = origin + Ry(yaw) * local  (x' = x cos + z sin, z' = -x sin + z cos)
            vs = [(ox + v[0] * cy + v[2] * sy, v[1] + oy, oz - v[0] * sy + v[2] * cy, v[3]) for v in vs]
            base = len(faces)
            for fi, f in enumerate(fs):
                faces.append((mi, [vid(vs[es[f[0] + k][0]][:3]) for k in range(f[2])]))
                links.append({})
                for k in range(f[2]):
                    e = es[f[0] + k]
                    a, c = vs[e[0]][:3], vs[e[1]][:3]
                    if e[3] != 0xFFFFFFFF and e[3] < len(fs):
                        links[base + fi][base + e[3]] = (a, c)
                        continue
                    n = max(1, int(math.dist(a, c) / 0.25))
                    for t in range(n + 1):
                        q = [a[j] + (c[j] - a[j]) * t / n for j in range(3)]
                        border.setdefault((round(q[0] * 2), round(q[2] * 2)), []).append((base + fi, q, name))
    pairs = {}  # (face i, face j) -> overlapping boundary samples of i's edge
    for lst in border.values():
        for i in range(len(lst)):
            for j in range(i + 1, len(lst)):
                if lst[i][0] != lst[j][0] and abs(lst[i][1][1] - lst[j][1][1]) < 0.6:
                    pairs.setdefault((lst[i][0], lst[j][0]), []).append(lst[i][1])
                    pairs.setdefault((lst[j][0], lst[i][0]), []).append(lst[j][1])
    for (i, j), qs in pairs.items():
        # the portal is the overlap segment: the two samples farthest apart
        p0 = qs[0]
        p1 = max(qs, key=lambda q: math.dist(q, p0))
        p2 = max(qs, key=lambda q: math.dist(q, p1))
        links[i].setdefault(j, (tuple(p1), tuple(p2)))
    return verts, faces, links


_MSBS = []


def _msbs(game):
    if not _MSBS:
        _MSBS.extend(it.scan_maps(Path(game))[1])
    return _MSBS


def part_offsets(game, m):
    """Candidate map-origin translations of a map: positions of its MSB collision (5) / connect-collision (11) /
    navmesh (8) parts (the Nightmare maps sit at y 1500 etc.: the hkaiNavMesh is in the part's local frame)."""
    out = collections.Counter()
    for (a, b, c, d), path in _msbs(game):
        if path.name.startswith(m):
            data = ft.read_maybe_dcx(path)
            for o in dict(it.msb_lists(data)).get("PARTS_PARAM_ST", []):
                if struct.unpack_from("<i", data, o + 0x14)[0] in (5, 8, 11):
                    v = struct.unpack_from("<6f", data, o + 0x28)
                    yaw = round(v[4], 2)
                    out[(round(v[0], 2), round(v[1], 2), round(v[2], 2), yaw)] += 1
                    if yaw:
                        out[(round(v[0], 2), round(v[1], 2), round(v[2], 2), -yaw)] += 1  # the sign convention is checked by the spots
            break
    return out


def spots_of(m):
    """Lamp / revive spots of a map (world frame) from the generated party_ids.inc."""
    inc = Path(__file__).resolve().parents[2] / "gpu/shim/party/party_ids.inc"
    key = m[1:3] + m[5:6]  # AA + block digit of mAA_0B: the ids are AABnnnn
    pts = []
    for line in inc.read_text(errors="replace").splitlines():
        r = re.match(r"\s+(-?[\d.]+)f, (-?[\d.]+)f, (-?[\d.]+)f, -?[\d.]+f,\s+// (\d{7})$", line)
        if r and r.group(4)[:3] == key:
            pts.append(tuple(float(r.group(i)) for i in (1, 2, 3)))
    return pts


def on_mesh_count(verts, faces, pts, tol=1.5):
    """How many points (x, y, z) lie on a face (XZ inside, |y| within tol)."""
    grid = collections.defaultdict(list)
    for fi, (_mi, vl) in enumerate(faces):
        xs = [verts[v][0] for v in vl]
        zs = [verts[v][2] for v in vl]
        for cx in range(int(min(xs) // 8), int(max(xs) // 8) + 1):
            for cz in range(int(min(zs) // 8), int(max(zs) // 8) + 1):
                grid[(cx, cz)].append(fi)
    n = 0
    for x, y, z in pts:
        hit = False
        for fi in grid.get((int(x // 8), int(z // 8)), ()):
            vl = faces[fi][1]
            a = verts[vl[0]]
            for k in range(1, len(vl) - 1):
                b, c = verts[vl[k]], verts[vl[k + 1]]
                d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
                if abs(d) < 1e-9:
                    continue
                l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d
                l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d
                l3 = 1 - l1 - l2
                if min(l1, l2, l3) >= -1e-6 and abs(l1 * a[1] + l2 * b[1] + l3 * c[1] - y) <= tol:
                    hit = True
                    break
            if hit:
                break
        n += hit
    return n


def piece_hits(game, m, name, b, off, pts):
    """Spots on one navmesh piece placed at off."""
    r = nr.navmesh(b)
    if not r:
        return 0
    fs, es, vs = r
    ox, oy, oz, yaw = off
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    verts = [(ox + v[0] * cy + v[2] * sy, v[1] + oy, oz - v[0] * sy + v[2] * cy) for v in vs]
    faces = [(0, [es[f[0] + k][0] for k in range(f[2])]) for f in fs]
    return on_mesh_count(verts, faces, pts)


def choose_offset(game, m):
    """The placement of map m: {"default": origin+yaw, "files": {piece: origin+yaw}} that puts the most of its lamp spots
    on its navmesh (the local frame when none helps). A piece may have its own placement (m32 is two regions); a
    piece with no spot on it takes the best single placement. Returns (spec, spots on the mesh, spots, candidates)."""
    pts = spots_of(m)
    zero = (0.0, 0.0, 0.0, 0.0)
    if m.startswith("m29"):
        # Chalice dungeons: the layout is assembled from blocks at run time (the MSB holds the template), so no static
        # placement exists; the walker keeps the local frame there and falls back when the start is off the mesh.
        return {"default": zero, "files": {}}, 0, len(pts), 1
    cands = [zero] + [o for o in part_offsets(game, m) if o != zero]
    spec = {"default": zero, "files": {}}
    if not pts or len(cands) == 1:
        return spec, 0, len(pts), len(cands)
    best, bn = cands[0], -1
    for o in cands:
        verts, faces, _links = build(game, [m], {m: o})
        n = on_mesh_count(verts, faces, pts)
        if n > bn:
            best, bn = o, n
    if best == zero and bn < max(2, (len(pts) + 1) // 2) and bn >= 0:
        pass
    spec["default"] = best
    files = ft.bnd4(ft.read_maybe_dcx(Path(game) / "map" / m / f"{m}.nvmhktbnd.dcx"))
    meshes = {name: nr.navmesh(b) for name, b in files.items()}
    meshes = {k: v for k, v in meshes.items() if v}

    def place(vs, off):
        ox, oy, oz, yaw = off
        cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
        return [(ox + v[0] * cy + v[2] * sy, v[1] + oy, oz - v[0] * sy + v[2] * cy) for v in vs]

    def near(v, seen):
        c = cell(v)
        return any((c[0] + dx, c[1], c[2] + dz) in seen for dx in (-1, 0, 1) for dz in (-1, 0, 1))

    def cell(v):
        return (round(v[0] * 2), round(v[1]), round(v[2] * 2))

    # 1. anchors: pieces with a lamp spot on them under some placement take the placement with the most spots
    placed, hits_total = {}, 0
    for name, (fs, es, vs) in meshes.items():
        hits = {o: piece_hits(game, m, name, files[name], o, pts) for o in cands}
        top = max(hits.values())
        if top > 0:
            placed[name] = max(hits, key=lambda o: (hits[o], o == best))
            hits_total += hits[placed[name]]
    if len(set(placed.values())) == 1:  # one placement for the whole map
        only = next(iter(placed.values()))
        spec["default"] = only
        return (spec if hits_total >= max(2, (len(pts) + 1) // 2) else {"default": zero, "files": {}}), min(hits_total, len(pts)), len(pts), len(cands)
    # 2. the other pieces are glued on: the placement whose vertices coincide most with the placed pieces' vertices
    seen = set()
    for name in placed:
        seen.update(cell(v) for v in place(meshes[name][2], placed[name]))
    for _ in range(30):
        moved = False
        for name, (fs, es, vs) in meshes.items():
            if name in placed:
                continue
            score = {o: sum(near(v, seen) for v in place(vs, o)) for o in cands}
            o = max(score, key=score.get)
            if score[o] >= 3 and sorted(score.values())[-1] > sorted(score.values())[-2] if len(score) > 1 else score[o] >= 3:
                placed[name] = o
                seen.update(cell(v) for v in place(vs, o))
                moved = True
        if not moved:
            break
    n = min(hits_total, len(pts))
    if n < max(2, (len(pts) + 1) // 2):
        return {"default": zero, "files": {}}, n, len(pts), len(cands)  # too little evidence: keep the local frame
    count = collections.Counter(placed.values())
    spec["default"] = count.most_common(1)[0][0]
    spec["files"] = {k: v for k, v in placed.items() if v != spec["default"]}
    spec["drop"] = sorted(set(meshes) - set(placed))  # no anchor and no neighbour: not guessed, left out
    return spec, n, len(pts), len(cands)


def write(path, maps, verts, faces, links):
    idx, first = [], []
    for _, vl in faces:
        first.append((len(idx), len(vl)))
        idx += vl
    nl = sum(len(l) for l in links)
    out = bytearray(b"BBNV")
    out += struct.pack("<5I", 1, len(verts), len(faces), nl, len(idx))
    for v in verts:
        out += struct.pack("<3f", *v)
    for (s, c), (mi, _) in zip(first, faces):
        out += struct.pack("<3I", s, c, mi)
    out += struct.pack("<%dI" % len(idx), *idx)
    ls = 0
    for l in links:
        out += struct.pack("<I", ls)
        ls += len(l)
    out += struct.pack("<I", ls)
    for l in links:
        for nb, (a, c) in l.items():
            out += struct.pack("<I6f", nb, *a, *c)
    out += struct.pack("<I", len(maps))
    for m in maps:
        out += m.encode().ljust(16, b"\0")
    tmp = Path(str(path) + ".tmp%d" % os.getpid())
    tmp.write_bytes(bytes(out))
    os.replace(tmp, path)  # atomic: parallel harness starts


# Areas whose maps are walked across a seam: the file of each holds both, so a walk that starts on one side and
# ends on the other (sj: the cave mouth m24_00 -> m27_00) plans over one connected mesh. The m24 side alone ends in a
# dead-end neck at the seam; the passage itself runs on the m27 pieces.
JOIN = {"24": ["27"], "27": ["24"]}


def main():
    game, outdir, areas = sys.argv[1], Path(sys.argv[2]), sys.argv[3:]
    allmaps = sorted(p.name for p in (Path(game) / "map").iterdir()
                     if p.is_dir() and len(p.name) == 12 and (p / f"{p.name}.nvmhktbnd.dcx").exists())
    if not areas:
        areas = sorted({m[1:3] for m in allmaps})
    outdir.mkdir(parents=True, exist_ok=True)
    for a in areas:
        maps = [m for m in allmaps if m[1:3] == a or m[1:3] in JOIN.get(a, ())]
        if not maps:
            continue
        offsets = {}
        for m in maps:
            o, n, tot, nc = choose_offset(game, m)
            offsets[m] = o
            if o["default"] != (0.0, 0.0, 0.0, 0.0) or o["files"]:
                print("  %s: placement %s + %d piece(s) placed apart, %d left out (%d of %d spots on the mesh, %d candidate(s))" % (m, o["default"], len(o["files"]), len(o.get("drop", ())), n, tot, nc))
        verts, faces, links = build(game, maps, offsets)
        write(outdir / f"nav_m{a}.bin", maps, verts, faces, links)
        print("area m%s: %d maps, %d faces, %d verts" % (a, len(maps), len(faces), len(verts)))


if __name__ == "__main__":
    main()
