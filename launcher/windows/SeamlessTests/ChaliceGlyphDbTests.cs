using System.IO;
using System.Net.Http;
using System.Text.Json;
using BloodborneLauncher.Core;

/// <summary>Community Chalice glyph list (ChaliceGlyphDb): validation, the gist document, the cache file and its refresh with a fake fetch.
/// Synthetic data only, no network.</summary>
static class ChaliceGlyphDbTests
{
    private static void Check(bool ok, string what)
    {
        if (!ok) throw new InvalidOperationException("Chalice glyph list: " + what);
    }

    private static string ByteList(int fill = 0x11) =>
        "[" + string.Join(",", Enumerable.Range(0, 124).Select(i => i % 2 == 0 ? $"\"0x{fill:x2}\"" : (fill + 1).ToString())) + "]";

    private static string Hex(int fill = 0x22) => string.Concat(Enumerable.Repeat(fill.ToString("x2"), 124));

    private static (ChaliceGlyphDb.Cache Cache, int Dropped) Validate(string json)
    {
        using var doc = JsonDocument.Parse(json);
        return ChaliceGlyphDb.Validate(doc.RootElement);
    }

    private static string Gist(string dungeons, bool truncated = false) =>
        JsonSerializer.Serialize(new { files = new Dictionary<string, object> { ["dungeons.json"] = new { truncated, content = dungeons } } });

    private static string Row(string glyph, string bytes, string desc) =>
        "{\"glyph\":\"" + glyph + "\",\"bytes\":" + bytes + ",\"desc\":\"" + desc + "\"}";

    private static string Quoted(string hex) => "\"" + hex + "\"";

    public static void Run()
    {
        // Both record forms stay; the name bytes 92..123 are zeroed; the glyph is lower-cased.
        var (cache, dropped) = Validate("{\"farming\":[" + Row("ABCD1234", ByteList(), "A good one") + "],\"equipment\":[" + Row("zzzz99999", Quoted(Hex()), "Hex form") + "]}");
        Check(dropped == 0 && cache.Glyphs.Count == 2, "good entries not kept");
        Check(cache.Glyphs[0].Id == "abcd1234" && cache.Glyphs[0].Cat == "farming" && cache.Glyphs[1].Cat == "equipment", "glyph or category wrong");
        Check(cache.Glyphs[0].Rec.Length == 248 && cache.Glyphs[0].Rec[..4] == "1112" && cache.Glyphs[1].Rec[..4] == "2222", "record not read");
        foreach (var g in cache.Glyphs)
            Check(g.Rec[184..] == new string('0', 64) && g.Rec[..184] != new string('0', 184), "name bytes not zeroed or the rest lost");

        // Bad glyph, wrong byte count, empty description, duplicate: dropped, the rest stays.
        (cache, dropped) = Validate("{\"farming\":[" + string.Join(",",
                Row("short", Quoted(Hex()), "x"),
                Row("goodglyph", "[1,2,3]", "x"),
                Row("goodglyph", Quoted(Hex()), "   "),
                Row("goodglyph", Quoted(Hex()), "fine"),
                Row("GOODGLYPH", Quoted(Hex()), "again"),
                Row("has space", Quoted(Hex()), "x"),
                "\"not an object\"") + "]," +
            "\"testing\":[" + Row("other123", Quoted(Hex(0xab)), "kept") + "]," +
            "\"unknown\":[" + Row("ignored12", Quoted(Hex()), "x") + "]}");
        Check(cache.Glyphs.Count == 2 && dropped == 6 && cache.Glyphs[0].Id == "goodglyph" && cache.Glyphs[1].Id == "other123", $"dropping wrong: {cache.Glyphs.Count} kept, {dropped} dropped");
        var (outOfRange, _) = Validate("{\"farming\":[" + Row("range123", "[" + string.Join(",", Enumerable.Repeat("256", 124)) + "]", "x") + "]}");
        Check(outOfRange.Glyphs.Count == 0, "a byte above 255 accepted");
        Check(Validate("[]").Cache.Glyphs.Count == 0 && Validate("{}").Dropped == 0, "a wrong document did not give an empty cache");

        // A long description is cut to 200 with "...", line breaks fold to spaces, control characters go.
        (cache, _) = Validate("{\"farming\":[" + Row("longdesc1", Quoted(Hex()), new string('a', 300)) + "," +
            Row("folded123", Quoted(Hex()), "one\\r\\ntwo\\n\\n three\\u0001\\tfour") + "]}");
        Check(cache.Glyphs.Count == 2 && cache.Glyphs[0].Desc.Length == 200 && cache.Glyphs[0].Desc.EndsWith("..."), "long description not cut");
        Check(cache.Glyphs[1].Desc == "one two three four", "description not folded: " + cache.Glyphs[1].Desc);
        (cache, _) = Validate("{\"farming\":[" + Row("wide12345", Quoted(Hex()), new string('é', 150)) + "]}");
        Check(System.Text.Encoding.UTF8.GetByteCount(cache.Glyphs[0].Desc) <= 200, "a long non-ASCII description is over 200 bytes");

        // The gist document: truncated or wrong ones are unusable.
        var dungeons = "{\"farming\":[" + Row("abcd1234", Quoted(Hex()), "d") + "]}";
        Check(ChaliceGlyphDb.FromGist(Gist(dungeons)) is { } ok && ok.Cache.Glyphs.Count == 1, "gist not read");
        Check(ChaliceGlyphDb.FromGist(Gist(dungeons, truncated: true)) == null, "a truncated gist was used");
        Check(ChaliceGlyphDb.FromGist("{\"files\":{}}") == null && ChaliceGlyphDb.FromGist("not json") == null &&
              ChaliceGlyphDb.FromGist("{\"files\":{\"dungeons.json\":{\"content\":5}}}") == null &&
              ChaliceGlyphDb.FromGist(Gist("not json")) == null, "a wrong document was used");

        // The cache JSON is what the engine reads.
        using (var written = JsonDocument.Parse(ChaliceGlyphDb.FromGist(Gist(dungeons))!.Value.Cache.ToJson()))
        {
            var root = written.RootElement;
            var entry = root.GetProperty("glyphs")[0];
            Check(root.GetProperty("version").GetInt32() == 1 && entry.GetProperty("glyph").GetString() == "abcd1234" &&
                  entry.GetProperty("cat").GetString() == "farming" && entry.GetProperty("desc").GetString() == "d" &&
                  entry.GetProperty("rec").GetString()!.Length == 248, "cache JSON shape");
        }

        // Refresh with a fake fetch: writes the file; a fresh file is not fetched again; offline keeps the cache.
        var dir = Path.Combine(Path.GetTempPath(), "nw-glyphdb-" + Guid.NewGuid().ToString("N")[..8]);
        try
        {
            var fetches = 0;
            var listed = "{\"farming\":[" + Row("abcd1234", Quoted(Hex()), "d") + "," + Row("bad", Quoted(Hex()), "d") + "]}";
            Task<string> Fetch() { fetches++; return Task.FromResult(Gist(listed)); }
            var status = ChaliceGlyphDb.RefreshAsync(dir, fetch: Fetch).GetAwaiter().GetResult();
            Check(status == "cached 1 (1 dropped)" && fetches == 1, "refresh status: " + status);
            var path = ChaliceGlyphDb.CachePath(dir);
            Check(File.Exists(path) && !File.Exists(path + ".tmp") && path.EndsWith(Path.Combine("community", "chalice_glyphs.json")), "cache file missing or tmp left");
            status = ChaliceGlyphDb.RefreshAsync(dir, fetch: Fetch).GetAwaiter().GetResult();
            Check(status == "fresh 1" && fetches == 1, "a fresh file was fetched again: " + status);
            status = ChaliceGlyphDb.RefreshAsync(dir, force: true, fetch: () => throw new HttpRequestException("offline")).GetAwaiter().GetResult();
            Check(status == "offline, cached 1", "offline with a cache: " + status);
            status = ChaliceGlyphDb.RefreshAsync(dir, fetch: Fetch, now: DateTime.UtcNow.AddDays(8)).GetAwaiter().GetResult();
            Check(status.StartsWith("cached 1") && fetches == 2, "an old file was not refreshed: " + status);
            Check(ChaliceGlyphDb.RefreshAsync(Path.Combine(dir, "none"), fetch: () => Task.FromResult("not json")).GetAwaiter().GetResult() == "offline, none", "offline without a cache");
            Check(ChaliceGlyphDb.RefreshAsync(Path.Combine(dir, "empty"), fetch: () => Task.FromResult(Gist("{}"))).GetAwaiter().GetResult() == "unusable", "an empty list was cached");
        }
        finally { try { Directory.Delete(dir, recursive: true); } catch (IOException) { } }
    }
}
