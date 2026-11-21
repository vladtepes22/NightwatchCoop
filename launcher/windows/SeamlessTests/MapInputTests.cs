using System.IO;
using BloodborneLauncher.Core;

/// <summary>The map generator's input: with the 1.09 update in its own folder beside the game, the
/// update's dvdroot_ps4 files lie over the game's (on base dumps the update brings the DLC maps), and
/// removing the view leaves the game and update folders as they were.</summary>
internal static class MapInputTests
{
    private static void Check(bool ok, string what)
    {
        if (!ok) throw new InvalidOperationException("Map input: " + what);
    }

    private static void Write(string path, string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, text);
    }

    public static void Run()
    {
        var tmp = Path.Combine(Path.GetTempPath(), "nw-map-input-" + Guid.NewGuid().ToString("N"));
        try
        {
            var game = Path.Combine(tmp, "CUSA00900");
            var update = Path.Combine(tmp, "CUSA00900-patch");
            var root = Path.Combine(tmp, "maps");
            Write(Path.Combine(game, "eboot.bin"), "base eboot");
            Write(Path.Combine(game, "sce_sys", "param.sfo"), "base sfo");
            Write(Path.Combine(game, "dvdroot_ps4", "map", "mapstudio", "m21_00_00_00.msb.dcx"), "base m21");
            Write(Path.Combine(game, "dvdroot_ps4", "map", "mapstudio", "m24_00_00_00.msb.dcx"), "base m24");
            Write(Path.Combine(game, "dvdroot_ps4", "msg", "engus", "menu.msgbnd.dcx"), "base menu");
            Write(Path.Combine(update, "eboot.bin"), "1.09 eboot");
            Write(Path.Combine(update, "sce_sys", "param.sfo"), "1.09 sfo");
            Write(Path.Combine(update, "dvdroot_ps4", "map", "mapstudio", "m24_00_00_00.msb.dcx"), "1.09 m24");
            Write(Path.Combine(update, "dvdroot_ps4", "map", "mapstudio", "m34_00_00_00.msb.dcx"), "dlc m34");
            Write(Path.Combine(update, "dvdroot_ps4", "event", "m34_00_00_00.emevd.dcx"), "dlc event");

            // Read-only game and update files (a copy from a disc or a read-only share) in a folder both have: the view's
            // hard links share that flag, and removing the view must still work and leave the flag on the game's files.
            var readOnly = new[] { Path.Combine(game, "dvdroot_ps4", "map", "mapstudio", "m21_00_00_00.msb.dcx"),
                                   Path.Combine(update, "dvdroot_ps4", "map", "mapstudio", "m24_00_00_00.msb.dcx") };
            foreach (var file in readOnly) File.SetAttributes(file, FileAttributes.ReadOnly);

            var input = MapPrep.PrepareInput(game, root, out var unlinked);
            Check(input != game, "a separate update folder was not used");
            Check(!unlinked, "the view on one drive was not linked");
            string Read(params string[] parts) => File.ReadAllText(Path.Combine(new[] { input }.Concat(parts).ToArray()));
            var studio = new[] { "dvdroot_ps4", "map", "mapstudio" };
            Check(Read(studio.Append("m21_00_00_00.msb.dcx").ToArray()) == "base m21", "a game-only map is missing");
            Check(Read(studio.Append("m24_00_00_00.msb.dcx").ToArray()) == "1.09 m24", "the update's file does not replace the game's");
            Check(Read(studio.Append("m34_00_00_00.msb.dcx").ToArray()) == "dlc m34", "a map only the update has is missing");
            Check(Read("dvdroot_ps4", "msg", "engus", "menu.msgbnd.dcx") == "base menu", "a game-only folder is missing");
            Check(Read("dvdroot_ps4", "event", "m34_00_00_00.emevd.dcx") == "dlc event", "an update-only folder is missing");
            Check(Read("eboot.bin") == "1.09 eboot" && Read("sce_sys", "param.sfo") == "1.09 sfo", "the update's eboot or sfo is not used");

            MapPrep.RemoveInput(input);
            Check(!Directory.Exists(input), "the view was not removed");
            Check(File.ReadAllText(Path.Combine(game, "dvdroot_ps4", "map", "mapstudio", "m24_00_00_00.msb.dcx")) == "base m24" &&
                  File.Exists(Path.Combine(game, "dvdroot_ps4", "msg", "engus", "menu.msgbnd.dcx")) &&
                  File.Exists(Path.Combine(update, "dvdroot_ps4", "map", "mapstudio", "m34_00_00_00.msb.dcx")) &&
                  File.Exists(Path.Combine(update, "dvdroot_ps4", "event", "m34_00_00_00.emevd.dcx")),
                  "removing the view touched the game or update files");
            Check(readOnly.All(f => File.Exists(f) && File.GetAttributes(f).HasFlag(FileAttributes.ReadOnly)),
                  "removing the view changed the game's read-only files");
            foreach (var file in readOnly) File.SetAttributes(file, FileAttributes.Normal);

            MapPrep.RemoveInput(game);
            Check(File.Exists(Path.Combine(game, "eboot.bin")), "RemoveInput deleted a folder it did not make");

            // NW-bbc8683: a game with only eboot.bin and sce_sys copied in says 01.09 but lacks the update's map files:
            // the randomizer must still read through the update (GameCheck.LaidOverGame, bb-start's rule)
            Check(GameCheck.MissesUpdateFiles(update, game), "a game without the update's files counts as updated");
            foreach (var file in Directory.EnumerateFiles(Path.Combine(update, "dvdroot_ps4"), "*", SearchOption.AllDirectories))
                Write(Path.Combine(game, "dvdroot_ps4", Path.GetRelativePath(Path.Combine(update, "dvdroot_ps4"), file)), "copied in");
            Check(!GameCheck.MissesUpdateFiles(update, game), "a game with all the update's files counts as not updated");
        }
        finally
        {
            try { MapPrep.RemoveInput(Path.Combine(tmp, "maps", "input")); } catch { /* the checks above say why */ }
            foreach (var file in Directory.EnumerateFiles(tmp, "*", SearchOption.AllDirectories)) File.SetAttributes(file, FileAttributes.Normal);
            Directory.Delete(tmp, recursive: true);
        }
    }
}
