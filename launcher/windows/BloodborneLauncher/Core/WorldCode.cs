namespace BloodborneLauncher.Core
{
    public enum WorldCodeKind { Ok, Newer, Bad }

    /// <summary>A decoded world code: Ok carries the canonical spelling, the seed and every option (normalized, as
    /// seedcode.normalize gives them); Newer means "made by a newer randomizer", Bad "not a valid code".</summary>
    public sealed record WorldCodeResult(WorldCodeKind Kind, string? Canonical = null, uint Seed = 0,
        IReadOnlyDictionary<string, object>? Options = null);

    /// <summary>
    /// The randomizer's world code (schema 2), decoded in the launcher so the Join list can read a host's code without
    /// starting the generator. A port of tools/randomizer/seedcode.py decode; tools/randomizer/tests/r2_vectors.json is
    /// the contract both sides are tested against. Input is untrusted: anything that does not re-encode to the same
    /// characters is Bad, so one world has one code.
    /// </summary>
    public static class WorldCode
    {
        public const int Schema = 2, CoreBits = 22;
        private const string Alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

        private static readonly string[] CoreFlags = { "boss_swap", "treasure", "shops", "drops", "treasure_by_type", "starters",
            "boss_cutscenes", "chalice_materials", "rewards" };
        private static readonly (string Name, int First, int Width)[] CoreFields = { ("scaling", 9, 2), ("enemy_chance", 11, 5), ("headroom", 16, 6) };

        // The free-form options (id, name, kind, default), ids ascending; kind: i int, l list, p pairs.
        private static readonly (int Id, string Name, char Kind, long Default)[] Extras =
        {
            (2, "area_chance", 'p', 0), (3, "exclude", 'l', 0), (4, "oops_all", 'i', 0), (6, "fixed_effect", 'i', 0),
            (8, "map_models", 'i', 24), (9, "map_mb", 'i', 343), (10, "big_models", 'i', 2), (11, "variety", 'i', 0), (23, "starter_melee", 'i', 3), (24, "starter_guns", 'i', 2),
            (26, "witch_mix", 'i', 0), (27, "shadows_mix", 'i', 0), (28, "emissary_mix", 'i', 0),
            (29, "failures_mix", 'i', 0), (30, "bell_maidens", 'i', 0), (31, "chalice_bosses", 'i', 0), (32, "hard_rom", 'i', 0),
            (33, "hard_amygdala", 'i', 0), (34, "hard_ebrietas", 'i', 0), (35, "difficult_enemies", 'i', 0),
            (36, "second_stage", 'i', 0), (37, "away_from_home", 'i', 0), (38, "free_poses", 'i', 0),
            (39, "starter_armor", 'i', 0),
        };
        // seedcode.BOOL_OPTIONS_R2
        private static readonly string[] Round2Bools = { "witch_mix", "shadows_mix", "emissary_mix", "failures_mix",
            "chalice_bosses", "hard_rom", "hard_amygdala", "hard_ebrietas", "difficult_enemies", "second_stage",
            "away_from_home", "free_poses", "starter_armor" };
        private static readonly HashSet<int> KnownIds = new() { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 22, 23, 24,
            26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37 };

        private sealed class BadCode : Exception { }

        public static WorldCodeResult Decode(string? code)
        {
            try
            {
                return DecodeOrThrow(code ?? "");
            }
            catch (BadCode)
            {
                return new WorldCodeResult(WorldCodeKind.Bad);
            }
            catch (OverflowException)
            {
                return new WorldCodeResult(WorldCodeKind.Bad);
            }
        }

        private static WorldCodeResult DecodeOrThrow(string code)
        {
            var text = code.Trim().ToUpperInvariant();
            var dash = text.IndexOf('-');
            var head = dash < 0 ? text : text[..dash];
            var rest = dash < 0 ? "" : text[(dash + 1)..];
            if (head.Length < 2 || head[0] != 'R' || !head[1..].All(c => c is >= '0' and <= '9') || head.Length > 6) throw new BadCode();
            var schema = int.Parse(head[1..]);
            if (schema > Schema) return new WorldCodeResult(WorldCodeKind.Newer);
            if (schema != Schema) throw new BadCode();
            text = rest.Replace("-", "").Replace('O', '0').Replace('I', '1').Replace('L', '1');
            if (text.Length < 3 || text.Any(c => Alphabet.IndexOf(c) < 0)) throw new BadCode();

            var body = text[..^2];
            var size = body.Length * 5 / 8;
            var payload = Unbase32(body, size);
            var at = 0;
            var length = ReadVarint(payload, ref at, size);
            if ((ulong)at + length > (ulong)size || length < 5) throw new BadCode();
            var end = at + (int)length;
            payload = payload[..end];
            if (Crc10(payload) != (Alphabet.IndexOf(text[^2]) << 5 | Alphabet.IndexOf(text[^1]))) throw new BadCode();

            var seed = (uint)(payload[at] << 24 | payload[at + 1] << 16 | payload[at + 2] << 8 | payload[at + 3]);
            at += 4;
            var core = ReadVarint(payload, ref at, end);
            if (core >> CoreBits != 0) return new WorldCodeResult(WorldCodeKind.Newer);
            var options = Defaults();
            for (var bit = 0; bit < CoreFlags.Length; bit++) options[CoreFlags[bit]] = (long)(core >> bit & 1);
            foreach (var (name, first, width) in CoreFields)
                options[name] = Unstep(name, (long)(core >> first & ((1UL << width) - 1)));

            while (at < end)
            {
                var id = ReadVarint(payload, ref at, end);
                var extra = Extras.FirstOrDefault(e => (ulong)e.Id == id);
                if (extra.Name == null)
                {
                    if (id <= int.MaxValue && KnownIds.Contains((int)id)) throw new BadCode();
                    return new WorldCodeResult(WorldCodeKind.Newer);
                }
                if (extra.Kind == 'i')
                {
                    options[extra.Name] = checked((long)ReadVarint(payload, ref at, end));
                    continue;
                }
                var count = ReadVarint(payload, ref at, end);
                if (count > (ulong)(end - at)) throw new BadCode(); // each item takes a byte at least
                if (extra.Kind == 'l')
                {
                    var items = new long[count];
                    for (var i = 0UL; i < count; i++) items[i] = checked((long)ReadVarint(payload, ref at, end));
                    options[extra.Name] = items;
                }
                else
                {
                    var pairs = new (long, long)[count];
                    for (var i = 0UL; i < count; i++)
                        pairs[i] = (checked((long)ReadVarint(payload, ref at, end)), checked((long)ReadVarint(payload, ref at, end) * 5));
                    options[extra.Name] = pairs;
                }
            }

            Normalize(options);
            var canonical = Text(seed, options);
            if (canonical != text) throw new BadCode(); // padding bits, order, duplicates or a default written out
            return new WorldCodeResult(WorldCodeKind.Ok, "R2-" + string.Join("-", Chunk(canonical)), seed, options);
        }

        /// <summary>Every option at its default (core flags off, 100 % of enemies, the area's scaling, variety 100 %), in the
        /// shapes Decode gives: long, long[] for lists, (long, long)[] for area pairs.</summary>
        public static Dictionary<string, object> Defaults()
        {
            var options = new Dictionary<string, object>();
            foreach (var flag in CoreFlags) options[flag] = 0L;
            options["scaling"] = 0L;
            options["enemy_chance"] = 100L;
            options["headroom"] = 100L;
            foreach (var e in Extras)
                options[e.Name] = e.Kind == 'i' ? e.Default : e.Kind == 'l' ? Array.Empty<long>() : Array.Empty<(long, long)>();
            return options;
        }

        /// <summary>The canonical code for a seed and options (seedcode.encode), or null when the options are out of range.
        /// Options in Defaults' shapes; missing ones are the defaults. Decode(Encode(s, o)) gives o back, normalized.</summary>
        public static string? Encode(uint seed, IReadOnlyDictionary<string, object> options)
        {
            var o = Defaults();
            foreach (var (key, value) in options)
                if (o.ContainsKey(key)) o[key] = value;
            try
            {
                Normalize(o);
            }
            catch (Exception e) when (e is BadCode or InvalidCastException)
            {
                return null;
            }
            return "R2-" + string.Join("-", Chunk(Text(seed, o)));
        }

        /// <summary>The code as it goes in a lobby label (lowercase), or null when it is not a valid code.</summary>
        public static string? LabelForm(string? code) => Decode(code) is { Kind: WorldCodeKind.Ok, Canonical: { } c } ? c.ToLowerInvariant() : null;

        private static IEnumerable<string> Chunk(string text)
        {
            for (var i = 0; i < text.Length; i += 5) yield return text.Substring(i, Math.Min(5, text.Length - i));
        }

        private static long Unstep(string name, long step) => name == "headroom" ? 100 + step * 5 : name == "enemy_chance" ? step * 5 : step;

        private static long Step(string name, long value) => name == "headroom" ? (value - 100) / 5 : name == "enemy_chance" ? value / 5 : value;

        // seedcode.normalize: lists sorted (exclude without duplicates, area pairs by area, the last percent kept), ranges checked.
        private static void Normalize(Dictionary<string, object> o)
        {
            o["exclude"] = ((long[])o["exclude"]).Distinct().OrderBy(x => x).ToArray();
            var areas = new Dictionary<long, long>();
            foreach (var (area, percent) in (ValueTuple<long, long>[])o["area_chance"]) areas[area] = percent;
            o["area_chance"] = areas.OrderBy(p => p.Key).Select(p => (p.Key, p.Value)).ToArray();
            long I(string k) => (long)o[k];
            if (I("enemy_chance") is < 0 or > 100 || I("enemy_chance") % 5 != 0) throw new BadCode();
            if (((ValueTuple<long, long>[])o["area_chance"]).Any(p => p.Item2 is < 0 or > 100 || p.Item2 % 5 != 0)) throw new BadCode();
            if (I("scaling") is < 0 or > 2) throw new BadCode();
            if (I("headroom") is < 100 or > 300 || I("headroom") % 5 != 0) throw new BadCode();
            if (CoreFlags.Any(f => I(f) is not (0 or 1))) throw new BadCode();
            if ((I("scaling") == 2) != (I("fixed_effect") != 0)) throw new BadCode();
            if (I("starter_melee") is < 1 or > 5 || I("starter_guns") is < 1 or > 3) throw new BadCode();
            if (I("big_models") is < 0 or > 24) throw new BadCode();
            if (Round2Bools.Any(f => I(f) is not (0 or 1)) || I("bell_maidens") is < 0 or > 2) throw new BadCode();
        }

        // seedcode._text: the code's characters without prefix or dashes.
        private static string Text(uint seed, Dictionary<string, object> o)
        {
            ulong core = 0;
            for (var bit = 0; bit < CoreFlags.Length; bit++) core |= (ulong)(long)o[CoreFlags[bit]] << bit;
            foreach (var (name, first, _) in CoreFields) core |= (ulong)Step(name, (long)o[name]) << first;
            var body = new List<byte> { (byte)(seed >> 24), (byte)(seed >> 16), (byte)(seed >> 8), (byte)seed };
            Varint(body, core);
            foreach (var e in Extras)
            {
                switch (o[e.Name])
                {
                    case long v when e.Kind == 'i':
                        if (v == e.Default) break;
                        Varint(body, (ulong)e.Id); Varint(body, (ulong)v);
                        break;
                    case long[] list:
                        if (list.Length == 0) break;
                        Varint(body, (ulong)e.Id); Varint(body, (ulong)list.Length);
                        foreach (var item in list) Varint(body, (ulong)item);
                        break;
                    case ValueTuple<long, long>[] pairs:
                        if (pairs.Length == 0) break;
                        Varint(body, (ulong)e.Id); Varint(body, (ulong)pairs.Length);
                        foreach (var (area, percent) in pairs) { Varint(body, (ulong)area); Varint(body, (ulong)(percent / 5)); }
                        break;
                }
            }
            var payload = new List<byte>();
            Varint(payload, (ulong)body.Count);
            payload.AddRange(body);
            var crc = Crc10(payload.ToArray());
            return Base32(payload.ToArray()) + Alphabet[crc >> 5] + Alphabet[crc & 31];
        }

        private static void Varint(List<byte> into, ulong value)
        {
            while (true)
            {
                var b = (byte)(value & 0x7f);
                value >>= 7;
                if (value == 0) { into.Add(b); return; }
                into.Add((byte)(b | 0x80));
            }
        }

        private static ulong ReadVarint(byte[] data, ref int at, int end)
        {
            ulong value = 0;
            for (var shift = 0; ; shift += 7)
            {
                if (at >= end || shift > 63) throw new BadCode();
                var b = data[at++];
                var part = (ulong)(b & 0x7f);
                if (shift > 0 && part >> (64 - shift) != 0) throw new BadCode(); // above 64 bits
                value |= part << shift;
                if ((b & 0x80) == 0) return value;
            }
        }

        private static int Crc10(byte[] data)
        {
            var crc = 0xffff;
            foreach (var b in data)
            {
                crc ^= b << 8;
                for (var i = 0; i < 8; i++) crc = (crc & 0x8000) != 0 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
            }
            return crc & 0x3ff;
        }

        private static string Base32(byte[] data)
        {
            var n = (data.Length * 8 + 4) / 5;
            var chars = new char[n];
            for (var i = 0; i < n; i++)
            {
                var v = 0;
                for (var j = 0; j < 5; j++)
                {
                    var bit = i * 5 + j;
                    var set = bit < data.Length * 8 && (data[bit / 8] >> (7 - bit % 8) & 1) != 0;
                    v = v << 1 | (set ? 1 : 0);
                }
                chars[i] = Alphabet[v];
            }
            return new string(chars);
        }

        private static byte[] Unbase32(string text, int size)
        {
            var bytes = new byte[size];
            for (var bit = 0; bit < size * 8; bit++)
            {
                var v = Alphabet.IndexOf(text[bit / 5]);
                if ((v >> (4 - bit % 5) & 1) != 0) bytes[bit / 8] |= (byte)(1 << (7 - bit % 8));
            }
            return bytes;
        }
    }
}
