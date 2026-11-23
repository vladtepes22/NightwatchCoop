"""Models that draw with another model's textures. A character's FLVER names its textures by path; most live in its own
TPF or in its family's texture-only archive (c1110 -> c1119, which the game loads with it), but some live only in another
model's own TPF (DATA, the game and Nocturne v6.0.0: the Orphan's phase two c4541 draws its body and blade with c4540's
textures; also c1055, c1211/c1212, c2501, c2571, c2711/c2712, c4120, c4150, c4531, c4542, c5034, c5070, c5410, c5502,
c5511/c5512, c5521/c5522, c9980). Where the source model is not loaded the body draws black, and at range not at all
(LIVE 23 Nov: c4541 alone in the Witch's arena). The source model must be loaded in the same map (planner budget,
generate.texture_parts)."""
import os
import re
import struct
from concurrent.futures import ThreadPoolExecutor

from containers import bnd4_read, dcx_read

_TEXTURE = re.compile(rb"((?:[\x20-\x7e]\x00){4,})")
_FOLDER = re.compile(r"chr[^a-z0-9](c[0-9]{4})[^a-z0-9]")


def tpf_names(data):
    """The texture names in a TPF (lower case). DATA PS4: count at 8, encoding byte at 0xe (1 = UTF-16), 0x24-byte
    entries from 0x10 with the name offset at +0x18."""
    count, wide = struct.unpack_from("<i", data, 8)[0], data[0xe] == 1
    out = set()
    for i in range(count):
        at = struct.unpack_from("<i", data, 0x10 + i * 0x24 + 0x18)[0]
        if wide:
            end = at
            while data[end:end + 2] != b"\0\0":
                end += 2
            out.add(data[at:end].decode("utf-16-le").lower())
        else:
            out.add(data[at:data.index(b"\0", at)].decode("latin-1").lower())
    return out


def flver_textures(data):
    """{(folder model or None, texture name)} a FLVER names (its UTF-16 texture paths)."""
    out = set()
    for found in _TEXTURE.finditer(data):
        path = found.group(1).decode("utf-16-le").lower()
        if path.endswith((".tga", ".tif", ".dds", ".psd")):
            folder = _FOLDER.search(path)
            out.add((folder.group(1) if folder else None, re.split(r"[^a-z0-9_.]", path)[-1].rsplit(".", 1)[0]))
    return out


def _read(files, model):
    """(textures its FLVERs name, names its TPFs hold) of one model's chrbnd; None without one."""
    rel = "chr/%s.chrbnd.dcx" % model
    if files.path(rel) is None:
        return None
    named, held = set(), set()
    for entry in bnd4_read(dcx_read(files.read(rel))[0])["entries"]:
        name = entry[2].lower()
        if name.endswith(".flver"):
            named |= flver_textures(entry[3])
        elif name.endswith(".tpf"):
            held |= tpf_names(entry[3])
    return named, held


def sources(files, models):
    """{model: sorted source models} for the models among `models` that need another model loaded for their textures:
    a texture the FLVER files under another model's folder that neither its own TPF nor a family texture archive
    (cXXX9 of its own family or of that folder's) holds, but that folder model's TPF does."""
    models = sorted(set(models))
    with ThreadPoolExecutor(max_workers=max(1, min(8, os.cpu_count() or 1))) as pool:
        read = dict(zip(models, pool.map(lambda m: _read(files, m), models)))
    cache = dict(read)

    def held(model):
        if model not in cache:
            cache[model] = _read(files, model)
        return cache[model][1] if cache[model] else set()

    out = {}
    for model in models:
        if read[model] is None:
            continue
        named, own = read[model]
        reach = own | held(model[:4] + "9")
        need = set()
        for folder, name in named:
            if folder is None or folder == model or folder.endswith("9") or name in reach:
                continue
            if name in held(folder[:4] + "9"):
                continue
            if name in held(folder):
                need.add(folder)
        if need:
            out[model] = sorted(need)
    return out
