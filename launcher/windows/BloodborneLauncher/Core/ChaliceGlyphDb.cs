using System.Globalization;
using System.IO;
using System.Net.Http;
using System.Text;
using System.Text.Json;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Community Chalice glyphs (bbport.ini chalice_glyphdb, visy 0.8.1): Noxde's list "Add dungeons to Bloodborne save"
    /// (Nexus mod 121). Only while the switch is on, the gist is downloaded from GitHub's API, strictly validated (bad
    /// entries dropped) and cached in &lt;user dir&gt;/community/chalice_glyphs.json, which the engine reads
    /// (party_chalice_glyphdb.h). Nothing of the list is in the repository or a release. The records carry the creators'
    /// names: they are zeroed on the way in and never shown or logged; the log gets one status line, never list content.
    /// </summary>
    public static class ChaliceGlyphDb
    {
        public const string GistUrl = "https://api.github.com/gists/a29f699f4175bf315d9bd4baeebefb66";
        public static readonly string[] Categories = { "farming", "equipment", "bloodgems", "testing" };
        public const int RecordBytes = 124;   // one altar slot of the save (0x7c bytes)
        public const int NameAt = 92;         // the creator's name field starts here
        public const int DescMax = 200;
        public const long MaxBytes = 4L << 20;
        public static readonly TimeSpan MaxAge = TimeSpan.FromDays(7);

        public sealed record Glyph(string Id, string Cat, string Desc, string Rec);

        /// <summary>The cache document: {"version":1,"glyphs":[{"glyph","cat","desc","rec"}]}.</summary>
        public sealed record Cache(List<Glyph> Glyphs)
        {
            public string ToJson()
            {
                using var stream = new MemoryStream();
                using (var w = new Utf8JsonWriter(stream))
                {
                    w.WriteStartObject();
                    w.WriteNumber("version", 1);
                    w.WriteStartArray("glyphs");
                    foreach (var g in Glyphs)
                    {
                        w.WriteStartObject();
                        w.WriteString("glyph", g.Id);
                        w.WriteString("cat", g.Cat);
                        w.WriteString("desc", g.Desc);
                        w.WriteString("rec", g.Rec);
                        w.WriteEndObject();
                    }
                    w.WriteEndArray();
                    w.WriteEndObject();
                }
                return Encoding.UTF8.GetString(stream.ToArray());
            }
        }

        public static string CachePath(string userDir) => Path.Combine(userDir, "community", "chalice_glyphs.json");

        /// <summary>The record as 248 hex digits: a list of 124 byte values ("0x1d" strings or ints) or a hex string. "" if bad.</summary>
        private static string Record(JsonElement value)
        {
            if (value.ValueKind == JsonValueKind.String)
            {
                var text = (value.GetString() ?? "").Replace("0x", "").Replace(" ", "").Replace(",", "");
                return text.Length == RecordBytes * 2 && text.All(Uri.IsHexDigit) ? text.ToLowerInvariant() : "";
            }
            if (value.ValueKind != JsonValueKind.Array || value.GetArrayLength() != RecordBytes) return "";
            var sb = new StringBuilder();
            foreach (var item in value.EnumerateArray())
            {
                int number;
                if (item.ValueKind == JsonValueKind.Number) { if (!item.TryGetInt32(out number)) return ""; }
                else if (item.ValueKind == JsonValueKind.String)
                {
                    var s = item.GetString() ?? "";
                    if (s.Length < 3 || s.Length > 4 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X') || !s.Skip(2).All(Uri.IsHexDigit)) return "";
                    number = int.Parse(s.AsSpan(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
                }
                else return "";
                if (number < 0 || number > 255) return "";
                sb.Append(number.ToString("x2", CultureInfo.InvariantCulture));
            }
            return sb.ToString();
        }

        private static bool GlyphOk(string g) => g.Length is 8 or 9 && g.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'z');

        private static string Fold(string desc)
        {
            var kept = new string(desc.Where(c => !char.IsControl(c) || char.IsWhiteSpace(c)).ToArray());
            return string.Join(" ", kept.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries));
        }

        /// <summary>A long description is cut (the engine takes at most 200 bytes), the entry stays.</summary>
        private static string Cut(string desc)
        {
            if (desc.Length <= DescMax && Encoding.UTF8.GetByteCount(desc) <= DescMax) return desc;
            var n = Math.Min(desc.Length, DescMax - 3);
            while (n > 0 && (Encoding.UTF8.GetByteCount(desc.AsSpan(0, n)) > DescMax - 3 || char.IsHighSurrogate(desc[n - 1]))) n--;
            return desc[..n].TrimEnd() + "...";
        }

        /// <summary>The cache and the dropped count from the gist's dungeons.json (already parsed): {category: [{glyph, bytes, desc}]}. Never throws.</summary>
        public static (Cache Cache, int Dropped) Validate(JsonElement dungeons)
        {
            var glyphs = new List<Glyph>();
            var seen = new HashSet<string>();
            var dropped = 0;
            if (dungeons.ValueKind == JsonValueKind.Object)
            {
                foreach (var cat in Categories)
                {
                    if (!dungeons.TryGetProperty(cat, out var rows) || rows.ValueKind != JsonValueKind.Array) continue;
                    foreach (var row in rows.EnumerateArray())
                    {
                        var isRow = row.ValueKind == JsonValueKind.Object;
                        var glyph = isRow && row.TryGetProperty("glyph", out var g) && g.ValueKind == JsonValueKind.String ? g.GetString()!.Trim().ToLowerInvariant() : "";
                        var desc = isRow && row.TryGetProperty("desc", out var d) && d.ValueKind == JsonValueKind.String ? Fold(d.GetString()!) : "";
                        var rec = isRow && row.TryGetProperty("bytes", out var b) ? Record(b) : "";
                        if (!GlyphOk(glyph) || seen.Contains(glyph) || rec.Length == 0 || desc.Length == 0) { dropped++; continue; }
                        seen.Add(glyph);
                        // Bytes 92..123 are the creator's name: never kept.
                        rec = rec[..(NameAt * 2)] + new string('0', (RecordBytes - NameAt) * 2);
                        glyphs.Add(new Glyph(glyph, cat, Cut(desc), rec));
                    }
                }
            }
            return (new Cache(glyphs), dropped);
        }

        /// <summary>The cache and dropped count from the gist API's JSON text; null when it is not that document (or truncated).</summary>
        public static (Cache Cache, int Dropped)? FromGist(string text)
        {
            try
            {
                using var doc = JsonDocument.Parse(text);
                var content = doc.RootElement.GetProperty("files").GetProperty("dungeons.json");
                if (content.TryGetProperty("truncated", out var t) && t.ValueKind == JsonValueKind.True) return null;
                if (!content.TryGetProperty("content", out var c) || c.ValueKind != JsonValueKind.String) return null;
                using var dungeons = JsonDocument.Parse(c.GetString()!);
                return Validate(dungeons.RootElement);
            }
            catch (Exception e) when (e is JsonException or KeyNotFoundException or InvalidOperationException or ArgumentException)
            {
                return null;
            }
        }

        private static readonly HttpClient Http = MakeClient();

        private static HttpClient MakeClient()
        {
            var client = new HttpClient { Timeout = TimeSpan.FromSeconds(15) };
            client.DefaultRequestHeaders.UserAgent.ParseAdd("nightwatch-launcher");
            client.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
            return client;
        }

        /// <summary>The gist API's JSON text (HTTPS, at most 4 MB). Throws on a network error or an oversized reply.</summary>
        public static async Task<string> FetchAsync()
        {
            using var reply = await Http.GetAsync(GistUrl, HttpCompletionOption.ResponseHeadersRead).ConfigureAwait(false);
            reply.EnsureSuccessStatusCode();
            await using var body = await reply.Content.ReadAsStreamAsync().ConfigureAwait(false);
            using var memory = new MemoryStream();
            var buffer = new byte[81920];
            int n;
            while ((n = await body.ReadAsync(buffer).ConfigureAwait(false)) > 0)
            {
                memory.Write(buffer, 0, n);
                if (memory.Length > MaxBytes) throw new InvalidDataException("reply too large");
            }
            return Encoding.UTF8.GetString(memory.ToArray());
        }

        private static int CachedCount(string path)
        {
            try
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(path));
                return doc.RootElement.TryGetProperty("glyphs", out var g) && g.ValueKind == JsonValueKind.Array ? g.GetArrayLength() : 0;
            }
            catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException) { return 0; }
        }

        /// <summary>Refreshes the cache when it is missing or older than a week (or force). Returns a one-line status without any
        /// list content: "fresh N", "cached N" (+ " (M dropped)"), "offline, cached N", "offline, none" or "unusable". Never throws.</summary>
        public static async Task<string> RefreshAsync(string userDir, bool force = false, Func<Task<string>>? fetch = null, DateTime? now = null)
        {
            try
            {
                var path = CachePath(userDir);
                var clock = now ?? DateTime.UtcNow;
                try
                {
                    if (!force && File.Exists(path) && clock - File.GetLastWriteTimeUtc(path) < MaxAge && CachedCount(path) is > 0 and var count)
                        return $"fresh {count}";
                }
                catch (IOException) { }
                (Cache Cache, int Dropped)? result = null;
                try { result = FromGist(await (fetch ?? FetchAsync)().ConfigureAwait(false)); }
                catch (Exception) { /* offline, too large, bad reply */ }
                if (result is not { } r)
                {
                    var n = CachedCount(path);
                    return n > 0 ? $"offline, cached {n}" : "offline, none";
                }
                if (r.Cache.Glyphs.Count == 0) return "unusable";
                try
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                    var tmp = path + ".tmp";
                    File.WriteAllText(tmp, r.Cache.ToJson(), new UTF8Encoding(false));
                    File.Move(tmp, path, overwrite: true);
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return "unusable"; }
                return $"cached {r.Cache.Glyphs.Count}" + (r.Dropped > 0 ? $" ({r.Dropped} dropped)" : "");
            }
            catch (Exception e)
            {
                return $"failed ({e.GetType().Name})";
            }
        }

        private static int _busy;

        /// <summary>In the background, never blocking or failing the caller; logs only the status line. Call only while chalice_glyphdb is on.</summary>
        public static void StartRefresh(string userDir, Action<string> log, bool force = false)
        {
            if (Interlocked.Exchange(ref _busy, 1) == 1) return;
            _ = Task.Run(async () =>
            {
                try { log("Community Chalice glyphs: " + await RefreshAsync(userDir, force).ConfigureAwait(false)); }
                catch (Exception) { /* a launcher must never fail on this */ }
                finally { Interlocked.Exchange(ref _busy, 0); }
            });
        }
    }
}
