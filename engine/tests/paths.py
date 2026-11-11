"""Repository paths for the Python tests (run: python3 -m unittest discover -s tests)."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT / 'scripts') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts'))


def test_exe(name):
    """A native test program (visy 0.8.1 tests): ours build into out/gpu, his into out."""
    exe = name + '.exe' if sys.platform == 'win32' else name
    for folder in (ROOT / 'out' / 'gpu', ROOT / 'out'):
        if (folder / exe).exists():
            return folder / exe
    return ROOT / 'out' / 'gpu' / exe


test_exe.__test__ = False  # a helper the test modules import, not a pytest test


def build_hint(name):
    """The skip reason when the test program has not been built."""
    return f'{name} not built: ninja -C out/gpu {name}'
