# Test walker over the game's navmesh (walk078, 0.7.8)

The smoke harness walks the players with the real left stick (`BB_PAD_FILE`). The old walker nudged: pulses of
0.12-0.7 s, a pause, a re-measure, a camera angle corrected from the miss, side steps and chr warps ("HOP") against
walls. The nav walker (`gpu/shim/party/nav_walk.h`, hooked into `party_director.cpp`) plans on the game's own navmesh
and steers like a player.

Switches: `BB_TEST_WALKER=nav|old` (default nav since nav2078, 0.7.8; `old` keeps the pulse walker), `BB_NAV_CACHE` (set by `tools/mp/instances.py`),
`BB_TEST_MOVE_WALK=1` (see below). `BB_TEST_WALK_SPRINT=1` still holds Circle.

## 1. Where the AI's path data is (RE, with confidence)

* The game's AI pathing is Havok AI (hkai): the exe carries the reflection names of `hkaiNavMesh`,
  `hkaiNavMeshInstance`, `hkaiPathfindingUtilFindPathInput/Output`, `hkaiNavMeshPathSearchParameters`,
  `hkaiLocalSteering`, `hkaiPathFollowingProperties` (`strings.tsv` 0x4764871-0x4767714). The functions are stripped
  and have no name or string xref in `deps\ghidra-export`; the classes are only registered through reflection tables,
  so the path search entry is not findable by a string; it would have to be traced from an AI goal (Approach /
  MoveToTarget in the Lua AI -> the NpcIns ComManipulator +0x3B8 -> hkaiBehavior) with a live debugger. (Confidence
  that the AI uses hkaiNavMesh A* + local steering: high. Confidence of any address for a callable path query: none.)
* Calling it safely from the test director is a poor trade: the query needs the live `hkaiWorld`, the
  `hkaiNavMeshInstance` of the block (instances are cut by closed gates and moving cars), an input struct with
  agent properties and a mediator, on the game thread, and its output lives in Havok pools. A wrong struct would
  crash the game, which is exactly the fragile test aid this work must not add.
* The same data is in the map files: `map/mAA_BB_CC_DD/mAA_BB_CC_DD.nvmhktbnd.dcx` is a BND4 of Havok 2014
  packfiles with one `hkaiNavMesh` per piece (faces, edges with the opposite face, vertices in map coordinates);
  `tools/party/nav_route.py` (0.7.2) already read them. That is the enemies' navmesh, so the walker's corridors are
  the AI's corridors (closed gates are in it, as the AI's static mesh has them; they are not the dynamic cutter's
  state). NVA / MCG graphs are not needed: the navmesh pieces link by their boundary edges.

**Chosen design: offline extraction + our own A* and funnel.** `tools/party/nav_cache.py GAME OUT [AREA...]` writes one
compact `nav_mAA.bin` per area (all maps `mAA_*`, so the m24_00 / m24_01 seam is linked): convex faces, vertices and
face links with their portal (the shared edge; across files the overlap point). m24 is 58 801 faces, 8 MB, loaded in
20 ms, a 250 m route plans in 4 ms. The director loads the file of the player's area (`map id >> 24`) when a walk
starts. Why not the runtime query: no safe call exists (above); the data is identical; the cache also lets the
harness answer "is this point on the mesh" (`NavMesh::FaceAt` / `Snap`). Failure modes it reports:
`start is off the navmesh` (on a lift car, in a ladder: the old walker takes over, logged), `goal is off the
navmesh` (nothing within 4 m), `no route (goal on another navmesh island)` (e.g. Cathedral Ward's bottom end): these
two end the walk at once with `approach: NAV UNREACHABLE` / `GAVE UP`.

## 2. How it walks

* Planner: A* over faces (portal midpoints), then string pulling (funnel) with the portals pulled in by 0.45 m so the
  path keeps off wall corners. X:Z-only goals take the floor nearest the player's height.
* Camera: the stick is camera-relative. The walker reads the view camera's world position (RendMan config
  `+0x50 +0x80`, the host_gfx `log` value) and takes the camera yaw as `atan2(player - camera)`; the stick angle is
  `wanted heading - camera yaw`. The observed motion (velocity direction under the commanded stick) is compared with the
  model; eight disagreeing observations in a row (>26 deg) switch to the observed offset (`cam MISMATCH` in the end line),
  and with no readable camera the offset is learned from the first 2 m/s of motion. No pulses, no trial and error.
* Pure pursuit: aims at the point 0.9-1.8 m ahead on the corridor (shorter when slow), the commanded heading turns at
  most 4.5 rad/s, full tilt except inside 4 m of the goal (down to 0.45) and at sharp corners (0.6). The stick is
  rewritten at about 25 Hz and only when it changed.
* Replan when 2.5 m off the corridor. Stall (no 0.4 m of progress in 1.5 s): 1 replan, 2 back up and strafe, 3 roll
  (Circle), 4 back up and strafe the other way, 5 a counted warp onto the path 4 m ahead (y + 1 m, because navmesh y
  can sit under the floor; logged as `route: HOP`), after 3 warps `blocked: no way past` fails the walk.
* Route steps: the waypoints are goals planned in order (one that is off the mesh is dropped as a hint; the last one
  must be reachable). Stick steps: one goal. `obj:` and `stain` goals are X:Z.
* The end lines keep their old shape (`test: approach: arrived|GAVE UP at N m`, `test: route: DONE|STOPPED ...`) plus
  `(nav: seconds, replans, back-ups, strafes, rolls, warps, stalls, cam model)`.
* `BB_TEST_MOVE_WALK=1`: a `host_move` / `guest_move` that is only a placement (3-80 m, same map, within 6 m of height,
  no `:pin`, `:native`, or yaw) is walked when the planner reaches the spot within 120 m; the step chain waits for the
  walk. Warps stay where the warp is the test (lamp travel, `host_warp`, falls, pinned fixtures).

## 3. Unit tests

`tests/test_nav_walk.cpp` (`ninja -C out/gpu nav-walk-test`): planner on synthetic meshes (straight, detour, island,
two floors, snap), the funnel keeps off a wall end, stick math and camera model (including the mismatch switch),
path following, a simulated player walks open floor at four camera yaws with a continuous stick and no warps, a
detour without a camera, X:Z hints, an unreachable goal fails at the start, a blocked player goes through the
recovery ladder (replan, back up, roll, 3 counted warps, a clear failure), replan when knocked off, turn-rate limit
and ease-off.

## 4. Map frames (br, the Nightmare maps)

The hkaiNavMesh files are in the map part's local frame. Most maps sit at the origin (m21, m24, m25); others do not:
`nav_cache.py` takes the candidate origins from the MSB collision / connect-collision / navmesh parts (+0x28 of a part
entry, type at +0x14) and keeps the one that puts the most of the map's lamp spots (ids_tool's return spots, world frame) on the
mesh (at least half of them and 2, else the local frame stays). Found: m22 (-185.8, -13.9, 418.0), m23 (50, -30, 0), m26
(0, 1000, 0), m28 (255, -37.5, 120.5), m33 (0, 1500, 0), m34 (-500, 1500, -500), m35 (-792.2, 1489.5, -723.8), m36
(-513.2, 1594.5, -824.2); all their lamp spots land on the mesh. NOT verified (kept local, so the walker falls back to
the old one there): m27 (two parts are rotated -70 deg, 0 of 2 spots), m29 (Chalice layouts, 1 of 7), m32 (yaw -20 deg, 1 of 6).
Rotated parts would need a per-part transform. Goals off the mesh by up to 10 m (the br walk through an opened fog
wall) are walked straight to, as a tail of the route. `tests/test_nav_cache.py` checks the offsets against the game data.

Placements with yaw: a candidate is origin + yaw (Y rotation in degrees at +0x30 of the part, applied as
x' = ox + x cos + z sin, z' = oz - x sin + z cos; both signs are tried and the lamp spots pick). m27: (-190.31, -187.18, 312.4)
yaw -70, 2 of 2 spots. m32 is two regions placed apart (origins (-429.37, -191.25, 511.9) and (1470.63, -191.25, 711.9), yaw -20): pieces with a
spot on them take their own placement, the rest are glued on by coinciding vertices, 15 pieces with neither are left
out (not guessed); 6 of 6 spots on the mesh and the rj start (-458, -176.3, 393) now plans. m29 (Chalice) stays in the
local frame: the layout is assembled from blocks at run time, so the placement of each block would have to come from the
running game (the dungeon's block table), not from the MSB; not guessed.

Goals far off the mesh (a ledge to fall from, rj's lake) use the old straight walk; an island (no route) still fails.

Camera model in m34 / m35: the render-matrix yaw disagreed with the motion in the first 3-4 s after a walk starts
(50-170 deg, no pattern) and did not come back (log "camera model dropped ... block rows"); the observed offset then
steers, all walks arrive. The block is the same one (the rows are unit vectors, up = +y); the likely cause is the
camera still swinging after the warp that put the player there. Unproven; a longer walk with the raw yaw logged each
second would settle it.

## 5. Default switch (nav2078, 0.7.8)

Nav is the default; `BB_TEST_WALKER=old` keeps the pulse walker. First-try pass per phase (one run each, 30 fps, same build,
`tools/mp/smoke_party.py --phase X` with and without `BB_TEST_WALKER=old`): fl, fd, fdj, cd, mk, cpa, cae, cao, caw, cab, caq, br, rj, eh, cis, s3,
lfw all pass first try with both walkers (lfw: nav 39/39 in one run); s3 old needed 3 tries (nav 1). sj: old 0/3 first try, nav
before the fix 1/3, nav after 3/3 with no warp.

sj fixes: (1) the m24 navmesh ends in a dead-end neck at the cave mouth seam; the passage continues on the m27 pieces, so the
cache files of m24 and m27 now hold both areas (`JOIN` in nav_cache.py; `NAV_CACHE_VERSION` in instances.py rebuilds the two files once);
the walker used to press into the doorframe corner and the old recovery (back up, roll, warp) undid the crossing. (2) A stage record change
(the seam) holds the stall verdict for 10 s: the game freezes the walker for ~7 s while the next block streams in, a player keeps the stick forward.
(3) An X:Z goal with no floor near the player's height takes any floor of that column (amp: the boss floor is 45 m above the
guest's respawn), so the walk is planned instead of falling back to the old walker.
