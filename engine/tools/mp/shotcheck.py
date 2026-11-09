"""Smoke screenshots: dependency check, black-frame detection and the per-instance visual verdict.

No PIL import at module level: `require_pil()` is the one place that turns a missing Pillow into a
clear error (it used to be an ImportError inside the shot.py subprocess, whose output nobody read:
a smoke ran to the end with no screenshots at all).

A frame is BLACK when its 64x36 luminance thumbnail is near-uniform and dark (max luma below
BLACK_MAX, std-dev below BLACK_STD) - an all-zero capture, but also a flat dark fill. A loading
screen has its text / spinner / tip and stays above that; so does a night scene or a dark
interior (max luma of the lamps, the sky, the HUD).
"""
import json
import re
import sys
from pathlib import Path

BLACK_MAX = 12      # brightest thumbnail pixel (0-255) of a black frame
BLACK_STD = 3.0     # luminance std-dev of a black frame
MIN_SHOTS = 3       # fewer shots after the first picture: not enough to judge a trailing black
BLACK_SHARE = 0.5   # an instance fails when this share (or more) of its shots, from its first picture on, is black

SHOT_NAME = re.compile(r"_inst(\d+)\.png$")


class ShotDependencyError(RuntimeError):
    pass


def require_pil(import_module=None):
    """Returns the PIL.Image module, or raises ShotDependencyError with what to install."""
    try:
        if import_module is not None:
            return import_module("PIL.Image")
        from PIL import Image  # noqa: WPS433
        return Image
    except ImportError as e:
        raise ShotDependencyError(
            "Pillow (PIL) is missing, so the smoke cannot take or check screenshots (%s). Install it "
            "with `%s -m pip install pillow`, or pass --no-shots to run without the visual check."
            % (e, sys.executable)) from e


def frame_stats(img):
    """(max luma, mean luma, std-dev) of the 64x36 thumbnail of a PIL image."""
    px = list(img.convert("L").resize((64, 36)).tobytes())
    mean = sum(px) / len(px)
    std = (sum((v - mean) ** 2 for v in px) / len(px)) ** 0.5
    return max(px), mean, std


def is_black_stats(mx, mean, std):
    return mx < BLACK_MAX and std < BLACK_STD


def is_black(img):
    return is_black_stats(*frame_stats(img))


def judge(flags):
    """flags: per shot of one instance in time order, True = black. -> (ok, detail)."""
    n = len(flags)
    if n == 0:
        return False, "no screenshot of this instance (the capture produced nothing)"
    if all(flags):
        return False, "all %d screenshots are black (no picture was ever captured)" % n
    first = flags.index(False)
    tail = flags[first:]
    blk = sum(tail)
    if len(tail) >= MIN_SHOTS and blk / len(tail) >= BLACK_SHARE:
        return False, "%d of %d screenshots after the first picture are black" % (blk, len(tail))
    return True, "%d screenshots, %d black (loading / fades)" % (n, sum(flags))


def shot_order(p):
    """Time order of a shots/ file: the elapsed seconds of `tag_<el>s_instN.png`, else its name."""
    m = re.search(r"_(\d+(?:\.\d+)?)s_inst\d+\.png$", p.name)
    return (float(m.group(1)) if m else -1.0, p.name)


def check_dir(shots_dir, insts, Image=None):
    """Visual verdict of one phase: [(inst, ok, detail)] for each rendered instance in `insts`."""
    Image = Image or require_pil()
    shots_dir = Path(shots_dir)
    per = {i: [] for i in insts}
    for p in sorted(shots_dir.glob("*.png"), key=shot_order):
        if p.name.startswith("GREY_"):
            continue
        m = SHOT_NAME.search(p.name)
        if not m or int(m.group(1)) not in per:
            continue
        try:
            with Image.open(p) as img:
                per[int(m.group(1))].append(is_black(img))
        except Exception as e:  # a truncated PNG is a failed capture, not a crash of the verdict
            per[int(m.group(1))].append(True)
            print("shotcheck: unreadable %s (%s)" % (p.name, e), file=sys.stderr)
    return [(i,) + judge(per[i]) for i in sorted(per)]


def rendered_instances(phase_dir):
    """The instances that draw a window: instances.json (count, headless) of a phase folder."""
    try:
        d = json.loads((Path(phase_dir) / "instances.json").read_text())
        return list(range(max(0, int(d["count"]) - int(d.get("headless") or 0))))
    except (OSError, ValueError, KeyError, TypeError):
        return [0]


def visual_rows(phase_name, phase_dir, Image=None):
    """Verdict rows (name, ok, detail) for a phase folder; [] when it has no shots folder
    (a re-verdict of a run made without shots) - a folder with no PNG at all is a FAILED row."""
    phase_dir = Path(phase_dir)
    shots = phase_dir / "shots"
    if not shots.is_dir():
        return []
    return [("%s: visual check inst%d" % (phase_name, i), ok, detail)
            for i, ok, detail in check_dir(shots, rendered_instances(phase_dir), Image)]
