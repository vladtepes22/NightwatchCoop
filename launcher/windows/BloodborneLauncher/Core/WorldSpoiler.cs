using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// A world's spoiler in words: the "readable" part of randomizer_spoiler.json, which the generator writes with the game's
    /// own English names (areas, items, named enemies). Normal enemies have no name in the game, so they stay model numbers.
    /// Worlds made before the generator wrote it have none (Read returns null).
    /// </summary>
    public sealed record WorldSpoiler(
        IReadOnlyDictionary<string, string> Areas,
        IReadOnlyDictionary<string, string> Models,
        IReadOnlyList<WorldSpoiler.Enemy> Enemies,
        IReadOnlyList<WorldSpoiler.Boss> Bosses,
        IReadOnlyList<WorldSpoiler.Trade> Treasure,
        IReadOnlyList<WorldSpoiler.Trade> Shops,
        IReadOnlyList<WorldSpoiler.Trade> Drops,
        IReadOnlyList<WorldSpoiler.Trade> Starters,
        string? Error)
    {
        public sealed record Enemy(string Map, string From, string To, int Count);
        public sealed record Boss(string Map, string From, string To);

        /// <summary>Where (an area, a shop's name or the enemies that drop it), what was there and what is there now.</summary>
        public sealed record Trade(string Where, string Was, string Now);

        public const string FileName = "randomizer_spoiler.json";

        public static WorldSpoiler? Read(string worldFolder)
        {
            try
            {
                using var stream = File.OpenRead(Path.Combine(worldFolder, FileName));
                return Parse(JsonNode.Parse(stream));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
            {
                return null;
            }
        }

        public static WorldSpoiler? Parse(JsonNode? spoiler)
        {
            if (spoiler?["readable"] is not JsonObject r) return null;
            var areas = Map(r["areas"]);
            var models = Map(r["models"]);
            string Area(string stem) => areas.TryGetValue(stem, out var name) ? name : stem;
            var enemies = new List<Enemy>();
            if (r["enemies"] is JsonObject byMap)
                foreach (var (stem, list) in byMap)
                    foreach (var row in Rows(list))
                        enemies.Add(new Enemy(Area(stem), Str(row, 0), Str(row, 1), row.Count > 2 && row[2] is JsonValue v && v.TryGetValue<int>(out var n) ? n : 1));
            var bosses = Rows(r["bosses"]).Select(row => new Boss(Area(Str(row, 0)), Str(row, 1), Str(row, 2))).ToList();
            var treasure = Rows(r["treasure"]).Select(row => new Trade(Area(Str(row, 0)), Str(row, 1), Str(row, 2))).ToList();
            var shops = Rows(r["shops"]).Select(row => new Trade(ShopName(row[0] is JsonValue v && v.TryGetValue<int>(out var n) ? n : 0), Str(row, 1), Str(row, 2))).ToList();
            var drops = Rows(r["drops"]).Select(row => new Trade(
                row[0] is JsonArray who ? string.Join(", ", who.Select(m => ModelText(models, m?.GetValue<string>() ?? ""))) : "",
                Str(row, 1), Str(row, 2))).ToList();
            var starters = Rows(r["starters"]).Select(row => new Trade(L.T("Hunter's Dream"), Str(row, 0), Str(row, 1))).ToList();
            var error = r["error"] is JsonValue e && e.TryGetValue<string>(out var text) ? text : null;
            return new WorldSpoiler(areas, models, enemies, bosses, treasure, shops, drops, starters, error);
        }

        /// <summary>"Blood-starved Beast (c2090)" for a model the game names, else the model number.</summary>
        public static string ModelText(IReadOnlyDictionary<string, string> models, string model) =>
            models.TryGetValue(model, out var name) ? $"{name} ({model})" : model;

        /// <summary>The shuffled shops in generator order (shops.SHOPS): the Bath Messengers, the insight shop and the two
        /// chalice dungeon merchants.</summary>
        public static string ShopName(int number) => number switch
        {
            1 => L.T("Bath Messengers"),
            2 => L.T("Insight shop"),
            3 => L.T("Chalice merchant 1"),
            4 => L.T("Chalice merchant 2"),
            _ => L.T("Shop"),
        };

        private static Dictionary<string, string> Map(JsonNode? node) =>
            node is JsonObject o ? o.Where(p => p.Value is JsonValue).ToDictionary(p => p.Key, p => p.Value!.GetValue<string>()) : new Dictionary<string, string>();

        private static IEnumerable<JsonArray> Rows(JsonNode? node) =>
            node is JsonArray a ? a.OfType<JsonArray>().Where(row => row.Count >= 2) : Enumerable.Empty<JsonArray>();

        private static string Str(JsonArray row, int index) =>
            index < row.Count && row[index] is JsonValue v && v.TryGetValue<string>(out var s) ? s : "";
    }
}
