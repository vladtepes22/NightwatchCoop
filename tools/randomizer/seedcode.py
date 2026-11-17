"""The seeded random streams and the shareable code. Integer only; no `random` module, no float decisions."""
import hashlib

GENERATOR_MAJOR = 2  # raised by any fix that changes output; old codes are then refused, never reinterpreted
SCHEMA = 2  # the code text format; R1 (40-bit seed, every option tagged) was never shared and is refused
MASK = (1 << 64) - 1
DRAW_LIMIT = 256


def _rotl(x, k):
    return ((x << k) | (x >> (64 - k))) & MASK


def splitmix64(state):
    """(next state, output)"""
    state = (state + 0x9E3779B97F4A7C15) & MASK
    z = state
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
    return state, z ^ (z >> 31)


class Stream:
    """xoshiro256**, keyed by SHA-256 of the seed, options, domain and slot."""

    def __init__(self, seed, options_hash, *key):
        text = "\0".join([str(GENERATOR_MAJOR), str(seed), options_hash] + [str(k) for k in key])
        digest = hashlib.sha256(text.encode("utf-8")).digest()
        s = [int.from_bytes(digest[i:i + 8], "little") for i in range(0, 32, 8)]
        if not any(s):
            state = 0
            for i in range(4):
                state, s[i] = splitmix64(state)
        self.s = s

    def next(self):
        s = self.s
        result = (_rotl((s[1] * 5) & MASK, 7) * 9) & MASK
        t = (s[1] << 17) & MASK
        s[2] ^= s[0]
        s[3] ^= s[1]
        s[1] ^= s[2]
        s[0] ^= s[3]
        s[2] ^= t
        s[3] = _rotl(s[3], 45)
        return result

    def below(self, n):
        """A uniform integer in [0, n) by bounded rejection. At the limit, 0: one bias in 2**(256*k) draws."""
        if n <= 0:
            raise ValueError("empty range")
        limit = ((1 << 64) // n) * n
        for _ in range(DRAW_LIMIT):
            value = self.next()
            if value < limit:
                return value % n
        return 0

    def percent(self, chance):
        return self.below(100) < chance

    def shuffled(self, items):
        out = list(items)
        for i in range(len(out) - 1, 0, -1):
            j = self.below(i + 1)
            out[i], out[j] = out[j], out[i]
        return out


# id, name, kind, default. Only non-defaults are stored. Add new ids at the end; never reuse one.
# kinds: int = a varint; list = a count and varints; pairs = a count and varint pairs.
OPTIONS = (
    (1, "enemy_chance", "int", 100),      # percent of free slots that change, 0-100 in steps of 5
    (2, "area_chance", "pairs", ()),      # (area number such as 2401, percent) overriding enemy_chance
    (3, "exclude", "list", ()),           # model numbers such as 2630 that never become a replacement
    (4, "oops_all", "int", 0),            # model number: every changed slot becomes this model
    (5, "scaling", "int", 0),             # 0 slot strength, 1 replacement's own strength, 2 fixed
    (6, "fixed_effect", "int", 0),        # the area SpEffect id used when scaling is 2
    (7, "rewards", "int", 0),             # 0 the slot's echoes and drops, 1 the replacement's
    (8, "map_models", "int", 24),         # model cap of one map; above vanilla's only with a crash warning
    (9, "map_mb", "int", 343),            # uncompressed chrbnd MB cap of one map
    (10, "big_models", "int", 2),         # retired, no effect (the cap never bound); kept so codes holding it still read
    (11, "variety", "int", 0),            # distinct new models per map, 0 = the map's vanilla free kinds x headroom / 100
    (12, "boss_swap", "int", 0),          # 1: one-body arenas trade bosses
    (13, "treasure", "int", 0),           # 1: treasure lots trade places, required ones pinned
    (14, "shops", "int", 0),              # 1: every shop row but Blood Vials and Quicksilver Bullets changes (shops.py)
    (15, "drops", "int", 0),              # 1: normal enemies' drop lots trade places, required ones pinned
    (16, "headroom", "int", 100),         # percent of vanilla's model and memory caps, per map and per loaded-together group; above 100 only with a crash warning
    (17, "treasure_by_type", "int", 0),   # 1: treasure lots only trade with lots of the same kind (weapon, armour, accessory, goods)
    (18, "starters", "int", 0),           # 1: the five free first weapons are drawn again and need no stats
    (21, "boss_cutscenes", "int", 0),     # retired, no effect: scenes on swapped arenas are always skipped; kept so old codes read
    (22, "chalice_materials", "int", 0),  # 1: chalice ritual materials shuffle too; 0: their map pickups, shop rows and enemy drops stay
    (23, "starter_melee", "int", 3),      # free melee weapons the Hunter's Dream offers at the start, 1-5 (above 3 adds rows)
    (24, "starter_guns", "int", 2),       # free firearms it offers, 1-3 (above 2 adds a row)
    # boss round 2 (4 Nov 2026): with boss_swap every fight of the main maps is swapped; these choose how
    (26, "witch_mix", "int", 0),          # 1: each Witch of Hemwick body draws its own boss; 0: one boss on body 1, the other leaves
    (27, "shadows_mix", "int", 0),        # 1: each Shadow of Yharnam draws its own boss; 0: one boss on body 1, the others leave
    (28, "emissary_mix", "int", 0),       # 1: every Celestial Emissary body becomes a boss with its own HP; 0: one boss on the real one, the small ones leave
    (29, "failures_mix", "int", 0),       # 1: every Living Failure becomes a boss with its own HP; 0: one boss replaces them all
    (30, "bell_maidens", "int", 0),       # One Reborn's bell maidens: 0 stay, 1 replaced from the normal pool, 2 from the difficult pool
    (31, "chalice_bosses", "int", 0),     # 1: the strong Chalice Dungeon bosses (deepest rows) join the boss pool; the chalice maps stay untouched
    (32, "hard_rom", "int", 0),           # 1: Rom fights with her deepest chalice row, in her own arena or wherever she goes
    (33, "hard_amygdala", "int", 0),      # 1: Amygdala likewise
    (34, "hard_ebrietas", "int", 0),      # 1: Ebrietas likewise
    (35, "difficult_enemies", "int", 0),  # 1: replaced ordinary enemies draw from the difficult list where it fits the map's memory
    # two-body bosses (user, 5 Nov 2026): Father Gascoigne, Ludwig, the Orphan of Kos
    (36, "second_stage", "int", 0),       # 1: wherever they fight, their second body holds another boss (planner.second_stages); 0: their own second phase
    (37, "away_from_home", "int", 0),     # 1: a replaced enemy is not one of its map's own vanilla kinds where another fits (planner.plan, user 6 Nov)
    (38, "free_poses", "int", 0),         # 1: an enemy the map's events force an animation on may be any enemy, which stands where it lacks one (planner.posed_slot, user 6-7 Nov)
    (39, "starter_armor", "int", 0),      # 1: a new character starts in a random head, chest, arms and legs piece the game hands out (starters.armor_plan, user 7 Nov)
    (40, "shadows_summons", "int", 0),    # 1: the Shadows of Yharnam summon random enemies; 0: nothing is summoned (gamedata.SUMMONS, user 8 Nov)
    (41, "chalice_enemies", "int", 0),    # 1: the Chalice Dungeons' regular enemy kinds join the enemy pool (chalice_enemies.py, PLAN 14)
    (42, "bosses_as_enemies", "int", 0),  # 1: now and then a boss stands in an ordinary enemy slot, as a plain enemy (planner.enemy_bosses, user 7 Nov)
    (43, "coiled_host", "int", 0),        # 1: Nocturne's Coiled Host is a boss of the pool and his own slot draws a boss (arenas.PHASES, user 7 Nov); no effect without him
    (44, "gem_roulette", "int", 0),       # 1: world blood gems, half the small vial/bullet pickups and enemy gem drops become fixed droplet gems (gems.py, user 8 Nov)
    (45, "cursed_gems", "int", 0),        # 1 (with gem_roulette): some gems trade a side effect for a curse and are stronger (gems.CURSE_CHANCE)
    (46, "boss_enemy_chance", "int", 25), # with bosses_as_enemies: percent chance for a map to hold bosses as enemies, 0-100 in steps of 5 (user 8 Nov)
    (47, "boss_enemy_max", "int", 1),     # with bosses_as_enemies: most bosses a map holds as enemies, 1-5; a map that rolls one draws 1 to this many
    (48, "huge_bosses_as_enemies", "int", 0),  # 1: bosses wider than any ordinary enemy (generate.huge_rows) may stand in enemy slots too
    (49, "boss_enemy_hp", "int", 8),      # with bosses_as_enemies: a boss as an enemy has this many times the area's typical enemy base HP, 8-16 in steps of 2 (generate.boss_enemy_hp, user 8 Nov)
    (50, "no_winter_lanterns", "int", 0), # 1: Winter Lanterns never replace an enemy (planner.excluded_models, user 10 Nov: their Frenzy reaches from afar)
    (51, "include_only", "list", ()),     # model numbers: when any, only these replace an ordinary enemy; exclude still wins (planner.replacement_pool, user 11 Nov)
    (52, "scorched_beast", "int", 0),     # 1: Nocturne v5.0.0's Scorched Beast (the Lesser Demon, c5500 with his head c5501) is a boss of the pool and his plaza draws a boss (arenas.py "lesser_demon"); no effect without him
)
_BY_ID = {o[0]: o for o in OPTIONS}
LATE_OPTIONS = {"headroom": 100, "treasure_by_type": 0, "starters": 0, "boss_cutscenes": 0,
                "chalice_materials": 0, "starter_melee": 3, "starter_guns": 2, "witch_mix": 0, "shadows_mix": 0,
                "emissary_mix": 0, "failures_mix": 0, "bell_maidens": 0, "chalice_bosses": 0, "hard_rom": 0, "hard_amygdala": 0,
                "hard_ebrietas": 0, "difficult_enemies": 0, "second_stage": 0, "away_from_home": 0, "free_poses": 0,
                "starter_armor": 0, "shadows_summons": 0, "chalice_enemies": 0, "bosses_as_enemies": 0, "coiled_host": 0, "gem_roulette": 0,
                "cursed_gems": 0, "boss_enemy_chance": 25, "boss_enemy_max": 1, "huge_bosses_as_enemies": 0,
                "boss_enemy_hp": 8, "no_winter_lanterns": 0, "include_only": (), "scorched_beast": 0}
BOOL_OPTIONS_R2 = ("witch_mix", "shadows_mix", "emissary_mix", "failures_mix", "chalice_bosses", "hard_rom",
                   "hard_amygdala", "hard_ebrietas", "difficult_enemies", "second_stage", "away_from_home", "free_poses",
                   "starter_armor", "shadows_summons", "chalice_enemies", "bosses_as_enemies", "coiled_host", "gem_roulette", "cursed_gems",
                   "huge_bosses_as_enemies", "no_winter_lanterns", "scorched_beast")
CONTENT_ONLY_OPTIONS = ("boss_cutscenes", "boss_enemy_hp")  # boss_enemy_hp sets HP only: the same enemies at 8x or 16x
# the most bosses a map may hold as enemies (option boss_enemy_max): five of the largest boss models (22 MB) are a third of
# a map's 343 MB budget at 100 %, about as much as the boss models can take before the map's own variety goes
BOSS_ENEMY_MAX = 5
# the choices of option boss_enemy_hp (user 8 Nov: 8x to 16x the area's typical enemy HP)
BOSS_ENEMY_HP = (8, 10, 12, 14, 16)
# retired options still read from codes; the hash sees them at the value every world was made with
RETIRED_VALUES = {"big_models": 2}
_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"


def defaults():
    return {o[1]: o[3] for o in OPTIONS}


def normalize(options):
    """Validated options with every field present and lists in canonical sorted form."""
    out = defaults()
    known = set(out)
    for key, value in options.items():
        if key not in known:
            raise ValueError("unknown option " + key)
        out[key] = value
    out["exclude"] = tuple(sorted(set(out["exclude"])))
    out["include_only"] = tuple(sorted(set(out["include_only"])))
    out["area_chance"] = tuple(sorted((int(a), int(p)) for a, p in dict(out["area_chance"]).items()))
    for name in ("enemy_chance",):
        if not (0 <= out[name] <= 100 and out[name] % 5 == 0):
            raise ValueError(name + " must be 0-100 in steps of 5")
    for _, percent in out["area_chance"]:
        if not (0 <= percent <= 100 and percent % 5 == 0):
            raise ValueError("area percent must be 0-100 in steps of 5")
    if out["scaling"] not in (0, 1, 2) or out["rewards"] not in (0, 1) or out["boss_swap"] not in (0, 1) or out["treasure"] not in (0, 1) or out["shops"] not in (0, 1) or out["drops"] not in (0, 1):
        raise ValueError("bad scaling or rewards value")
    if not (100 <= out["headroom"] <= 300 and out["headroom"] % 5 == 0):
        raise ValueError("headroom must be 100-300 in steps of 5")
    if not 0 <= out["big_models"] <= 24:
        raise ValueError("big_models must be 0-24")
    if not 1 <= out["starter_melee"] <= 5 or not 1 <= out["starter_guns"] <= 3:
        raise ValueError("starter_melee must be 1-5 and starter_guns 1-3")
    if not (0 <= out["boss_enemy_chance"] <= 100 and out["boss_enemy_chance"] % 5 == 0) or not 1 <= out["boss_enemy_max"] <= BOSS_ENEMY_MAX:
        raise ValueError("boss_enemy_chance must be 0-100 in steps of 5 and boss_enemy_max 1-%d" % BOSS_ENEMY_MAX)
    if out["boss_enemy_hp"] not in BOSS_ENEMY_HP:
        raise ValueError("boss_enemy_hp must be one of %s" % (BOSS_ENEMY_HP,))
    if out["treasure_by_type"] not in (0, 1) or out["starters"] not in (0, 1) or any(out[k] not in (0, 1) for k in ("boss_cutscenes", "chalice_materials")):
        raise ValueError("treasure_by_type, starters, boss_cutscenes and chalice_materials are 0 or 1")
    if (out["scaling"] == 2) != (out["fixed_effect"] != 0):
        raise ValueError("fixed_effect goes with scaling 2 only")
    if any(out[k] not in (0, 1) for k in BOOL_OPTIONS_R2) or out["bell_maidens"] not in (0, 1, 2):
        raise ValueError("the boss round options are 0 or 1, bell_maidens 0-2")
    return out


def options_hash(options):
    norm = normalize(options)
    # options added after the first release of the draws are left out at their default, so older codes keep their world;
    # options that only add content (no draws) are always left out, so turning them on keeps the same world
    norm["big_models"] = RETIRED_VALUES["big_models"]  # retired: any value makes the world of the default
    text = repr(sorted((k, v) for k, v in norm.items()
                       if k not in CONTENT_ONLY_OPTIONS and not (k in LATE_OPTIONS and v == LATE_OPTIONS[k])))
    return hashlib.sha256(text.encode("utf-8")).hexdigest()



# ---- The code text (schema 2) ----
# payload = varint(len(body)) + body; body = seed (4 bytes, big endian) + varint(core) + extras.
# core: every option the launcher sets, always present. Bit layout, low to high (never reorder; new toggles take the next
# free bit above the last field, and an older generator refuses a code with a bit it does not know):
CORE_FLAGS = ("boss_swap", "treasure", "shops", "drops", "treasure_by_type", "starters",
              "boss_cutscenes", "chalice_materials", "rewards")                    # bits 0-8, one each
CORE_FIELDS = (("scaling", 9, 2), ("enemy_chance", 11, 5), ("headroom", 16, 6))  # (name, first bit, width), as steps
CORE_BITS = 22
# extras: free-form options, only when not at their default, as varint(option id) + value, ids ascending.
# Percents are stored as steps of 5 (enemy_chance in the core too).
EXTRAS = (2, 3, 4, 6, 8, 9, 10, 11, 23, 24) + tuple(range(26, 53))  # area_chance, exclude, oops_all, fixed_effect, map_models, map_mb, big_models, variety, starter_melee, starter_guns, the boss round options, second_stage, away_from_home, free_poses, starter_armor, shadows_summons, chalice_enemies, bosses_as_enemies, coiled_host, gem_roulette, cursed_gems, boss_enemy_chance, boss_enemy_max, huge_bosses_as_enemies, boss_enemy_hp, no_winter_lanterns, include_only
SEED_BITS = 32


class CodeError(ValueError):
    """kind 'newer': made by a newer generator (unknown schema, core bit or option): update the launcher.
    kind 'bad': not a valid code (typing mistake, checksum, characters, length, or an old schema)."""

    def __init__(self, kind, message):
        super().__init__(message)
        self.kind = kind


def _step(name, value):
    return (value - 100) // 5 if name == "headroom" else value // 5 if name == "enemy_chance" else value


def _unstep(name, step):
    return 100 + step * 5 if name == "headroom" else step * 5 if name == "enemy_chance" else step


def _varint(value):
    out = bytearray()
    if value < 0:
        raise ValueError("negative option value")
    while True:
        byte = value & 0x7f
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def _read_varint(data, at, end):
    value, shift = 0, 0
    while True:
        if at >= end or shift > 63:
            raise CodeError("bad", "the code is cut short")
        byte = data[at]
        at += 1
        value |= (byte & 0x7f) << shift
        shift += 7
        if not byte & 0x80:
            return value, at


def _crc10(data):
    crc = 0xffff
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
    return crc & 0x3ff


def _base32(data):
    bits = int.from_bytes(data, "big")
    n = (len(data) * 8 + 4) // 5
    bits <<= n * 5 - len(data) * 8
    return "".join(_ALPHABET[(bits >> (5 * (n - 1 - i))) & 31] for i in range(n))


def _unbase32(text, size):
    bits = 0
    for ch in text:
        bits = (bits << 5) | _ALPHABET.index(ch)
    return ((bits >> (len(text) * 5 - size * 8)) & ((1 << (size * 8)) - 1)).to_bytes(size, "big")


def _text(seed, options):
    """The code's characters without prefix or dashes."""
    if not 0 <= seed < 1 << SEED_BITS:
        raise ValueError("seed must fit %d bits" % SEED_BITS)
    norm = normalize(options)
    core = 0
    for bit, name in enumerate(CORE_FLAGS):
        core |= norm[name] << bit
    for name, first, width in CORE_FIELDS:
        core |= _step(name, norm[name]) << first
    body = bytearray(seed.to_bytes(4, "big")) + _varint(core)
    for option_id in EXTRAS:
        _, name, kind, default = _BY_ID[option_id]
        value = norm[name]
        if value == default:
            continue
        body += _varint(option_id)
        if kind == "int":
            body += _varint(value)
        elif kind == "list":
            body += _varint(len(value))
            for item in value:
                body += _varint(item)
        else:
            body += _varint(len(value))
            for area, percent in value:
                body += _varint(area) + _varint(percent // 5)
    # a length first: base32 padding bits must not read as an option
    payload = _varint(len(body)) + bytes(body)
    crc = _crc10(payload)
    return _base32(payload) + _ALPHABET[crc >> 5] + _ALPHABET[crc & 31]


def encode(seed, options):
    """The shareable code in its one canonical form, such as R2-7KQ2M-9XF4T-B3H8N-QK."""
    text = _text(seed, options)
    return "R%d-" % SCHEMA + "-".join(text[i:i + 5] for i in range(0, len(text), 5))


def decode(code):
    """(seed, options). Raises CodeError: kind 'newer' or 'bad' (see there). Case, dashes and the look-alikes O, I, L
    are forgiven; anything else that does not re-encode to the same characters is refused."""
    text = code.strip().upper()
    head, _, rest = text.partition("-")
    if not (len(head) >= 2 and head[0] == "R" and head[1:].isdigit()):
        raise CodeError("bad", "this is not a randomizer code")
    schema = int(head[1:])
    if schema > SCHEMA:
        raise CodeError("newer", "this code was made by a newer randomizer: update the launcher")
    if schema != SCHEMA:
        raise CodeError("bad", "this code was made by an old randomizer that is no longer supported")
    text = rest.replace("-", "").replace("O", "0").replace("I", "1").replace("L", "1")
    if len(text) < 3 or any(ch not in _ALPHABET for ch in text):
        raise CodeError("bad", "the code has characters that do not belong in it")
    body, crc_text = text[:-2], text[-2:]
    size = len(body) * 5 // 8
    payload = _unbase32(body, size)
    length, at = _read_varint(payload, 0, size)
    if at + length > size or length < 5:
        raise CodeError("bad", "the code is cut short")
    payload = payload[:at + length]
    if _crc10(payload) != _ALPHABET.index(crc_text[0]) << 5 | _ALPHABET.index(crc_text[1]):
        raise CodeError("bad", "the code's checksum is wrong: check for a typing mistake")
    end = len(payload)
    seed = int.from_bytes(payload[at:at + 4], "big")
    core, at = _read_varint(payload, at + 4, end)
    if core >> CORE_BITS:
        raise CodeError("newer", "this code uses an option of a newer randomizer: update the launcher")
    options = {}
    for bit, name in enumerate(CORE_FLAGS):
        options[name] = core >> bit & 1
    for name, first, width in CORE_FIELDS:
        options[name] = _unstep(name, core >> first & ((1 << width) - 1))
    while at < end:
        option_id, at = _read_varint(payload, at, end)
        if option_id not in EXTRAS:
            if option_id in _BY_ID:
                raise CodeError("bad", "the code holds option %d twice" % option_id)
            raise CodeError("newer", "this code uses an option of a newer randomizer: update the launcher")
        _, name, kind, _ = _BY_ID[option_id]
        if kind == "int":
            options[name], at = _read_varint(payload, at, end)
        else:
            count, at = _read_varint(payload, at, end)
            items = []
            for _ in range(count):
                a, at = _read_varint(payload, at, end)
                if kind == "pairs":
                    b, at = _read_varint(payload, at, end)
                    items.append((a, b * 5))
                else:
                    items.append(a)
            options[name] = tuple(items)
    try:
        norm = normalize(options)
        canonical = _text(seed, norm)
    except ValueError as e:
        raise CodeError("bad", "the code holds a value out of range: " + str(e))
    if canonical != text:  # padding bits, order, duplicates or a default written out: one world, one code
        raise CodeError("bad", "the code is not in its canonical form: check for a typing mistake")
    return seed, norm


def canonical(code):
    """The one spelling of a code, for folder names, caches and sharing."""
    return encode(*decode(code))


def label_code(code):
    """The code as it goes in the lobby label (lowercase; the label allows a-z 0-9 and '-')."""
    return canonical(code).lower()
