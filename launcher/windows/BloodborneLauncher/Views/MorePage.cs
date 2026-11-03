using System.Windows;
using System.Windows.Controls;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>The less used pages behind one tab: controls, trophies, cheats, the engine's switches and the game's
    /// log (Co-op, Workshop and Launcher have tabs of their own).</summary>
    public sealed class MorePage : SectionPage
    {
        // Cheats only once the engine applies them.
        public MorePage() : base(L.T("More"), new (string, string, Type?)[]
        {
            ("controls", L.T("Controls"), typeof(ControlsPage)),
            ("trophies", L.T("Trophies"), typeof(TrophiesPage)),
            ("cheats", L.T("Cheats"), typeof(CheatsPage)),
            ("engine", L.T("Engine"), typeof(EnginePage)),
            ("output", L.T("Log"), typeof(OutputPage)),
        }.Where(s => s.Item1 != "cheats" || CheatsPage.Supported).ToArray()) { }
    }
}
