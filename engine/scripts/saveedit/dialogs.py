"""Dialogs of the save editor: Add item, gem effects, add blood gem, add rune (tkinter, launcher style).

Each dialog calls back into the SaveEditorTab (ui.py), which owns the model, asks the once-per-session risk
confirm and refreshes its lists. Keyboard: type to search, Down moves into the list, Enter adds / applies,
Escape closes; the buttons are plain ttk buttons, so a gamepad mapped to keys works the same.
"""
import tkinter as tk
from tkinter import ttk, messagebox

from . import gearops as G

BG, FG, SEL = "#1c1c1e", "#e6e6e6", "#9a0000"
ADD_CATEGORIES = (("Weapons", 0), ("Armor", 1), ("Items", 4))
SHAPE_NAMES = {1: "Radial", 2: "Triangle", 4: "Waning", 8: "Circle", 63: "Droplet"}
NO_EFFECT = "(none)"


def _window(tab, title, size="520x460"):
    win = tk.Toplevel(tab)
    win.title(title)
    win.configure(bg=BG)
    win.geometry(size)
    win.transient(tab.winfo_toplevel())
    win.bind("<Escape>", lambda e: win.destroy())
    return win


def _listbox(win, height=14):
    frame = ttk.Frame(win)
    lb = tk.Listbox(frame, height=height, bg=BG, fg=FG, selectbackground=SEL, relief="flat", exportselection=False,
                    activestyle="none")
    sb = ttk.Scrollbar(frame, command=lb.yview)
    lb.configure(yscrollcommand=sb.set)
    lb.pack(side="left", fill="both", expand=True)
    sb.pack(side="right", fill="y")
    return frame, lb


class AddItemDialog:
    """Weapons (added at +0), armor and items from the game's own list; stays open to add several."""

    def __init__(self, tab):
        self.tab = tab
        self.win = _window(tab, "Add item")
        self.cat_var, self.text_var, self.count_var = tk.StringVar(value="Weapons"), tk.StringVar(), tk.StringVar(value="1")
        self.status = tk.StringVar(value="Pick an item and press Add. Weapons come at +0; raise the level afterwards.")
        top = ttk.Frame(self.win)
        top.pack(fill="x", padx=10, pady=(10, 4))
        cb = ttk.Combobox(top, textvariable=self.cat_var, values=[n for n, _ in ADD_CATEGORIES], state="readonly", width=10)
        cb.pack(side="left")
        cb.bind("<<ComboboxSelected>>", lambda e: self.fill())
        self.entry = ttk.Entry(top, textvariable=self.text_var, width=28)
        self.entry.pack(side="left", padx=6)
        self.entry.bind("<KeyRelease>", lambda e: self.fill())
        self.entry.bind("<Down>", lambda e: (self.lb.focus_set(), self.lb.selection_set(0)))
        frame, self.lb = _listbox(self.win)
        frame.pack(fill="both", expand=True, padx=10, pady=4)
        self.lb.bind("<Double-Button-1>", lambda e: self.add())
        self.lb.bind("<Return>", lambda e: self.add())
        row = ttk.Frame(self.win)
        row.pack(fill="x", padx=10, pady=4)
        ttk.Label(row, text="Count (items)").pack(side="left")
        ttk.Entry(row, textvariable=self.count_var, width=5).pack(side="left", padx=4)
        ttk.Button(row, text="Add", style="Secondary.TButton", command=self.add).pack(side="left", padx=6)
        ttk.Button(row, text="Close", style="Secondary.TButton", command=self.win.destroy).pack(side="left")
        ttk.Label(self.win, textvariable=self.status, wraplength=480).pack(anchor="w", padx=10, pady=(0, 8))
        self.rows = []
        self.fill()
        self.entry.focus_set()

    def category(self):
        return dict(ADD_CATEGORIES)[self.cat_var.get()]

    def fill(self):
        self.lb.delete(0, "end")
        self.rows = G.catalogue_rows(self.tab.catalogue, self.category(), self.text_var.get(), limit=400)
        for n, name in self.rows:
            self.lb.insert("end", f"{name}  [{n}]")
        if not self.tab.catalogue.ok:
            self.status.set("The game folder was not found: the item list needs the game's own files.")

    def add(self):
        sel = self.lb.curselection()
        if not sel:
            return
        number, name = self.rows[sel[0]]
        try:
            count = int(self.count_var.get()) if self.category() == 4 else 1
        except ValueError:
            self.status.set("The count is a number.")
            return
        if self.tab.add_item(self.category(), number, count):
            self.status.set(f"Added {name}. Press Apply changes in the editor to write it.")


class GemDialog:
    """Edit the effects of a blood gem, or compose a new one (shape and up to 3 effects)."""

    def __init__(self, tab, handle=None):
        self.tab, self.handle = tab, handle
        cat = tab.catalogue
        self.effects = cat.effects_for(0)
        self.label = {e: f"{t}" for e, t in self.effects}
        self.win = _window(tab, "Blood gem effects" if handle else "Add blood gem", "560x300")
        self.shape_var = tk.StringVar(value="Radial")
        self.filter_var = tk.StringVar()
        top = ttk.Frame(self.win)
        top.pack(fill="x", padx=10, pady=10)
        if handle is None:
            ttk.Label(top, text="Shape").pack(side="left")
            ttk.Combobox(top, textvariable=self.shape_var, state="readonly", width=10,
                         values=[SHAPE_NAMES[s] for s in cat.ADD_SHAPES if cat.blood_gem_gen(s)]).pack(side="left", padx=6)
        ttk.Label(top, text="Filter").pack(side="left", padx=(12, 0))
        f = ttk.Entry(top, textvariable=self.filter_var, width=18)
        f.pack(side="left", padx=6)
        f.bind("<KeyRelease>", lambda e: self.refilter())
        self.vars, self.boxes = [], []
        current = []
        if handle is not None:
            row = next((r for r in G.gem_rows(tab.model, cat) if r["handle"] == handle), None)
            current = [e for e, _ in row["effects"]] if row else []
        for k in range(G.GEM_EFFECTS):
            v = tk.StringVar(value=NO_EFFECT)
            if k < len(current):
                v.set(self._text(current[k]))
            elif handle is None and k == 0 and self.effects:
                v.set(self._text(self.effects[0][0]))
            cb = ttk.Combobox(self.win, textvariable=v, width=60)
            cb.pack(padx=10, pady=3, anchor="w")
            self.vars.append(v)
            self.boxes.append(cb)
        self.refilter()
        row = ttk.Frame(self.win)
        row.pack(fill="x", padx=10, pady=8)
        ttk.Button(row, text="Apply" if handle else "Add", style="Secondary.TButton", command=self.ok).pack(side="left")
        ttk.Button(row, text="Close", style="Secondary.TButton", command=self.win.destroy).pack(side="left", padx=6)
        self.status = tk.StringVar()
        ttk.Label(self.win, textvariable=self.status, wraplength=520).pack(anchor="w", padx=10)

    def _text(self, effect_id):
        return f"{self.label.get(effect_id) or '(effect %d)' % effect_id}  [{effect_id}]"

    def refilter(self):
        want = self.filter_var.get().lower()
        values = [self._text(e) for e, t in self.effects if want in t.lower()] or [NO_EFFECT]
        for k, cb in enumerate(self.boxes):
            cb.configure(values=([NO_EFFECT] if k else []) + values)

    def chosen(self):
        out = []
        for v in self.vars:
            text = v.get().strip()
            if text and text != NO_EFFECT:
                try:
                    out.append(int(text.rsplit("[", 1)[1].rstrip("]")))
                except (IndexError, ValueError):
                    raise ValueError(f"pick the effects from the list: {text}")
        return out

    def ok(self):
        try:
            effects = self.chosen()
            if self.handle is not None:
                done = self.tab.set_gem_effects(self.handle, effects)
            else:
                shape = next(s for s, n in SHAPE_NAMES.items() if n == self.shape_var.get())
                done = self.tab.add_blood_gem(shape, effects)
        except ValueError as ex:
            self.status.set(str(ex))
            return
        if done:
            self.win.destroy()


class RuneDialog:
    """Add a Caryll or oath rune from the game's own rune list."""

    def __init__(self, tab):
        self.tab = tab
        self.win = _window(tab, "Add rune")
        self.rows = G.rune_rows(tab.catalogue)
        self.text_var = tk.StringVar()
        top = ttk.Frame(self.win)
        top.pack(fill="x", padx=10, pady=(10, 4))
        e = ttk.Entry(top, textvariable=self.text_var, width=28)
        e.pack(side="left")
        e.bind("<KeyRelease>", lambda ev: self.fill())
        frame, self.lb = _listbox(self.win)
        frame.pack(fill="both", expand=True, padx=10, pady=4)
        self.lb.bind("<Double-Button-1>", lambda ev: self.add())
        self.lb.bind("<Return>", lambda ev: self.add())
        row = ttk.Frame(self.win)
        row.pack(fill="x", padx=10, pady=8)
        ttk.Button(row, text="Add", style="Secondary.TButton", command=self.add).pack(side="left")
        ttk.Button(row, text="Close", style="Secondary.TButton", command=self.win.destroy).pack(side="left", padx=6)
        self.status = tk.StringVar(value="Runes carry the effects the game gives them; they are not edited." if self.rows else
                                   "The game's rune list needs the game folder.")
        ttk.Label(self.win, textvariable=self.status, wraplength=480).pack(anchor="w", padx=10)
        self.shown = []
        self.fill()
        e.focus_set()

    def fill(self):
        want = self.text_var.get().lower()
        cat = self.tab.catalogue
        self.lb.delete(0, "end")
        self.shown = []
        for effect, name, shape in self.rows:
            text = cat.effect_text(effect) or ""
            if want in name.lower() or want in text.lower():
                self.shown.append(effect)
                self.lb.insert("end", (f"{name}: {text}" if text else name) + f"  [{effect}]")  # tiers read alike: the id tells them apart

    def add(self):
        sel = self.lb.curselection()
        if sel and self.tab.add_rune(self.shown[sel[0]]):
            self.status.set("Added. Press Apply changes in the editor to write it.")
