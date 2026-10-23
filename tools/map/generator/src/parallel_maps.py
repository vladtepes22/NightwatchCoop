"""Runs the per-map pipeline phases in worker processes.

Each map's work is the same function on the same inputs as the sequential pipeline, so the
generated files are identical; only the maps' order of completion changes. ATLAS_WORKERS=1
keeps everything in this process.
"""
import math
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed


def worker_count():
    forced = os.environ.get('ATLAS_WORKERS', '')
    if forced.isdigit() and int(forced) > 0:
        return int(forced)
    # Shapely's overlay scales with hardware threads (8 cores / 16 threads: 14 workers 90 s, 8 workers 120 s).
    return max(1, min(14, (os.cpu_count() or 2) - 2))


def _init(game, stage):
    import pipeline_context as ctx
    ctx.configure(game, stage)
    import soulstruct_local_config
    sys.modules['soulstruct.config'] = soulstruct_local_config


def heights(mid, source, out, catalog, links):
    """generate_map_v13.generate, returning the catalog entry it fills in (a worker's copy)."""
    import generate_map_v13 as continuous
    result = continuous.generate(mid, source, out, catalog, links)
    return result, catalog[mid]


def floor_part(game, mid, part, parts):
    """Floor-plan layers of every parts-th height band of a map, starting at band index part."""
    import numpy as np
    import generate_floorplans_v12 as vector
    tri, failures, floors, walls, bands = vector.prepare(game, mid)
    return len(tri), failures, vector.band_layers(floors, walls, bands, np.unique(bands)[part::parts])


def floorplans(game, stage, source, out, mids, weights, done=None):
    """generate_floorplans_v12.generate for every map. A heavy map's height bands are split
    across workers (its collision is read once per part); each map's layers are reassembled
    in band order, so the written files are the sequential ones."""
    start = time.monotonic()
    per = sum(weights.values()) / 12
    calls = [(game, mid, part, parts) for mid in mids
             for parts in [max(1, min(4, math.ceil(weights[mid] / per)))] for part in range(parts)]
    calls.sort(key=lambda c: -weights[c[1]] / c[3])  # heaviest parts first
    results = run(floor_part, calls, game, stage, done and (lambda n: done(n, len(calls))))
    import generate_floorplans_v12 as vector
    reports = []
    for mid in mids:
        parts = [r for c, r in zip(calls, results) if c[1] == mid]
        layers = [layer for _, layer in sorted((b for _, _, found in parts for b in found), key=lambda b: b[0])]
        count, failures, _ = parts[0]
        reports.append(vector.write(source, out, mid, count, failures, layers, start))
    return reports


def portable_rewrite(path):
    """generate_atlas's final rewrite of one generated JSON file (portable source paths)."""
    import json
    from generate_atlas import jsonwrite, portable
    jsonwrite(path, portable(json.loads(path.read_text(encoding='utf-8'))))


def run(fn, calls, game, stage, done=None):
    """fn(*args) for each args in calls, results in the calls' order; done(n) after each finishes."""
    workers = min(worker_count(), len(calls))
    if workers <= 1:
        results = []
        for i, args in enumerate(calls):
            results.append(fn(*args))
            if done: done(i + 1)
        return results
    with ProcessPoolExecutor(max_workers=workers, initializer=_init, initargs=(game, stage)) as pool:
        futures = [pool.submit(fn, *args) for args in calls]
        for i, _ in enumerate(as_completed(futures)):
            if done: done(i + 1)
        return [f.result() for f in futures]
