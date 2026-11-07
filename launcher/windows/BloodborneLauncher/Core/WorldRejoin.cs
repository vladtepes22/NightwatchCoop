using System.IO;
using System.Text.RegularExpressions;

namespace BloodborneLauncher.Core;

/// <summary>
/// A guest the host refused for other mods, where the host's mods label names a randomized world: the launcher builds that
/// world here and joins again by itself, once per label. The host's label reaches a guest two ways (game 0.7.9,
/// party_options.h reject_with_mods / party_runtime.cpp write_host_mods_file): a newer guest game writes it to
/// party_host_mods.txt in the user folder (one line) and adds "(the host's mods: LABEL)" to its error text; an older guest
/// game shows the host's "[host mods: LABEL] " prefix inside the refusal text. Games before 0.7.9 send no label: nothing
/// happens then (the Join lists' "Build world and join" still works before joining). The label is a key only.
/// </summary>
public static class WorldRejoin
{
    public const string HostModsFile = "party_host_mods.txt";

    /// <summary>The join a start was made with (the code or address, its password and the host options a row told), so a
    /// join again after building the world goes to the same host. Recorded by <see cref="SeamlessSettings.Apply"/>.</summary>
    public sealed record JoinTarget(string Code, string Password, Dictionary<string, string>? HostOptions);

    public static JoinTarget? LastJoin { get; private set; }

    /// <summary>A start in join mode (not an invasion): the host it goes to.</summary>
    public static void Started(string code, string password, Dictionary<string, string>? hostOptions) =>
        LastJoin = new JoinTarget(code, password, hostOptions == null ? null : new Dictionary<string, string>(hostOptions));

    public static void Forget() => LastJoin = null;

    /// <summary>The host's lobby name for a guest's copy of its world (WorldSaves.NameForGuest): the first member of the refused
    /// session that is not this game, or "" when the refusal came before any member was listed (the name is then "Host's world").</summary>
    public static string HostName(SeamlessSession.Status? status) =>
        status?.members?.FirstOrDefault(m => m != null && !m.local && !string.IsNullOrWhiteSpace(m.name))?.name ?? "";

    private const string LabelChars = "[a-z0-9@._:+\\-]{1,255}";
    private static readonly Regex Prefix = new(@"\[host mods: (" + LabelChars + @")\] ", RegexOptions.CultureInvariant);
    private static readonly Regex Suffix = new(@"\(the host's mods: (" + LabelChars + @")\)", RegexOptions.CultureInvariant);

    /// <summary>The host's mods label a refusal text names ("[host mods: LABEL] " as an older guest game shows it, or "(the
    /// host's mods: LABEL)" as a newer one adds it), or null. Re-validated: a label the lobby would refuse is no label.</summary>
    public static string? LabelFromText(string? text)
    {
        foreach (var rx in new[] { Prefix, Suffix })
            if (rx.Match(text ?? "") is { Success: true } m && Valid(m.Groups[1].Value))
                return m.Groups[1].Value;
        return null;
    }

    /// <summary>party_host_mods.txt as a newer guest game writes it after a refusal over mods: one valid label, written at or
    /// after <paramref name="notBeforeUtc"/> (the game's start: the file stays after a later successful join). Null otherwise.</summary>
    public static string? ReadHostModsFile(string userDir, DateTime notBeforeUtc)
    {
        try
        {
            var path = Path.Combine(userDir, HostModsFile);
            if (!File.Exists(path) || File.GetLastWriteTimeUtc(path) < notBeforeUtc.AddSeconds(-2)) return null;
            var info = new FileInfo(path);
            if (info.Length > 512) return null;
            var label = File.ReadAllText(path).Trim();
            return Valid(label) ? label : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    private static bool Valid(string label) =>
        label.All(c => c < 0x80) && VpnLobby.ValidModsLabel(System.Text.Encoding.ASCII.GetBytes(label));

    /// <summary>The host's label after this guest was refused: the file first, then the status texts. Null for a host, a
    /// guest not refused for mods, or a host that sends no label.</summary>
    public static string? RefusedHostLabel(SeamlessSession.Status? status, string userDir, DateTime gameStartUtc)
    {
        if (status is not { role: "guest" }) return null;
        var text = status.detail + "\n" + status.alert + "\n" + status.@event;
        // The file alone is not enough: it is kept after a later join, so the refusal must be this one's (texts or state).
        var state = status.state ?? "";
        var refused = state.Contains("reject", StringComparison.OrdinalIgnoreCase) || state.Contains("refus", StringComparison.OrdinalIgnoreCase) ||
                      text.Contains("refused", StringComparison.OrdinalIgnoreCase) || LabelFromText(text) != null;
        if (!refused) return null;
        return ReadHostModsFile(userDir, gameStartUtc) ?? LabelFromText(text);
    }

    /// <summary>What to do about a refusal naming this host label: Build when the label names a randomized world this PC can
    /// build (<see cref="CuratedMods.JoinAdvice"/> BuildWorld) and no automatic try was made for it yet; else None.</summary>
    public enum Answer { None, Build }

    private static string? _tried;

    public static Answer Decide(string? hostLabel, JoinChoice advice) =>
        hostLabel == null || advice.Kind != JoinKind.BuildWorld || _tried == hostLabel ? Answer.None : Answer.Build;

    /// <summary>Clears the once-per-label memory (tests; a new launcher run starts clear too).</summary>
    public static void Reset() => _tried = null;

    /// <summary>The automatic answer to a refusal over mods: stop the game, build (or reuse) the host's world with progress,
    /// then join the same host again. Each step is given, so the flow runs the same in the launcher and in tests. Returns
    /// true when it joined again; a failed build throws (the caller shows the message) and nothing joins. Only once per
    /// label: a second refusal naming the same world is left to the player (another cause than the world).</summary>
    public static async Task<bool> RunAsync(string hostLabel, JoinChoice advice, Func<Task> stopGame,
        Func<JoinChoice, Action<double, string>, Task> build, Func<Task> joinAgain, Action<double, string> progress)
    {
        if (Decide(hostLabel, advice) != Answer.Build) return false;
        _tried = hostLabel;
        progress(0, L.T("Building the host's world…"));
        await stopGame();
        await build(advice, (fraction, step) => progress(fraction, L.T("Building the host's world…") + " " + step));
        progress(1, L.T("Joining again…"));
        await joinAgain();
        return true;
    }
}
