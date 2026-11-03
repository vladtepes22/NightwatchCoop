using System.Windows;
using System.Windows.Controls;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>The Workshop tab: mods, and a button that opens the save editor.</summary>
    public sealed class WorkshopPage : SectionPage
    {
        public WorkshopPage() : base(L.T("Workshop"), new (string, string, Type?)[]
        {
            ("mods", L.T("Mods"), typeof(ModsPage)),
            ("saveeditor", L.T("Save editor"), null),
        }) { }

        protected override void Activate(string key)
        {
            if (key == "saveeditor") new SaveEditorWindow(S).ShowDialog();
        }
    }
}
