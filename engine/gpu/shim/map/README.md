Map code shared by the in-game map and the map baker (`tools/map/bake_map.cpp`).

These headers are taken unchanged (line endings aside) from Hunter's Atlas by MikeTat1988,
github.com/MikeTat1988/HuntersAtlas commit `06d4021`, GPL-2.0-or-later:

- `core.h`, `texture_budget.h` from `source/atlas2/`
- `bbport_atlas_style.h`, `bbport_atlas_interiors.h` from `source/native/gpu/shim/`

Adapted from the same project (each file says what it follows):

- `map_vector.*`: the renderer (`source/compact/map_vector.cpp`, `vector_backend.cpp`)
- `map_page.cpp`: the map view and its controls (`source/compact/bbport_map.cpp`), without map travel
- `map_data_rules.h`: lamps, pickups, boss reveal, NPC sites, place names
  (`source/native/gpu/shim/bbport_map_features.cpp`), from the generator's own output on the player's PC
- `map_art.*`: the frame and icon pictures (`source/native/gpu/shim/bbport_map_art.cpp`); the pictures are
  `tools/map/art/*.png` (Hunter's Atlas `source/art`), shipped as BBRA beside `parchment.bbra`
