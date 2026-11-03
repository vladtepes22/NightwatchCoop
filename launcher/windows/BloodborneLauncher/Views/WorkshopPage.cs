using System.Windows;
using System.Windows.Controls;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>The Workshop tab: mods.</summary>
    public sealed class WorkshopPage : SectionPage
    {
        public WorkshopPage() : base(L.T("Workshop"), new (string, string, Type)[]
        {
            ("mods", L.T("Mods"), typeof(ModsPage)),
        }) { }
    }
}
