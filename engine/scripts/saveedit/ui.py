"""The launcher's "Save editor" tab (tkinter): edit a single-player or party character save.

Pages: Character (name, stats, level, Echoes, Insight, NG cycle), Items (counts, remove, weapon level),
Progress (boss kills, lamps, ending flags, any flag by id), Party (marks and one-click repairs).
All edits are staged in a scripts/saveedit SaveModel and written by Apply after an automatic backup;
Backups... lists them with Restore. Nothing is written while the game runs. Only fields whose offsets are
pinned in docs/party/save_layout.md are editable; the rest of the save is never touched.
"""
import tkinter as tk
from pathlib import Path
from tkinter import ttk, messagebox

from . import model as M, store as S, names as N, flagsdata as F

RISK_WARNING = ("Editing a save can make the game crash or break the save (cut content, unobtainable items "
                "and values beyond the normal range are allowed here).\n\nA backup of the whole save folder is "
                "made automatically before every write, and Backups... puts it back.\n\nContinue?")
PARTY_WARNING = ("This is a party save. In a party, shared progress (lamps, bosses, quests, key items) follows "
                 "the host: what you change here can be overwritten or undone at the next join.\n\nContinue?")
EMPTY_WEAPONS = (1000, 1000000)  # Bare Fists and its left-hand twin: what an empty hand slot holds
KIND_FILTERS = (("All", None), ("Weapons", 0), ("Armor", 1), ("Items", 4), ("Runes and gems", 8))
NAMES = {"vigor": "Vitality", "endurance": "Endurance", "strength": "Strength", "skill": "Skill",
         "bloodtinge": "Bloodtinge", "arcane": "Arcane"}


class SaveEditorTab(ttk.Frame):
    def __init__(self, parent, get_user, get_game_dir, get_cache_dir, running, log=print, update_party=None,
                 confirm=None):
        super().__init__(parent, style="Card.TFrame", padding=(14, 10, 14, 12))
        self.get_user, self.get_game_dir, self.get_cache_dir = get_user, get_game_dir, get_cache_dir
        self.running, self.log, self.update_party = running, log, update_party
        self.confirm = confirm or messagebox.askokcancel
        self.model, self.kind, self.slot = None, "solo", None
        self.catalogue = N.Catalogue()
        self.risk_ok = False  # one "may break your save" confirm per session
        self.columnconfigure(0, weight=1)
        self.rowconfigure(1, weight=1)
        self.kind_var = tk.StringVar(value="solo")
        self.slot_var = tk.StringVar()
        self.status = tk.StringVar(value="Pick a save and press Open.")
        self._top()
        self.pages = ttk.Notebook(self)
        self.pages.grid(row=1, column=0, sticky="nsew", pady=(8, 6))
        self._character_page()
        self._items_page()
        self._progress_page()
        self._party_page()
        bar = ttk.Frame(self, style="Card.TFrame")
        bar.grid(row=2, column=0, sticky="ew")
        self.apply_btn = ttk.Button(bar, text="Apply changes", style="Secondary.TButton", command=self.apply)
        self.apply_btn.pack(side="left")
        self.revert_btn = ttk.Button(bar, text="Revert", style="Secondary.TButton", command=self.open_slot)
        self.revert_btn.pack(side="left", padx=6)
        ttk.Label(bar, textvariable=self.status).pack(side="left", padx=10)
        self._set_enabled(False)

    # --- layout ------------------------------------------------------------------------------------
    def _top(self):
        top = ttk.Frame(self, style="Card.TFrame")
        top.grid(row=0, column=0, sticky="ew")
        ttk.Radiobutton(top, text="Single-player", value="solo", variable=self.kind_var, command=self.refresh_slots).pack(side="left")
        ttk.Radiobutton(top, text="Party", value="party", variable=self.kind_var, command=self.refresh_slots).pack(side="left", padx=(8, 12))
        ttk.Label(top, text="Slot").pack(side="left")
        self.slot_box = ttk.Combobox(top, textvariable=self.slot_var, state="readonly", width=34)
        self.slot_box.pack(side="left", padx=6)
        ttk.Button(top, text="Open", style="Secondary.TButton", command=self.open_slot).pack(side="left")
        ttk.Button(top, text="Backups...", style="Secondary.TButton", command=self.backups_dialog).pack(side="left", padx=6)

    def _character_page(self):
        p = ttk.Frame(self.pages, padding=10)
        self.pages.add(p, text="Character")
        self.name_var = tk.StringVar()
        self.stat_vars = {k: tk.StringVar() for k in M.STAT_NAMES}
        self.level_var = tk.StringVar()
        self.hp_var = tk.StringVar()
        self.echoes_var, self.insight_var, self.ng_var = tk.StringVar(), tk.StringVar(), tk.StringVar()
        ttk.Label(p, text="Name").grid(row=0, column=0, sticky="w")
        self.name_entry = ttk.Entry(p, textvariable=self.name_var, width=20)
        self.name_entry.grid(row=0, column=1, sticky="w", pady=2)
        self.stat_boxes = {}
        for i, k in enumerate(M.STAT_NAMES):
            ttk.Label(p, text=NAMES[k]).grid(row=1 + i, column=0, sticky="w")
            sb = ttk.Entry(p, width=6, textvariable=self.stat_vars[k])
            sb.grid(row=1 + i, column=1, sticky="w", pady=2)
            sb.bind("<KeyRelease>", lambda e: self._stats_changed())
            self.stat_boxes[k] = sb
        ttk.Label(p, text="Level (calculated from the stats)").grid(row=7, column=0, sticky="w")
        ttk.Label(p, textvariable=self.level_var).grid(row=7, column=1, sticky="w")
        ttk.Label(p, text="HP (the game refills it)").grid(row=8, column=0, sticky="w")
        ttk.Label(p, textvariable=self.hp_var).grid(row=8, column=1, sticky="w")
        self.num_boxes = []
        for r, (label, var) in enumerate((("Blood Echoes", self.echoes_var), ("Insight", self.insight_var),
                                          ("NG cycle", self.ng_var)), start=9):
            ttk.Label(p, text=label).grid(row=r, column=0, sticky="w")
            e = ttk.Entry(p, textvariable=var, width=12)
            e.grid(row=r, column=1, sticky="w", pady=2)
            self.num_boxes.append(e)

    def _items_page(self):
        p = ttk.Frame(self.pages, padding=10)
        self.pages.add(p, text="Items")
        p.columnconfigure(0, weight=1)
        p.rowconfigure(1, weight=1)
        bar = ttk.Frame(p)
        bar.grid(row=0, column=0, sticky="ew")
        self.search_var = tk.StringVar()
        self.filter_var = tk.StringVar(value="All")
        e = ttk.Entry(bar, textvariable=self.search_var, width=24)
        e.pack(side="left")
        e.bind("<KeyRelease>", lambda ev: self.fill_items())
        ttk.Label(bar, text="  Show").pack(side="left")
        cb = ttk.Combobox(bar, textvariable=self.filter_var, values=[n for n, _ in KIND_FILTERS], state="readonly", width=14)
        cb.pack(side="left", padx=4)
        cb.bind("<<ComboboxSelected>>", lambda ev: self.fill_items())
        ttk.Style().configure("SaveEdit.Treeview", rowheight=int(self.winfo_fpixels("1i") / 96 * 22) + 4)
        self.tree = ttk.Treeview(p, columns=("kind", "count", "level"), show="tree headings", height=14, selectmode="browse",
                                 style="SaveEdit.Treeview")
        self.tree.heading("#0", text="Name")
        self.tree.heading("kind", text="Kind")
        self.tree.heading("count", text="Count")
        self.tree.heading("level", text="Level")
        self.tree.column("#0", width=340)
        for c, w in (("kind", 90), ("count", 60), ("level", 50)):
            self.tree.column(c, width=w, anchor="w")
        self.tree.grid(row=1, column=0, sticky="nsew", pady=6)
        sb = ttk.Scrollbar(p, command=self.tree.yview)
        sb.grid(row=1, column=1, sticky="ns")
        self.tree.configure(yscrollcommand=sb.set)
        row = ttk.Frame(p)
        row.grid(row=2, column=0, sticky="w")
        self.count_var, self.wlevel_var = tk.StringVar(value="1"), tk.StringVar(value="0")
        ttk.Label(row, text="Count").pack(side="left")
        ttk.Entry(row, textvariable=self.count_var, width=7).pack(side="left", padx=4)
        self.set_count_btn = ttk.Button(row, text="Set count", style="Secondary.TButton", command=self.set_count)
        self.set_count_btn.pack(side="left")
        self.remove_btn = ttk.Button(row, text="Remove", style="Secondary.TButton", command=self.remove_item)
        self.remove_btn.pack(side="left", padx=(12, 0))
        row2 = ttk.Frame(p)
        row2.grid(row=3, column=0, sticky="w", pady=(4, 0))
        ttk.Label(row2, text="Weapon level").pack(side="left")
        ttk.Entry(row2, textvariable=self.wlevel_var, width=4).pack(side="left", padx=4)
        self.set_level_btn = ttk.Button(row2, text="Set level", style="Secondary.TButton", command=self.set_level)
        self.set_level_btn.pack(side="left")

    def _progress_page(self):
        p = ttk.Frame(self.pages, padding=10)
        self.pages.add(p, text="Progress")
        p.columnconfigure(0, weight=1)
        p.rowconfigure(1, weight=1)
        self.group_var = tk.StringVar(value="Bosses")
        bar = ttk.Frame(p)
        bar.grid(row=0, column=0, sticky="ew")
        cb = ttk.Combobox(bar, textvariable=self.group_var, values=["Bosses", "Lamps", "Endings"], state="readonly", width=12)
        cb.pack(side="left")
        cb.bind("<<ComboboxSelected>>", lambda ev: self.fill_flags())
        self.flag_tree = ttk.Treeview(p, columns=("state",), show="tree headings", height=14, selectmode="browse",
                                      style="SaveEdit.Treeview")
        self.flag_tree.heading("#0", text="Flag")
        self.flag_tree.heading("state", text="Set")
        self.flag_tree.column("#0", width=380)
        self.flag_tree.column("state", width=60)
        self.flag_tree.grid(row=1, column=0, sticky="nsew", pady=6)
        row = ttk.Frame(p)
        row.grid(row=2, column=0, sticky="w")
        self.toggle_btn = ttk.Button(row, text="Set / clear selected", style="Secondary.TButton", command=self.toggle_flag)
        self.toggle_btn.pack(side="left")
        self.lamps_btn = ttk.Button(row, text="Light all lamps", style="Secondary.TButton", command=self.light_lamps)
        self.lamps_btn.pack(side="left", padx=6)
        row = ttk.Frame(p)
        row.grid(row=3, column=0, sticky="w", pady=(4, 0))
        ttk.Label(row, text="Flag id").pack(side="left")
        self.fid_var = tk.StringVar()
        ttk.Entry(row, textvariable=self.fid_var, width=12).pack(side="left", padx=4)
        self.fid_get = ttk.Button(row, text="Show", style="Secondary.TButton", command=self.show_flag)
        self.fid_get.pack(side="left")
        self.fid_set = ttk.Button(row, text="Set", style="Secondary.TButton", command=lambda: self.set_flag_id(True))
        self.fid_set.pack(side="left", padx=2)
        self.fid_clear = ttk.Button(row, text="Clear", style="Secondary.TButton", command=lambda: self.set_flag_id(False))
        self.fid_clear.pack(side="left")

    def _party_page(self):
        p = ttk.Frame(self.pages, padding=10)
        self.pages.add(p, text="Party")
        self.party_text = tk.Text(p, height=10, width=70, wrap="word", bg="#1c1c1e", fg="#e6e6e6", relief="flat",
                                  font=("Segoe UI", 10))
        self.party_text.pack(fill="x")
        self.party_text.configure(state="disabled")
        row = ttk.Frame(p)
        row.pack(anchor="w", pady=6)
        self.phantom_btn = ttk.Button(row, text="Repair phantom mark", style="Secondary.TButton", command=self.repair_phantom)
        self.phantom_btn.pack(side="left")
        self.dup_btn = ttk.Button(row, text="Merge duplicate key items", style="Secondary.TButton", command=self.merge_dups)
        self.dup_btn.pack(side="left", padx=6)

    def _set_enabled(self, on):
        state = "normal" if on else "disabled"
        widgets = [self.apply_btn, self.revert_btn, self.name_entry, self.set_count_btn, self.set_level_btn, self.remove_btn,
                   self.toggle_btn, self.lamps_btn, self.fid_get, self.fid_set, self.fid_clear, self.phantom_btn, self.dup_btn,
                   *self.stat_boxes.values(), *self.num_boxes]
        for w in widgets:
            w.configure(state=state)
        if on and not self.catalogue.valid_ids(0):
            self.set_level_btn.configure(state="disabled")  # needs the game's weapon list

    # --- slots ---------------------------------------------------------------------------------------
    def refresh_slots(self):
        self.kind = self.kind_var.get()
        user = self.get_user()
        rows = S.list_slots(user, self.kind)
        self._slots = {f"{r['slot']}: {r['name']}  level {r['level']}": r["slot"] for r in rows}
        self.slot_box.configure(values=list(self._slots))
        self.slot_var.set(next(iter(self._slots), ""))
        self.status.set("" if rows else f"No character saves in {Path(user) / S.KINDS[self.kind]}")
        self._set_enabled(False)
        self.model = None

    def open_slot(self):
        if not getattr(self, "_slots", None):
            self.refresh_slots()
        slot = self._slots.get(self.slot_var.get())
        if slot is None:
            return
        self.catalogue = N.Catalogue.load(self.get_game_dir(), self.get_cache_dir())
        try:
            self.model = S.open_slot(self.get_user(), self.kind, slot)
        except (OSError, M.SaveFormatError) as ex:
            messagebox.showerror("Save editor", f"This save cannot be opened:\n{ex}")
            return
        self.slot = slot
        self._set_enabled(True)
        self.load_character()
        self.fill_items()
        self.fill_flags()
        self.fill_party()
        names = "game names loaded" if self.catalogue.ok else "game folder not found: ids only, weapon levels read-only"
        self.status.set(f"{self.model.name}: {names}")

    # --- character --------------------------------------------------------------------------------------
    def load_character(self):
        m = self.model
        self.name_var.set(m.name)
        for k, v in m.stats.items():
            self.stat_vars[k].set(str(v))
        self.level_var.set(str(m.level))
        self.hp_var.set(f"{m.hp[0]} / {m.hp[1]}")
        self.echoes_var.set(str(m.echoes))
        self.insight_var.set(str(m.insight))
        self.ng_var.set(str(m.ng_cycle))

    def _stats_changed(self):
        try:
            self.level_var.set(str(M.SaveModel.level_for({k: int(v.get()) for k, v in self.stat_vars.items()})))
        except ValueError:
            self.level_var.set("?")

    def _stage_character(self):
        """Copy the Character page into the model; ValueError names what is wrong."""
        m = self.model
        if self.name_var.get() != m.name:
            m.set_name(self.name_var.get())
        for k, var in self.stat_vars.items():
            if int(var.get()) != m.stats[k]:
                m.set_stat(k, int(var.get()))
        if int(self.echoes_var.get()) != m.echoes:
            m.set_echoes(int(self.echoes_var.get()))
        if int(self.insight_var.get()) != m.insight:
            m.set_insight(int(self.insight_var.get()))
        if int(self.ng_var.get()) != m.ng_cycle:
            m.set_ng_cycle(int(self.ng_var.get()))

    # --- items ------------------------------------------------------------------------------------------
    def _item_name(self, it):
        n = self.catalogue.name(it.category, it.number)
        return n if n else f"({it.kind} {it.number})"

    def fill_items(self):
        self.tree.delete(*self.tree.get_children())
        if self.model is None:
            return
        want = dict(KIND_FILTERS)[self.filter_var.get()]
        text = self.search_var.get().lower()
        rows = []
        empty_weapons = 0
        for it in self.model.items():
            if it.category == 0 and it.number in EMPTY_WEAPONS:
                empty_weapons += 1  # the bare-hand placeholder entries: not real weapons
                continue
            if want is not None and it.category != want:
                continue
            name = self._item_name(it)
            if text and text not in name.lower() and text not in str(it.number):
                continue
            rows.append((self.catalogue.name(it.category, it.number) is None, it.section, it.category, name, it))
        if empty_weapons and want in (None, 0) and not text:
            self.tree.insert("", "end", iid="empty", text=f"(empty weapon slot x{empty_weapons}, bare hands)",
                             values=("(empty)", "", ""))
        for unnamed, sec, cat, name, it in sorted(rows, key=lambda r: (r[0], r[1], r[2], r[3])):
            kind = it.kind + (" (key)" if it.section else "")
            self.tree.insert("", "end", iid=f"{it.section}:{it.slot}", text=name,
                             values=(kind, it.count, "" if it.level is None else f"+{it.level}"))

    def _selected_item(self):
        sel = self.tree.selection()
        if not sel or sel[0] == "empty":
            return None
        sec, slot = (int(x) for x in sel[0].split(":"))
        return next((i for i in self.model.items((sec,)) if i.slot == slot), None)

    def _risk(self):
        if self.risk_ok:
            return True
        self.risk_ok = bool(self.confirm("Save editor", RISK_WARNING))
        return self.risk_ok

    def set_count(self):
        it = self._selected_item()
        if it is None or not self._risk():
            return
        try:
            self.model.set_count(it, int(self.count_var.get()))
        except ValueError as ex:
            messagebox.showerror("Save editor", str(ex))
        self.fill_items()

    def set_level(self):
        it = self._selected_item()
        if it is None or it.category != 0 or not self._risk():
            return
        try:
            self.model.set_weapon_level(it, int(self.wlevel_var.get()), self.catalogue.valid_ids(0))
        except ValueError as ex:
            messagebox.showerror("Save editor", str(ex))
        self.fill_items()

    def remove_item(self):
        it = self._selected_item()
        if it is None or not self._risk():
            return
        self.model.remove(it)
        self.fill_items()
        self.fill_party()

    # --- flags ------------------------------------------------------------------------------------------
    def fill_flags(self):
        self.flag_tree.delete(*self.flag_tree.get_children())
        if self.model is None:
            return
        which = self.group_var.get()
        rows = F.bosses(self.catalogue) if which == "Bosses" else F.lamps(self.catalogue) if which == "Lamps" else F.endings()
        groups = {}
        for fid, group, label in rows:
            if group not in groups:
                groups[group] = self.flag_tree.insert("", "end", iid=f"g:{group}", text=group, open=True)
            self.flag_tree.insert(groups[group], "end", iid=str(fid), text=label,
                                  values=("yes" if self.model.flag(fid) else "no",))

    def toggle_flag(self):
        sel = [i for i in self.flag_tree.selection() if not i.startswith("g:")]
        if sel and self._risk():
            fid = int(sel[0])
            self.model.set_flag(fid, not self.model.flag(fid))
            self.fill_flags()

    def light_lamps(self):
        if self._risk():
            for fid in F.LAMP_IDS:
                self.model.set_flag(fid, True)
            self.fill_flags()

    def _fid(self):
        try:
            return int(self.fid_var.get())
        except ValueError:
            messagebox.showerror("Save editor", "A flag id is a number, e.g. 12401800.")
            return None

    def show_flag(self):
        fid = self._fid()
        if fid is not None:
            v = self.model.flag(fid)
            self.status.set(f"Flag {fid}: " + ("this id has no place in the save" if v is None else "set" if v else "clear"))

    def set_flag_id(self, on):
        fid = self._fid()
        if fid is None or not self._risk():
            return
        try:
            self.model.set_flag(fid, on)
        except ValueError as ex:
            messagebox.showerror("Save editor", str(ex))
        self.fill_flags()
        self.show_flag()

    # --- party ------------------------------------------------------------------------------------------
    def party_findings(self):
        m, out = self.model, []
        if m.phantom_mark:
            out.append("Phantom mark: this save was last written as a summoned guest. Played alone it can soft lock "
                       "the own world. Repair sets the mark to 0.")
        dups = m.duplicate_keys()
        if dups:
            out.append("Key items in two stacks: " + ", ".join(f"{self.catalogue.name(4, d) or d}" for d in dups) +
                       ". Merge keeps one stack of each.")
        if m.world_empty:
            out.append("Written in another player's world (empty world-state section). The editor leaves that "
                       "section alone; other save editors need the launcher's Export for save editor.")
        return out or ["Nothing to repair."]

    def fill_party(self):
        self.party_text.configure(state="normal")
        self.party_text.delete("1.0", "end")
        head = ("Party save: shared progress follows the host in a party.\n\n" if self.kind == "party" else "")
        self.party_text.insert("1.0", head + "\n\n".join(self.party_findings()))
        self.party_text.configure(state="disabled")

    def repair_phantom(self):
        self.model.repair_phantom_mark()
        self.fill_party()

    def merge_dups(self):
        self.model.merge_duplicate_keys()
        self.fill_items()
        self.fill_party()

    # --- write ------------------------------------------------------------------------------------------
    def apply(self):
        if self.model is None:
            return
        try:
            self._stage_character()
        except ValueError as ex:
            messagebox.showerror("Save editor", str(ex))
            return
        changes = self.model.diff()
        if not changes:
            self.status.set("Nothing changed.")
            return
        if not self.risk_ok and not self._risk():
            return
        if self.kind == "party" and not self.confirm("Save editor", PARTY_WARNING):
            return
        try:
            backup = S.apply_edit(self.get_user(), self.kind, self.slot, self.model, self.running, self.update_party)
        except S.GameRunning as ex:
            messagebox.showwarning("Save editor", f"{ex}.")
            return
        except (OSError, S.EditConflict, M.SaveFormatError) as ex:
            messagebox.showerror("Save editor", f"Nothing was changed (the old save is back):\n{ex}")
            return
        self.log(f"[SAVE EDITOR] {self.kind} slot {self.slot} written ({len(changes)} range(s)); backup {backup}")
        self.status.set(f"Saved. Backup: {backup.name}")
        keep = self.slot
        self.refresh_slots()
        for label, slot in self._slots.items():
            if slot == keep:
                self.slot_var.set(label)
        self.open_slot()

    def backups_dialog(self):
        user = self.get_user()
        win = tk.Toplevel(self)
        win.title("Save editor backups")
        win.configure(bg="#1c1c1e")
        lb = tk.Listbox(win, width=60, height=12, bg="#1c1c1e", fg="#e6e6e6", selectbackground="#9a0000", relief="flat")
        lb.pack(padx=10, pady=10)
        rows = S.list_backups(user, self.kind)
        for r in rows:
            lb.insert("end", r["name"])

        def restore():
            sel = lb.curselection()
            if not sel:
                return
            r = rows[sel[0]]
            if not self.confirm("Restore backup", f"Put back the {S.KINDS[self.kind]} folder as of {r['name']}?\n"
                                "The current folder is kept as a before-restore backup first."):
                return
            try:
                S.restore_backup(user, self.kind, r["path"], self.running)
            except S.GameRunning as ex:
                messagebox.showwarning("Save editor", f"{ex}.", parent=win)
                return
            except OSError as ex:
                messagebox.showerror("Save editor", str(ex), parent=win)
                return
            self.log(f"[SAVE EDITOR] restored {r['path']}")
            win.destroy()
            self.refresh_slots()
            self.status.set("Restored.")

        ttk.Button(win, text="Restore selected", command=restore).pack(pady=(0, 10))
