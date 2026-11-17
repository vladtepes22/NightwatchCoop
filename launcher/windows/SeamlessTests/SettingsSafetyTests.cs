using System.IO;
using System.Text.Json.Nodes;
using BloodborneLauncher.Core;

/// <summary>bbport.ini, mods.json, patches.json and settings.json are never written over a file that could not be read,
/// keep the last good one as .bak, are written through a temp file, and the ini keeps what the game wrote meanwhile.</summary>
internal static class SettingsSafetyTests
{
    private static readonly List<string> Failures = new();

    // Collects instead of throwing, so one run names every broken rule.
    private static void Check(bool ok, string what)
    {
        if (!ok) Failures.Add(what);
    }

    internal static void Run()
    {
        var dir = Path.Combine(Path.GetTempPath(), "nw-settings-safety-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        var oldConfig = Environment.GetEnvironmentVariable("BB_CONFIG");
        var oldData = Environment.GetEnvironmentVariable("BB_DATA_DIR");
        try
        {
            Ini(dir);
            Json(dir);
        }
        finally
        {
            Environment.SetEnvironmentVariable("BB_CONFIG", oldConfig);
            Environment.SetEnvironmentVariable("BB_DATA_DIR", oldData);
            try { Directory.Delete(dir, true); } catch { }
        }
        if (Failures.Count > 0) throw new InvalidOperationException(string.Join("; ", Failures));
        Console.WriteLine("PASS: settings files are not written over an unreadable file, keep a .bak, leave no temp file, and the ini keeps the game's changes");
    }

    private static void Ini(string dir)
    {
        var path = Path.Combine(dir, "bbport.ini");
        Environment.SetEnvironmentVariable("BB_CONFIG", path);
        File.WriteAllText(path, "# mine\nupscaler=fsr\nshow_fps=0\n");

        // A normal save: the previous file stays as .bak, no temp file is left behind.
        var ini = new IniFile();
        ini.Load();
        ini.Set("sharpen", "0.80");
        ini.Save();
        Check(File.ReadAllText(path).Contains("sharpen=0.80") && File.ReadAllText(path).Contains("# mine"), "The ini save did not write the change");
        Check(File.Exists(path + ".bak") && File.ReadAllText(path + ".bak") == "# mine\nupscaler=fsr\nshow_fps=0\n", "The ini save kept no .bak of the old file");
        Check(!File.Exists(path + ".tmp"), "The ini save left a temp file behind");

        // The game changes another key (and adds one) while the launcher holds the old copy: that stays.
        File.WriteAllText(path, "# mine\nupscaler=fsr\nshow_fps=1\ngame_only=7\nsharpen=0.80\n");
        ini.Set("preset", "3");
        ini.Save();
        var text = File.ReadAllText(path);
        Check(text.Contains("show_fps=1") && text.Contains("game_only=7") && text.Contains("preset=3"), "A save undid what the game wrote meanwhile");

        // A file that cannot be read right now (held open for writing) is never replaced by defaults.
        var before = File.ReadAllText(path);
        var told = new List<string>();
        void Hear(string m) => told.Add(m);
        SettingsFile.Warned += Hear;
        try
        {
            ini.Set("preset", "5");
            using (new FileStream(path, FileMode.Open, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete))
            {
                for (var i = 0; i < 2; i++)
                    try { ini.Save(); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            }
            Check(File.ReadAllText(path) == before, "An unreadable ini was written over");
            Check(!File.Exists(path + ".tmp"), "A refused ini save left a temp file behind");
            Check(told.Count == 1 && told[0].Contains("bbport.ini") && !told[0].Contains("antern"), "The refused ini save was not reported once: " + string.Join(" | ", told));
            // The change is not lost: it goes in once the file reads again.
            ini.Save();
            Check(File.ReadAllText(path).Contains("preset=5") && File.ReadAllText(path).Contains("game_only=7"), "The pending ini change was lost after a refused save");
        }
        finally { SettingsFile.Warned -= Hear; }
    }

    private static void Json(string dir)
    {
        Environment.SetEnvironmentVariable("BB_DATA_DIR", dir);
        var mods = Mods.ProfilePath;
        var list = new[] { new Mods.Entry("A", true), new Mods.Entry("B", false) };

        // Corrupt mods.json: kept as it is.
        File.WriteAllText(mods, "{ \"order\": [\"A\", ");
        Mods.Save(list);
        Check(File.ReadAllText(mods) == "{ \"order\": [\"A\", ", "A corrupt mods.json was written over");
        Check(!File.Exists(mods + ".tmp") && !File.Exists(mods + ".bak"), "A refused mods.json save left a temp or .bak file");

        // A good one: replaced, the old content kept as .bak, no temp file.
        File.WriteAllText(mods, "{\"order\":[\"Old\"],\"disabled\":[]}");
        Mods.Save(list);
        Check(JsonNode.Parse(File.ReadAllText(mods))!["order"]!.AsArray().Count == 2, "mods.json was not saved");
        Check(File.Exists(mods + ".bak") && File.ReadAllText(mods + ".bak") == "{\"order\":[\"Old\"],\"disabled\":[]}", "mods.json kept no .bak of the old file");
        Check(!File.Exists(mods + ".tmp"), "mods.json save left a temp file behind");

        // A bad file never becomes the .bak: the good copy survives a refused save and a later repair.
        File.WriteAllText(mods, "");
        Mods.Save(list);
        Check(File.Exists(mods + ".bak") && File.ReadAllText(mods + ".bak") == "{\"order\":[\"Old\"],\"disabled\":[]}", "An empty mods.json replaced the .bak");

        // settings.json: a broken file is not replaced by the defaults on the next save; a good one keeps a .bak.
        var file = Path.Combine(dir, "settings.json");
        File.WriteAllText(file, "{ \"ui_language\": \"de\", ");
        var broken = new LauncherSettings(file);
        broken.Load();
        broken.Set("ui_language", "fr");
        broken.Save();
        Check(File.Exists(file + ".broken") && File.ReadAllText(file + ".broken") == "{ \"ui_language\": \"de\", ",
            "A corrupt settings.json was not kept as settings.json.broken");
        Check(File.Exists(file) && File.ReadAllText(file).Contains("\"fr\"") && !File.Exists(file + ".tmp"),
            "Saving stayed refused after a corrupt settings.json was moved aside");
        File.Delete(file + ".broken");
        File.Delete(file + ".bak");
        File.WriteAllText(file, "{\"ui_language\":\"de\",\"extra_key\":\"kept\"}");
        var good = new LauncherSettings(file);
        good.Load();
        Check(good.Str("ui_language") == "de", "settings.json was not read");
        good.Set("ui_language", "fr");
        good.Save();
        Check(File.ReadAllText(file).Contains("\"fr\"") && File.ReadAllText(file).Contains("extra_key"), "settings.json was not saved");
        Check(File.Exists(file + ".bak") && File.ReadAllText(file + ".bak") == "{\"ui_language\":\"de\",\"extra_key\":\"kept\"}", "settings.json kept no .bak of the old file");
        Check(!File.Exists(file + ".tmp"), "settings.json save left a temp file behind");

        var patches = Patches.ProfilePath;
        File.WriteAllText(patches, "not json");
        Patches.Save(new List<(Patches.Patch Patch, bool On)>());
        Check(File.ReadAllText(patches) == "not json", "A corrupt patches.json was written over");
    }
}
