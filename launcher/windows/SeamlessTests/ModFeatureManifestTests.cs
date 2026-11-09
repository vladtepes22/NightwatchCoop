using System.IO;
using System.Text;
using BloodborneLauncher.Core;

/// <summary>The Mods page's reading of nightwatch.json: the engine's rules (engine/start/features.h, mods.cpp CheckManifests).</summary>
internal static class ModFeatureManifestTests
{
    private static void Check(bool ok, string what)
    {
        if (!ok) throw new InvalidOperationException(what);
    }

    private static ModFeatureManifest.Verdict Judge(string json, string nightwatch = "0.3.0")
    {
        var tmp = Path.Combine(Path.GetTempPath(), "nw-manifest-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(tmp);
        try
        {
            File.WriteAllBytes(Path.Combine(tmp, "nightwatch.json"), Encoding.UTF8.GetBytes(json));
            return ModFeatureManifest.Read(tmp, System.Version.Parse(nightwatch));
        }
        finally
        {
            Directory.Delete(tmp, true);
        }
    }

    internal static void Run()
    {
        var good = Judge("{\"id\":\"nocturne\",\"name\":\"Nocturne\",\"version\":\"3.0.0\",\"author\":\"x\",\"requires\":\"0.3.0\",\"features\":[\"hit_effects\",\"weapon_clip_groups\"]}");
        Check(good.HasManifest && !good.Refused && good.Manifest!.Name == "Nocturne" && good.Manifest.Version == "3.0.0"
            && good.Manifest.Author == "x" && good.Manifest.Requires == "0.3.0" && good.Manifest.Features.Count == 2, "Good manifest is not read");

        var tooNew = Judge("{\"id\":\"a\",\"requires\":\"0.4.0\"}");
        Check(tooNew.Problem == ModFeatureManifest.Problem.TooNew && tooNew.Detail == "0.4.0", "A newer requires is not refused");
        Check(Judge("{\"id\":\"a\",\"requires\":\"0.3.1\"}").Problem == ModFeatureManifest.Problem.TooNew, "Patch level is not compared");
        Check(!Judge("{\"id\":\"a\",\"requires\":\"0.2.9\"}").Refused, "An older requires is refused");
        Check(!Judge("{\"id\":\"a\",\"requires\":\"0.3\"}").Refused, "X.Y is not read as X.Y.0");

        // A pre-release suffix is ignored on both sides: 0.3.0-alpha.1 runs "requires 0.3.0", and "requires 0.3.0-beta.1" runs on 0.3.0.
        Check(!Judge("{\"id\":\"a\",\"requires\":\"0.3.0-beta.1\"}").Refused, "requires with a beta suffix is refused on 0.3.0");
        Check(!Judge("{\"id\":\"a\",\"requires\":\"0.3.0\"}", "0.3.0.0").Refused, "Build part changes the comparison");
        Check(Judge("{\"id\":\"a\",\"requires\":\"0.3.1-beta.1\"}").Problem == ModFeatureManifest.Problem.TooNew, "Suffix hides a newer patch");

        var unknown = Judge("{\"id\":\"a\",\"requires\":\"0.3.0\",\"features\":[\"hit_effects\",\"teleport\"]}");
        Check(unknown.Problem == ModFeatureManifest.Problem.UnknownFeature && unknown.Detail == "teleport", "An unknown feature is not refused");
        Check(Judge("{\"id\":\"a\",\"requires\":\"0.4.0\",\"features\":[\"teleport\"]}").Problem == ModFeatureManifest.Problem.TooNew, "Requires is not checked before features");

        foreach (var bad in new[]
                 {
                     "not json", "[]", "{}", "{\"requires\":\"0.3.0\"}", "{\"id\":\"a\"}", "{\"id\":\"\",\"requires\":\"0.3.0\"}",
                     "{\"id\":\"a\",\"requires\":\"soon\"}", "{\"id\":\"a\",\"requires\":\"1\"}", "{\"id\":\"a\",\"requires\":\"1.2.3.4\"}",
                     "{\"id\":\"a\",\"requires\":\"0.3.0\",\"name\":5}", "{\"id\":\"a\",\"requires\":\"0.3.0\",\"features\":\"hit_effects\"}",
                     "{\"id\":\"a\",\"requires\":\"0.3.0\",\"features\":[\"\"]}", "{\"id\":\"a\",\"requires\":\"0.3.0\",\"features\":[3]}",
                 })
        {
            var v = Judge(bad);
            Check(v.Problem == ModFeatureManifest.Problem.Bad && v.Detail.Length > 0 && v.Manifest == null, "Bad manifest is not refused: " + bad);
        }
        Check(!Judge("﻿{\"id\":\"a\",\"requires\":\"0.3.0\"}").Refused, "A BOM is not skipped");
        Check(!Judge("{\"id\":\"a\",\"requires\":\"0.3.0\",\"extra\":[1]}").Refused, "Unknown keys are not ignored");

        var none = Path.Combine(Path.GetTempPath(), "nw-manifest-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(none);
        try
        {
            var v = ModFeatureManifest.Read(none, new System.Version(0, 3, 0));
            Check(!v.HasManifest && !v.Refused && v.Manifest == null, "A mod without a manifest is not shown as before");
        }
        finally
        {
            Directory.Delete(none, true);
        }
    }
}
