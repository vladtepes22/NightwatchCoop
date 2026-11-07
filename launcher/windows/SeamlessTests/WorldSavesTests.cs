using System.IO;
using System.Security.Cryptography;
using System.Text;
using BloodborneLauncher.Core;
using BloodborneLauncher.Views;

/// <summary>Randomized worlds' saves (WorldSaves): folders under worlds\, local names, the copy of earlier launchers' folders,
/// and the save editor's world list. Temporary folders only.</summary>
internal static class WorldSavesTests
{
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException("World saves: " + message);
    }

    private const string CodeA = "R2-0M000-01G74-ZY", CheckA = "227291f8";
    private const string CodeB = "R2-1C000-035G6-0GM68", CheckB = "2c534bed";

    internal static void Run()
    {
        var root = Path.Combine(Path.GetTempPath(), "nw-worldsaves-" + Guid.NewGuid().ToString("N"));
        try
        {
            Names();
            Directory.CreateDirectory(root);
            WithoutOldSaves(Path.Combine(root, "fresh"));
            WithOldSaves(Path.Combine(root, "old"));
            FailedCheck(Path.Combine(root, "failed"));
            NameRoundTrip(Path.Combine(root, "names"));
            NamingOldFolder(Path.Combine(root, "naming-old"));
            GuestNames(Path.Combine(root, "guest"));
            EditorList(Path.Combine(root, "editor"));
        }
        finally
        {
            WorldSaves.AfterCopy = null;
            try { Directory.Delete(root, true); } catch (IOException) { }
        }
        Console.WriteLine("PASS: world saves (worlds\\ folders, names, copy of old folders and its check, guest names, save editor list)");
    }

    private static Randomizer.World World(string code, string check) => Randomizer.Parse($"Randomizer {code} {check}")!;

    /// <summary>Settings whose saves folder and mods folder are in <paramref name="dir"/>, playing this world (its mod folder made).</summary>
    private static LauncherSettings Playing(string dir, Randomizer.World world)
    {
        var s = new LauncherSettings();
        s.Set("user_dir", Path.Combine(dir, "user"));
        s.Set("mods_dir", Path.Combine(dir, "mods"));
        Directory.CreateDirectory(Path.Combine(dir, "mods", world.Folder));
        Randomizer.Use(s, world.Folder);
        return s;
    }

    // Every file under a folder with its size, time and SHA-256: "untouched" means this is equal before and after.
    private static string Print(string folder) =>
        string.Join("\n", Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal).Select(f =>
        {
            var info = new FileInfo(f);
            return Path.GetRelativePath(folder, f) + "|" + info.Length + "|" + info.LastWriteTimeUtc.Ticks + "|" +
                   Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(f)));
        })) + "\n" + string.Join("\n", Directory.EnumerateDirectories(folder, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal));

    // An earlier launcher's world folder: saves of both kinds, backups, party files and a shader cache.
    private static string OldFolder(string profile, Randomizer.World world)
    {
        var old = WorldSaves.LegacyFolder(profile, world);
        void Put(string relative, string text)
        {
            var path = Path.Combine(old, relative);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, text);
        }
        Put(Path.Combine("savedata", "1", "CUSA03173", "SPRJ0005", "userdata0000"), new string('a', 5000));
        Put(Path.Combine("savedata", "1", "CUSA03173", "SPRJ0005", "userdata0010"), "system");
        Put(Path.Combine("savedata_party", "1", "CUSA03173", "SPRJ0005", "userdata0000"), new string('p', 7000));
        Put(Path.Combine("save_backups", "savedata_party", "20261104-2200", "userdata0000"), "older");
        Put("party.json", "{}");
        Put("trophies.log", "t");
        Put(Path.Combine("cache", "pipelines.bin"), new string('c', 9000));
        return old;
    }

    private static void Names()
    {
        Check(WorldSaves.CleanName("  Night\trun  ") == "Nightrun", "control characters and edges go");
        Check(WorldSaves.CleanName(new string('x', 60)).Length == WorldSaves.MaxName, "a name is cut at its limit");
        Check(WorldSaves.ValidName("   ", out var empty) == null && empty.Length > 0, "an empty name is refused with a reason");
        Check(WorldSaves.ValidName(new string('y', WorldSaves.MaxName + 1), out var tooLong) == null && tooLong.Length > 0, "a long name is refused");
        Check(WorldSaves.ValidName(" Anna's world ", out _) == "Anna's world", "a name keeps its apostrophe");
        Check(WorldSaves.GuestName("Anna") == "Anna's world", "a guest's default name: " + WorldSaves.GuestName("Anna"));
        Check(WorldSaves.GuestName("  ") == "Host's world", "a host without a lobby name");
        var dir = Path.Combine(Path.GetTempPath(), "nw-safefolder-" + Guid.NewGuid().ToString("N"));
        Check(WorldSaves.SafeFolder(dir, "Anna's world: 2/3?") == "Anna s world 2 3", "folder names keep only safe characters: " + WorldSaves.SafeFolder(dir, "Anna's world: 2/3?"));
        Check(WorldSaves.SafeFolder(dir, "con") == "World con" && WorldSaves.SafeFolder(dir, "COM1 run") == "World COM1 run", "no Windows device names");
        Check(WorldSaves.SafeFolder(dir, "***") == "World" && WorldSaves.SafeFolder(dir, new string('z', 80)).Length == 40, "empty and long folder names");
        Check(WorldSaves.SafeFolder(dir, "Ünïcödé Welt") == "Unicode Welt" && WorldSaves.SafeFolder(dir, "世界") == "World", "folder names are ASCII (accents dropped)");
    }

    private static void WithoutOldSaves(string dir)
    {
        var world = World(CodeA, CheckA);
        var s = Playing(dir, world);
        var profile = PlayerProfiles.Folder(s, PlayerProfiles.Current(s));
        Check(WorldSaves.MigrateAll(new[] { profile }).Count == 0, "nothing to copy without old folders");
        Check(WorldSaves.List(profile).Count == 0, "no worlds yet");
        var user = GameEnvironment.UserDir(s);
        Check(Path.GetDirectoryName(user) == WorldSaves.WorldsDir(profile), "a world plays in worlds\\: " + user);
        Check(WorldSaves.Read(user) is { Code: CodeA, Check: CheckA, Name: "World 1", Named: false, Joined: false }, "a first start names it World 1");
        Check(GameEnvironment.UserDir(s) == user && WorldSaves.List(profile).Count == 1, "the same folder every time");
        Check(!Directory.Exists(Path.Combine(profile, WorldSaves.LegacyRootName)), "no old randomizer folder is made");
        Check(SaveBackups.Folder(s) == Path.Combine(GameEnvironment.SavesRoot(s), "launcher-backups", PlayerProfiles.Main, WorldSaves.RootName, Path.GetFileName(user)),
            "a world's launcher backups are kept apart: " + SaveBackups.Folder(s));
        Randomizer.StopUsing(s);
        Check(GameEnvironment.UserDir(s) == profile && SaveBackups.Folder(s) == Path.Combine(GameEnvironment.SavesRoot(s), "launcher-backups", PlayerProfiles.Main),
            "without a world: the profile's own folder and backups");
    }

    private static void WithOldSaves(string dir)
    {
        var world = World(CodeA, CheckA);
        var other = World(CodeB, CheckB);
        var s = Playing(dir, world);
        var profile = PlayerProfiles.Folder(s, PlayerProfiles.Current(s));
        var old = OldFolder(profile, world);
        var oldOther = OldFolder(profile, other);
        var before = Print(old);
        Check(GameEnvironment.UserDir(s) == old, "before the copy the old folder keeps playing");
        Check(WorldSaves.List(profile) is { Count: 2 } listed && listed.All(e => e.Legacy) && listed.Any(e => e.Name == CodeA && e.Folder == old), "old folders not copied yet are listed by their code");
        var moved = WorldSaves.MigrateAll(new[] { profile });
        Check(moved.Count == 2 && moved.All(m => m.Error == null && m.To != null), "both old folders copied: " + string.Join("; ", moved.Select(m => m.Error)));
        var to = moved.Single(m => m.From == old).To!;
        Check(Print(old) == before, "the old folder is left as it was");
        Check(Directory.Exists(oldOther), "the other old folder stays too");
        Check(WorldSaves.Difference(old, to) == null, "the copy holds the same files");
        Check(!Directory.Exists(Path.Combine(to, "cache")), "the shader cache is left to the engine");
        Check(File.ReadAllText(Path.Combine(to, "savedata_party", "1", "CUSA03173", "SPRJ0005", "userdata0000")) == new string('p', 7000), "party saves copied");
        Check(WorldSaves.Read(to) is { Code: CodeA, Check: CheckA, Named: false }, "the copy knows its world");
        Check(GameEnvironment.UserDir(s) == to, "after the copy the game plays the new folder");
        Check(File.Exists(Path.Combine(profile, WorldSaves.LegacyRootName, "README.txt")), "a note says what the old folder is");
        Check(!Directory.EnumerateDirectories(WorldSaves.WorldsDir(profile), "*.copying").Any(), "no unfinished copy left");
        var names = WorldSaves.List(profile).Select(e => e.Name).ToList();
        Check(names.Count == 2 && names.Distinct().Count() == 2 && WorldSaves.List(profile).All(e => !e.Legacy), "two worlds with their own names: " + string.Join(", ", names));
        Check(WorldSaves.MigrateAll(new[] { profile }).Count == 0, "a second run copies nothing");
        Check(WorldSaves.MigrateWorld(profile, world).To == to, "copying a copied world again points at its folder");
    }

    private static void FailedCheck(string dir)
    {
        var world = World(CodeA, CheckA);
        var s = Playing(dir, world);
        var profile = PlayerProfiles.Folder(s, PlayerProfiles.Current(s));
        var old = OldFolder(profile, world);
        var before = Print(old);
        // A copy that comes out different (a disk error, a file changed while copying): the check must catch it.
        WorldSaves.AfterCopy = copy => File.WriteAllText(Path.Combine(copy, "savedata_party", "1", "CUSA03173", "SPRJ0005", "userdata0000"), new string('q', 7000));
        var result = WorldSaves.MigrateWorld(profile, world);
        Check(result.To == null && result.Error is { Length: > 0 }, "a different copy is refused");
        Check(Print(old) == before, "a failed check leaves the old folder untouched");
        Check(WorldSaves.Find(profile, CodeA, CheckA) == null, "no world folder after a failed check");
        Check(!Directory.Exists(WorldSaves.WorldsDir(profile)) || !Directory.EnumerateFileSystemEntries(WorldSaves.WorldsDir(profile)).Any(),
            "the unfinished copy is removed");
        Check(GameEnvironment.UserDir(s) == old, "the game keeps playing the old folder");
        // A missing file in the copy is caught too.
        WorldSaves.AfterCopy = copy => File.Delete(Path.Combine(copy, "trophies.log"));
        Check(WorldSaves.MigrateWorld(profile, world).To == null && Print(old) == before, "a copy with a file missing is refused");
        // An unfinished copy from a stopped run is cleaned up, then the copy succeeds.
        WorldSaves.AfterCopy = null;
        var stale = Path.Combine(WorldSaves.WorldsDir(profile), "World 1.copying");
        Directory.CreateDirectory(stale);
        File.WriteAllText(Path.Combine(stale, WorldSaves.InfoFile), "{\"code\": \"" + CodeA + "\", \"check\": \"" + CheckA + "\", \"name\": \"World 1\"}");
        File.WriteAllText(Path.Combine(stale, "half"), "x");
        var again = WorldSaves.MigrateWorld(profile, world);
        Check(again.To != null && !Directory.Exists(stale) && Print(old) == before, "a stopped copy is redone: " + again.Error);
    }

    private static void NameRoundTrip(string dir)
    {
        var world = World(CodeA, CheckA);
        var s = Playing(dir, world);
        var profile = PlayerProfiles.Folder(s, PlayerProfiles.Current(s));
        var made = WorldSaves.SetName(profile, world, "Yharnam après minuit: 2/3");
        Check(made.Name == "Yharnam après minuit: 2/3" && made.Named && Path.GetFileName(made.Folder) == "Yharnam apres minuit 2 3", "a typed name: " + made.Folder);
        Check(WorldSaves.Read(made.Folder) == made, "the name reads back exactly");
        Check(GameEnvironment.UserDir(s) == made.Folder, "the game plays the named folder");
        File.WriteAllText(Path.Combine(made.Folder, "keep.txt"), "save");
        var renamed = WorldSaves.SetName(profile, world, "  Second name  ");
        Check(renamed.Folder == made.Folder && renamed.Name == "Second name" && File.Exists(Path.Combine(made.Folder, "keep.txt")), "renaming moves nothing");
        Check(WorldSaves.Read(made.Folder)?.Name == "Second name" && WorldSaves.Find(profile, CodeA, CheckA)?.Name == "Second name", "the new name reads back");
        Check(WorldSaves.List(profile).Single().Folder == made.Folder, "still one world");
        var twin = WorldSaves.SetName(profile, World(CodeB, CheckB), "Yharnam après minuit 2 3");
        Check(twin.Folder != made.Folder && Path.GetFileName(twin.Folder) == "Yharnam apres minuit 2 3 2", "a taken folder name gets a number: " + twin.Folder);
        // The name stays local: nothing of it in the world's mod folder, and the folder name reads as the same world.
        Check(!Directory.EnumerateFiles(Path.Combine(dir, "mods"), "*", SearchOption.AllDirectories).Any(), "the world's files are not written");
        Check(Randomizer.Active(s)?.Folder == world.Folder, "the world is still the same world");
        try { WorldSaves.SetName(profile, world, " \t "); Check(false, "an empty name is refused"); }
        catch (ArgumentException) { }
    }

    // A name for a world whose saves are still in an earlier launcher's folder: never a new empty folder in front of them.
    private static void NamingOldFolder(string dir)
    {
        var world = World(CodeA, CheckA);
        var s = Playing(dir, world);
        var profile = PlayerProfiles.Folder(s, PlayerProfiles.Current(s));
        var old = OldFolder(profile, world);
        var before = Print(old);
        try { WorldSaves.SetName(profile, world, "Mine", mayCopy: false); Check(false, "naming while the old folder can't be copied is refused"); }
        catch (IOException) { }
        Check(WorldSaves.Find(profile, CodeA, CheckA) == null && GameEnvironment.UserDir(s) == old, "a refused name leaves the old folder in use");
        Check(WorldSaves.NameForGuest(profile, world, "Anna", mayCopy: false).Legacy && GameEnvironment.UserDir(s) == old, "a guest's name too");
        var named = WorldSaves.SetName(profile, world, "Mine");
        Check(named.Name == "Mine" && named.Folder != old && WorldSaves.Difference(old, named.Folder) == null && Print(old) == before &&
              GameEnvironment.UserDir(s) == named.Folder, "naming copies the old folder first, then names the copy");
    }

    private static void GuestNames(string dir)
    {
        var world = World(CodeA, CheckA);
        var s = Playing(dir, world);
        var profile = PlayerProfiles.Folder(s, PlayerProfiles.Current(s));
        // The join switched to the world; the first lookups made its folder with a default name before the name came.
        var user = GameEnvironment.UserDir(s);
        WorldSaves.NameForGuest(s, "Anna");
        Check(WorldSaves.Read(user) is { Name: "Anna's world", Joined: true, Named: false }, "a guest's copy is named after the host: " + WorldSaves.Read(user)?.Name);
        WorldSaves.SetName(profile, world, "Our run");
        WorldSaves.NameForGuest(s, "Brian");
        Check(WorldSaves.Read(user)?.Name == "Our run", "a name the player chose stays");
        // A guest's copy of another world, no folder yet: made with the host's name.
        var other = World(CodeB, CheckB);
        var made = WorldSaves.NameForGuest(profile, other, "Céline");
        Check(made is { Name: "Céline's world", Joined: true } && WorldSaves.Find(profile, CodeB, CheckB)?.Folder == made.Folder, "a new guest copy");
        // An old folder of a joined world is copied first, so the name lands where the game plays.
        var third = World("R2-00", "aaaaaaaa");
        var old = OldFolder(profile, third);
        var moved = WorldSaves.NameForGuest(profile, third, "Dana");
        Check(moved.Name == "Dana's world" && moved.Folder != old && Directory.Exists(Path.Combine(moved.Folder, "savedata")), "an old folder is copied, then named");
        // No world played: nothing happens.
        Randomizer.StopUsing(s);
        WorldSaves.NameForGuest(s, "Eve");
        Check(WorldSaves.List(profile).All(e => e.Name != "Eve's world"), "no world, no name");
    }

    private static void EditorList(string dir)
    {
        var a = World(CodeA, CheckA);
        var s = Playing(dir, a);
        var profile = PlayerProfiles.Folder(s, PlayerProfiles.Current(s));
        WorldSaves.SetName(profile, a, "zeta run");
        WorldSaves.NameForGuest(profile, World(CodeB, CheckB), "Anna");
        OldFolder(profile, World("R2-11", "bbbbbbbb"));
        var list = WorldSaves.List(profile);
        Check(list.Select(e => e.Name).SequenceEqual(new[] { "Anna's world", "R2-11", "zeta run" }), "the editor's list, by name: " + string.Join(", ", list.Select(e => e.Name)));
        Check(list.Select(SaveEditorWindow.WorldLabel).SequenceEqual(new[] { "Anna's world (joined)", "R2-11", "zeta run" }), "guest copies are marked joined");
        Check(list.Single(e => e.Legacy).Folder == WorldSaves.LegacyFolder(profile, World("R2-11", "bbbbbbbb")), "an old folder not copied yet opens in place");
        Check(list.Where(e => !e.Legacy).All(e => Path.GetDirectoryName(e.Folder) == WorldSaves.WorldsDir(profile)), "the others are under worlds\\");
        Check(WorldSaves.List(Path.Combine(dir, "nobody")).Count == 0, "a profile without worlds lists none");
        // Another profile has its own worlds.
        PlayerProfiles.Add(s, "Second");
        s.Set("player_profile", "Second");
        var second = PlayerProfiles.Folder(s, "Second");
        Check(WorldSaves.List(second).Count == 0 && GameEnvironment.UserDir(s).StartsWith(WorldSaves.WorldsDir(second)), "worlds are per profile");
    }
}
