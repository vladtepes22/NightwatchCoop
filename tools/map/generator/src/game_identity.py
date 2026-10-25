"""Which game folders the map generator accepts: the engine's own check (engine/scripts/game_check.py).

The game is identified by the SHA-256 of eboot.bin's loaded image, never by a hash of eboot.bin
itself: dump tools write different SELF headers around the same 1.09 code, and the raw-file hash
this generator once used refused such players ("Unsupported executable fingerprint"); see
engine/docs/party/version_check.md, "Game identity". Run from source, game_check is found in the
repository; the frozen exe bundles it (generate_atlas.spec).
"""
import sys
from pathlib import Path

if not getattr(sys, 'frozen', False):
    sys.path.append(str(Path(__file__).resolve().parents[4] / 'engine' / 'scripts'))

import game_check  # noqa: E402

# The game version the map data belongs to, recorded in features.json for the engine's map reader
# (engine/gpu/shim/map/map_data_rules.h).
GAME_IMAGE_SHA256 = game_check.SUPPORTED_IMAGE


def require_supported(game):
    """Raises ValueError with what to do unless `game` holds Bloodborne CUSA03173 at 1.09."""
    if found := game_check.check(game):
        raise ValueError(game_check.explain(*found) + ' No game files were changed.')
