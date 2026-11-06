"""Moved to scripts/partydata/ (dev tools and the test oracle of the engine C++ table derivation).
This shim keeps the old import path and command line working."""
import importlib.util
import sys
from pathlib import Path

_real = Path(__file__).resolve().parents[2] / "scripts" / "partydata" / Path(__file__).name
if str(_real.parent) not in sys.path:
    sys.path.insert(0, str(_real.parent))
_spec = importlib.util.spec_from_file_location(__name__, _real)
_mod = importlib.util.module_from_spec(_spec)
sys.modules[__name__] = _mod
_spec.loader.exec_module(_mod)
