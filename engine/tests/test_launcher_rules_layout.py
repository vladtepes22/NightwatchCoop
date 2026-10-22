"""Party Rules & Sync dialog layout (0.7.2): it fits a 1366x768 screen; the sections scroll.

Built off screen: a withdrawn Tk root and a withdrawn dialog (no window shown, no focus taken);
the sizes come from Tk's geometry requests. python -m pytest tests/test_launcher_rules_layout.py
"""
from paths import ROOT
import importlib.util
import tkinter as tk
import unittest


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_layout", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class RulesDialogLayout(unittest.TestCase):
    def test_fits_768_screen(self):
        try:
            L = load_launcher()
            app = L.BloodborneLauncher.__new__(L.BloodborneLauncher)
            tk.Tk.__init__(app)
        except Exception as ex:  # no tkinter / display here
            raise unittest.SkipTest(f"no Tk here: {ex}")
        try:
            app.withdraw()
            app.build_style()
            app.settings = {}
            app.party_switch_vars = {k: tk.BooleanVar(app, value=d) for k, _, d in L.PARTY_SWITCHES + L.PARTY_INI_SWITCHES}
            app.party_start_var = tk.StringVar(app, value=L.PARTY_START_CHOICES[0][0])
            app.party_insight_var = tk.StringVar(app, value=L.PARTY_INSIGHT_CHOICES[0][0])
            app.party_backup_minutes_var = tk.StringVar(app, value="10")
            app.party_backup_keep_var = tk.StringVar(app, value="10")
            app.save_settings = lambda: None
            app.save_party_rules = lambda: None
            app.show_party_help = lambda: None
            made = []
            real = L.tk.Toplevel

            class Hidden(real):  # never mapped: no window on the screen, no focus
                def __init__(self, *a, **k):
                    super().__init__(*a, **k)
                    self.withdraw()
                    made.append(self)

                def grab_set(self):
                    pass

                def geometry(self, g=None):  # the size the dialog asks for
                    if g:
                        self.asked = g
                    return super().geometry(g)

            L.tk.Toplevel = Hidden
            try:
                app.open_party_rules_dialog()
            finally:
                L.tk.Toplevel = real
            dlg = made[0]
            dlg.update_idletasks()
            body, bar = dlg.party_rules_parts
            width, height = (int(x) for x in dlg.asked.split("+")[0].split("x"))
            # The window: 700 px of client area plus the title bar and the taskbar fit 768.
            self.assertLessEqual(height, 700)
            self.assertLessEqual(width, 1366)
            # Everything is reachable: the bar always shows, the sections scroll inside the rest.
            self.assertLess(bar.winfo_reqheight() + 10 + 150, height)  # the bar and 150+ px of sections
            print("rules dialog %dx%d: sections need %d px (scrolled), the bar %d px" % (
                width, height, body.winfo_reqheight(), bar.winfo_reqheight()))
            # All four rot switches are in it.
            texts = []

            def walk(w):
                for c in w.winfo_children():
                    try:
                        texts.append(str(c.cget("text")))
                    except tk.TclError:
                        pass
                    walk(c)
            walk(body)
            for t in ("Rot on death", "    A boss kill cleanses it", "    Killing your killer cleanses it",
                      "    Retrieving your Blood Echoes cleanses it"):
                self.assertIn(t, texts)
        finally:
            app.destroy()


if __name__ == "__main__":
    unittest.main()
