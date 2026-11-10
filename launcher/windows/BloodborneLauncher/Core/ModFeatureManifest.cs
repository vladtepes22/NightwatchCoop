using System.IO;
using System.Text;
using System.Text.Json;

namespace BloodborneLauncher.Core
{
    /// <summary>A mod's nightwatch.json, read with the engine's own rules (engine/start/features.h ParseManifest / Refusal and
    /// mods.cpp CheckManifests), so the Mods page marks exactly the mods bb-start will not load.</summary>
    public sealed record ModFeatureManifest(string Id, string Name, string Version, string Author, string Requires,
        IReadOnlyList<string> Features)
    {
        public const string FileName = "nightwatch.json";
        private const int MaxBytes = 1 << 20;

        /// <summary>The features this Nightwatch offers (engine/start/features.h Known(); a manifest naming any other is refused).</summary>
        public static readonly IReadOnlyList<string> KnownFeatures = new[] { "hit_effects", "weapon_clip_groups", "sounds" };

        public enum Problem { None, Bad, TooNew, UnknownFeature }

        /// <summary>What the engine does with a mod: no manifest (loads), fine (loads) or refused. `Detail`: the engine's error text
        /// (Bad), the `requires` text (TooNew) or the feature name (UnknownFeature).</summary>
        public sealed record Verdict(ModFeatureManifest? Manifest, Problem Problem, string Detail)
        {
            public bool HasManifest => Manifest != null || Problem == Problem.Bad;
            public bool Refused => Problem != Problem.None;
        }

        /// <summary>"X.Y.Z" or "X.Y" with an optional "-suffix" / "+suffix" (ignored); false for anything else.</summary>
        public static bool TryParseVersion(string text, out System.Version version)
        {
            version = new System.Version(0, 0, 0);
            var cut = text.IndexOfAny(new[] { '-', '+' });
            if (cut >= 0) text = text[..cut];
            var parts = text.Split('.');
            if (parts.Length is < 2 or > 3) return false;
            var n = new int[3];
            for (var i = 0; i < parts.Length; i++)
            {
                var p = parts[i];
                if (p.Length == 0 || p.Any(c => c < '0' || c > '9')) return false;
                var t = p.TrimStart('0');
                if (t.Length > 5) return false; // the engine refuses a part above 99999
                n[i] = t.Length == 0 ? 0 : int.Parse(t);
            }
            version = new System.Version(n[0], n[1], n[2]);
            return true;
        }

        /// <summary>features.h ParseManifest: the manifest, or null with the engine's error text.</summary>
        public static ModFeatureManifest? Parse(byte[] bytes, out string error)
        {
            error = "";
            if (bytes.Length > MaxBytes)
            {
                error = FileName + " is larger than 1 MiB";
                return null;
            }
            var span = bytes.AsSpan();
            if (span.Length >= 3 && span[0] == 0xEF && span[1] == 0xBB && span[2] == 0xBF) span = span[3..];
            JsonDocument doc;
            try
            {
                doc = JsonDocument.Parse(span.ToArray());
            }
            catch (JsonException)
            {
                error = FileName + " is not a JSON object";
                return null;
            }
            var err = "";
            using (doc)
            {
                var j = doc.RootElement;
                if (j.ValueKind != JsonValueKind.Object)
                {
                    err = FileName + " is not a JSON object";
                    { error = err; return null; }
                }
                bool Text(string key, bool required, out string value)
                {
                    value = "";
                    if (!j.TryGetProperty(key, out var it))
                    {
                        if (required) err = $"{FileName} has no \"{key}\"";
                        return !required;
                    }
                    if (it.ValueKind != JsonValueKind.String || (required && it.GetString()!.Length == 0))
                    {
                        err = $"{FileName}: \"{key}\" is not a{(required ? " non-empty" : "")} string";
                        return false;
                    }
                    value = it.GetString()!;
                    return true;
                }
                if (!Text("id", true, out var id) || !Text("requires", true, out var requires) || !Text("name", false, out var name)
                    || !Text("version", false, out var version) || !Text("author", false, out var author))
                    { error = err; return null; }
                if (!TryParseVersion(requires, out _))
                {
                    err = $"{FileName}: \"requires\" is not a version like 0.3.0: {requires}";
                    { error = err; return null; }
                }
                var features = new List<string>();
                if (j.TryGetProperty("features", out var list))
                {
                    if (list.ValueKind != JsonValueKind.Array)
                    {
                        err = $"{FileName}: \"features\" is not a list";
                        { error = err; return null; }
                    }
                    foreach (var f in list.EnumerateArray())
                    {
                        if (f.ValueKind != JsonValueKind.String || f.GetString()!.Length == 0)
                        {
                            err = $"{FileName}: \"features\" holds something other than a feature name";
                            { error = err; return null; }
                        }
                        features.Add(f.GetString()!);
                    }
                }
                return new ModFeatureManifest(id, name, version, author, requires, features);
            }
        }

        /// <summary>features.h Refusal: what stops `nightwatch` from loading this mod (TooNew first, then the first unknown feature).</summary>
        public Verdict Judge(System.Version nightwatch)
        {
            if (!TryParseVersion(Requires, out var need)) return new Verdict(this, Problem.Bad, Requires);
            if (new System.Version(nightwatch.Major, nightwatch.Minor, Math.Max(0, nightwatch.Build)) < need)
                return new Verdict(this, Problem.TooNew, Requires);
            foreach (var f in Features)
                if (!KnownFeatures.Contains(f)) return new Verdict(this, Problem.UnknownFeature, f);
            return new Verdict(this, Problem.None, "");
        }

        /// <summary>Reads `modFolder`/nightwatch.json (the engine looks in the mod folder itself) and judges it for `nightwatch`.
        /// No file: no manifest, the mod loads.</summary>
        public static Verdict Read(string modFolder, System.Version nightwatch)
        {
            var file = Path.Combine(modFolder, FileName);
            try
            {
                if (!File.Exists(file)) return new Verdict(null, Problem.None, "");
                var manifest = Parse(File.ReadAllBytes(file), out var error);
                return manifest?.Judge(nightwatch) ?? new Verdict(null, Problem.Bad, error);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                return new Verdict(null, Problem.Bad, "cannot read " + FileName + ": " + e.Message);
            }
        }

        /// <summary>This launcher's Nightwatch.</summary>
        public static Verdict Read(string modFolder) => Read(modFolder, Updater.Current);
    }
}
