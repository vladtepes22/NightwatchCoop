namespace BloodborneLauncher.Core;

public static class LobbyListRules
{
    public static (List<LobbyGame> Games, int Total, int Hidden) Apply(IEnumerable<LobbyGame> source,
        string buildPrefix, bool hideFull, bool hideWrongBuild, bool noPassword)
    {
        var all = source.ToList();
        var games = all.Where(g => (!hideFull || g.Players < g.Max)
                && (!hideWrongBuild || PartyDiscovery.BuildMatches(g.Build, buildPrefix))
                && (!noPassword || !g.Password))
            .OrderBy(g => g.Players >= g.Max || g.Password || !PartyDiscovery.BuildMatches(g.Build, buildPrefix))
            .ToList();
        return (games, all.Count, all.Count - games.Count);
    }
}
