using System.Windows;
using System.Windows.Controls;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>The Workshop tab: mods, the randomizer, and a button that opens the save editor.</summary>
    public sealed class WorkshopPage : SectionPage
    {
        public WorkshopPage() : base(L.T("Workshop"), new (string, string, Type?)[]
        {
            ("mods", L.T("Mods"), typeof(ModsPage)),
            ("randomizer", L.T("Randomizer"), typeof(RandomizerPage)),
            ("merge", L.T("Merge"), typeof(MergePage)),
            ("saveeditor", L.T("Save editor"), null),
        }.Where(s => (s.Item1 != "randomizer" || Randomizer.Supported) && (s.Item1 != "merge" || Merger.Supported)).ToArray()) { }

        protected override void Activate(string key)
        {
            if (key == "saveeditor") new SaveEditorWindow(S).ShowDialog();
        }
    }
}
