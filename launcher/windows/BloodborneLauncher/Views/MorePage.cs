using System.Windows;
using System.Windows.Controls;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>The less used pages behind one tab: trophies, cheats, the engine's switches and the game's log (Controls,
    /// Co-op, Workshop and Launcher have tabs of their own; Controls is back on the bar, user 4 Nov).</summary>
    public sealed class MorePage : SectionPage
    {
        // Cheats only once the engine applies them.
        public MorePage() : base(L.T("More"), new (string, string, Type?)[]
        {
            ("trophies", L.T("Trophies"), typeof(TrophiesPage)),
            ("cheats", L.T("Cheats"), typeof(CheatsPage)),
            ("engine", L.T("Engine"), typeof(EnginePage)),
            ("output", L.T("Log"), typeof(OutputPage)),
        }.Where(s => s.Item1 != "cheats" || CheatsPage.Supported).ToArray()) { }
    }
}
