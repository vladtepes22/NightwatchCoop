#!/usr/bin/env python3
"""A real-Internet party session on ONE PC (~12-15 min): the host on the normal connection, guest 1
through a VPN's per-app split tunnel, guest 2 on the LAN (the member relay for guest 1).

  python tools/mp/realnet_party.py --print-exe          (setup only: the exe paths, nothing runs)
  python tools/mp/realnet_party.py [--phase direct,relay] [--root DIR] [--out DIR] [--guest2-headless]
  python tools/mp/realnet_party.py --check-only --out DIR   (verdict again from the saved logs)
  python tools/mp/realnet_party.py --dry-run                (the instances.py commands, nothing runs)

Before a run (the player, once):
  1. VPN app with per-app split tunnelling in include mode (Proton VPN: Settings > Split tunneling >
     Include mode, add ROOT\\inst1\\out\\bbport.exe only; reconnect). Connect to any server whose exit
     address is not this PC's (a nearby country keeps the ping low). UDP protocols (WireGuard /
     OpenVPN UDP) are best. Kill switch off (a dropped tunnel then shows up as "SAME public IPv4").
  2. Windows Firewall: each of the three exes asks once (the prompt can open behind the game
     windows): Allow on the network type this PC's adapter has (Get-NetConnectionProfile), or add the
     rules beforehand in an admin PowerShell (New-NetFirewallRule -Direction Inbound -Program <exe>
     -Action Allow -Profile Private,Public), one per exe.
  3. UPnP on the router (the host maps its port; the relay phase maps guest 2's UDP port).

Instances (tools/mp/instances.py --real-net, root ROOT, default agents/NAME/realnet):
  inst0  host     ROOT\\inst0\\out\\bbport.exe  normal connection: UPnP, public STUN, the Internet code
  inst1  guest 1  ROOT\\inst1\\out\\bbport.exe  THE ONE IN THE VPN TUNNEL: joins the host's public
                                               address from the VPN's exit server; LAN discovery off
                                               (BB_PARTY_LAN_DISCOVERY=0), so it never takes the LAN
  inst2  guest 2  ROOT\\inst2\\out\\bbport.exe  normal connection: same public address as the host,
                                               so the game's LAN fallback finds the host on the LAN

Phase direct (~9 min, 3 players): guest 1 title-joins over the Internet; Party > Leave and Rejoin (an
  in-world direct summon), after guest 1 died outside a fight (bloodstain, respawn at the host's lamp); a
  party lamp travel to the Tomb of Oedon; two host kills and one guest kill there (the kill record on
  every game); the host dies outside a fight (its revive at the lamp).
Phase relay (~4 min, 3 players): the host with NO UPnP mapping on new ports (closed to the Internet:
  guest 1 cannot reach it); this script maps guest 2's party UDP port on the router by UPnP (Windows'
  HNetCfg.NATUPnP; --no-relay-upnp if you forwarded it by hand); guest 2 starts first and joins over
  the LAN; guest 1 starts once guest 2 is in, with BB_PARTY_RELAY_VIA=<this PC's public IPv4>:<guest
  2's port>: its direct attempts fail and guest 2 relays for it. A party lamp travel, one host and one
  guest kill through the relay. The mapping is removed afterwards (and the direct phase's host
  mapping, which a killed host leaves on the router).

Every phase: the address lines of each game as they appear (public IPv4 by STUN, the host's view of
each guest, the link's path) and ROOT\\realnet_report.txt; the run stops early when guest 1's public
IPv4 equals the host's (the VPN tunnel is not taking guest 1). Screenshots of every rendered game every
10 s and at the key moments in OUT/<phase>/shots; OUT/verdict.txt. LOOK AT THE SCREENSHOTS.
"""
import argparse, os, re, socket, subprocess, sys, threading, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import instances  # noqa: E402
import smoke_party as sp  # noqa: E402

REPO = HERE.parent.parent
AGENT_ROOT = REPO.parent
DEFAULT_ROOT = (AGENT_ROOT / "realnet") if AGENT_ROOT.parent.name == "agents" else (REPO.parent / "realnet")
TOMB = "2412953"  # the Tomb of Oedon lamp (m24_01)
COMMON_ENV = ["BB_PARTY_ENEMY_CENSUS=1"]  # host_kill / guest_kill pick their target from the census
G1_ENV = ["1:BB_PARTY_LAN_DISCOVERY=0"]   # guest 1 never takes the LAN path (it must go through the VPN)
HOST_LAST = r"0:travel step (\d+)/\1: "   # the host chain's last step


def shots_watch(relay=False):
    w = [r"connected to the host as slot=joined@1,5",
         r"title join: descriptor read=titlejoin@4,10",
         r"guest_leave: Party > Leave pressed=leave@2,8",
         r"direct summon: joined the host's room=dsummon@1,6",
         r"travel step \d+/\d+: guest_die=gdeath@2,8,14",
         r"host_warp to lamp %s=travel@2,12,20" % TOMB,
         r"test: host_kill: entity=hkill@1,4",
         r"test: guest_kill: entity=gkill@1,4",
         r"travel step \d+/\d+: host_die=hdeath@2,8,16"]
    if relay:
        w += [r"relays for us=relay@1,8", r"through \S+ relay=relaypath@1,8"]
    out = ["--grey"]
    for x in w:
        out += ["--watch", x]
    return out


def phase_direct(a, port0):
    host = ",".join([
        # Conditions, not long fixed waits: "^2*2" = once guest 1's Rejoin brought the host's cooperators
        # back to 2 (the second rise to 2). Guest 1 dies BEFORE its Leave, so nothing of guest 1's is
        # left to wait for (its death does not change the host's count; deaths071 smoke log).
        "host_warp=%s@3^2*2" % TOMB,           # party lamp travel, 3 s after guest 1's death / Leave / Rejoin
        "host_kill=2410247@3%m24_01",          # two kills of this map's enemies (s3's), steady world + 3 s
        "host_kill=2410202@3%m24_01",
        "host_press=0@10%m24_01!",             # guest 1's kill meanwhile (5 s after its load)
        "host_die@3%m24_01",                   # the host's death outside a fight: revive at the lamp
        "host_press=0@5%m24_01!",              # up again ("!": once alive and the world is up)
        "host_press=0@5%m24_01!",              # the guests' screens of it, then the end
    ])
    g1 = ",".join([
        "echoes=5000",                         # echoes to drop: the bloodstain
        "guest_die@5",                         # a death outside a fight: respawn at the host's lamp
        "guest_leave@3%m",                     # "%m" (any map): steady-world clock, 3 s after the respawn load
        "guest_rejoin@3",                      # Party > Leave, Rejoin: back by a direct summon
        "guest_kill=2410261@5%m24_01",         # its own kill at the Tomb
    ])
    return dict(
        name="direct", seconds=720, count=3, headless=1 if a.guest2_headless else 0,
        env=COMMON_ENV, crash=[],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "2:BB_PARTY_DIRECTOR_TEST=god"]
             + G1_ENV,
        args=["--reserve", "3"],
        until=[HOST_LAST, r"1:travel step (\d+)/\1: "],
        until_hold=10,
        watch=shots_watch(),
        ports=(port0, port0 + 1, port0 + 2),
    )


def phase_relay(a, port0, pub):
    ports = (port0 + 5, port0 + 6, port0 + 7)  # new ports: no router mapping left from the direct phase
    # "^2": the travel 3 s after both guests are in (guest 1 through the relay), not a fixed wait
    host = ",".join(["god", "host_warp=%s@3^2" % TOMB, "host_kill=2410247@3%m24_01",
                     "host_press=0@10%m24_01!", "host_press=0@5%m24_01!"])
    g1 = "god,guest_kill=2410261@3%m24_01"
    return dict(
        name="relay", seconds=420, count=3, headless=1 if a.guest2_headless else 0,
        env=COMMON_ENV, crash=[],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "2:BB_PARTY_DIRECTOR_TEST=god",
              "%d:BB_PARTY_PORT=%d" % (0, ports[0]), "%d:BB_PARTY_PORT=%d" % (1, ports[1]),
              "%d:BB_PARTY_PORT=%d" % (2, ports[2]),
              "1:BB_PARTY_RELAY_VIA=%s:%d" % (pub or "0.0.0.0", ports[2])] + G1_ENV,
        # guest 2 first (it must be in the party before guest 1 asks it to relay)
        args=["--reserve", "3", "--real-net-no-upnp", "--start-order", "0,2,1"],
        start_when=["1/0:cooperators 0 -> 1"],
        until=[HOST_LAST, r"1:travel step (\d+)/\1: "],
        until_hold=10,
        watch=shots_watch(relay=True),
        ports=ports,
    )


# ---- the router (UPnP through Windows' own HNetCfg.NATUPnP, the API the game's UpnpMapper uses) ----

def lan_ipv4():
    """This PC's address on the default route (a UDP connect sends nothing)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 53))
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


def upnp(action, port, proto="UDP", ip=None):
    """'add' / 'remove' one static port mapping (external = internal port). True when it worked."""
    if action == "add":
        op = "$m.Add(%d,'%s',%d,'%s',$true,'bbport realnet test') | Out-Null" % (port, proto, port, ip)
    else:
        op = "try { $m.Remove(%d,'%s') } catch {}" % (port, proto)
    ps = ("$n = New-Object -ComObject HNetCfg.NATUPnP; $m = $null; "
          "for ($k = 0; $k -lt 3 -and $m -eq $null; $k++) { $m = $n.StaticPortMappingCollection; "
          "if ($m -eq $null) { Start-Sleep -Seconds 2 } }; "
          "if ($m -eq $null) { 'NO-UPNP'; exit 2 }; " + op + "; 'OK'")
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError) as e:
        print("[realnet] UPnP %s %s %d: %s" % (action, proto, port, e), flush=True)
        return False
    ok = "OK" in r.stdout
    print("[realnet] UPnP %s %s %d: %s" % (action, proto, port, "done" if ok else
                                          (r.stdout.strip() or r.stderr.strip()[:200] or "failed")), flush=True)
    return ok


# ---- the VPN check while a phase runs ----

class VpnGuard:
    """Tails the instances' logs; once both the host's and guest 1's public IPv4 (STUN) are known and
    equal, guest 1 is not in the VPN tunnel: the run is stopped (instances.py kill) and says so."""

    def __init__(self, root):
        self.root, self.stop, self.tripped = Path(root), False, None
        self.t = threading.Thread(target=self.loop, daemon=True)
        self.t.start()

    def loop(self):
        marks = {}
        for i in (0, 1):
            p = self.root / f"inst{i}" / "run.log"
            marks[i] = (p.stat().st_ino, p.stat().st_size) if p.exists() else (None, 0)
        while not self.stop:
            time.sleep(2)
            pubs = []
            for i, key in ((0, "public (STUN, host)"), (1, "public (STUN, guest)")):
                p = self.root / f"inst{i}" / "run.log"
                try:
                    with open(p, "rb") as f:
                        f.seek(marks[i][1] if os.fstat(f.fileno()).st_ino == marks[i][0] else 0)
                        text = f.read().decode("utf-8", errors="replace")
                except OSError:
                    text = ""
                pubs.append(instances._ip(dict(instances.real_net_lines(text)).get(key)))
            if pubs[0] and pubs[1]:
                if pubs[0] == pubs[1]:
                    self.tripped = ("guest 1 has the host's public IPv4: the VPN's split tunnel is not taking %s "
                                    "(the VPN connected, that exe included?); run stopped"
                                    % (self.root / "inst1" / "out" / "bbport.exe"))
                    print("[realnet] STOP: " + self.tripped, flush=True)
                    subprocess.run([sys.executable, str(HERE / "instances.py"), "kill", "--root", str(self.root)],
                                   capture_output=True)
                else:
                    print("[realnet] VPN check: guest 1 leaves through another public IPv4 than the host: OK", flush=True)
                return

    def close(self):
        self.stop = True


# ---- verdict ----

def check_direct(v, L):
    need, Lf = sp.need, L.find
    need(v, L, "direct join: 3 players (host cooperators -> 2)", 0, r"cooperators [01] -> 2")
    vpn_check(v, L, "direct")
    need(v, L, "direct guest 1: title join", 1, r"title join: descriptor read")
    sp.none_of(v, L, "direct guest 1: no failed title join", r"title join: FAILED", 0, 1e9, (1,))
    lv = need(v, L, "direct Leave (guest 1)", 1, r"guest_leave: Party > Leave pressed")
    if lv is not None:
        need(v, L, "direct Leave: host sees it (2 -> 1)", 0, r"cooperators 2 -> 1", lv)
        rj = need(v, L, "direct Rejoin pressed", 1, r"guest_rejoin: Party > Rejoin pressed", lv)
        if rj is not None:
            need(v, L, "direct Rejoin: a direct summon over the Internet", 1, r"direct summon: joined the host's room", rj)
            back = need(v, L, "direct Rejoin: back in (1 -> 2)", 0, r"cooperators 1 -> 2", rj)
            if back is not None:
                v.step("direct Rejoin time (press -> host room)", back - rj < 45, "%.1f s (limit 45)" % (back - rj))
    d = need(v, L, "direct guest death (scripted)", 1, r"travel step \d+/\d+: guest_die", lv or 0)
    if d is not None:
        need(v, L, "direct guest bloodstain recorded", 1, r"guest: bloodstain recorded", d)
        need(v, L, "direct guest respawned at the host's lamp", 1,
             r"guest: respawned at the host's lamp|guest: died alone: revived|revived at", d)
    tr = need(v, L, "direct party lamp travel (host)", 0, r"travel step \d+/\d+: host_warp to lamp %s" % TOMB)
    for i in (1, 2):
        need(v, L, "direct travel: guest %d arrived in m24_01" % i, i, r"map - -> m24_01", tr or 0)
    kills = [re.search(r"entity (\d+)", l).group(1) for _, l in
             L.all(0, r"test: host_kill: entity \d+ (killed|HP)") + L.all(1, r"test: guest_kill: entity \d+ (killed|HP)")]
    v.step("direct kills (host 2, guest 1)", len(kills) >= 3, "entities " + " ".join(kills) if kills else "none")
    gk = Lf(1, r"test: guest_kill: entity 2410261")
    if gk:
        need(v, L, "direct guest 1's kill on the host's game", 0,
             r"kill (record|seen): entity 2410261|entity 2410261 .*(dead|died|put to death)", gk[0] - 1)
    hd = need(v, L, "direct host death (scripted)", 0, r"travel step \d+/\d+: host_die", tr or 0)
    if hd is not None:
        need(v, L, "direct host up again", 0, r"host revive #\d+: after|respawn|revived", hd)
    sp.check_common(v, L, "direct")


def check_relay(v, L):
    need = sp.need
    need(v, L, "relay host: no UPnP", 0, r"UPnP: off")
    need(v, L, "relay guest 2 in the party (over the LAN)", 0, r"cooperators 0 -> 1")
    need(v, L, "relay guest 2 found the host on the LAN", 2, r"LAN fallback: found host")
    vpn_check(v, L, "relay")
    need(v, L, "relay guest 1: a member relays for it", 1, r"member relay: .*relays for us")
    need(v, L, "relay guest 1: link through the member's relay", 1, r"network path: party link over .*relay")
    need(v, L, "relay host: guest 1 through guest 2's relay", 0, r"Hunter1 .*through Hunter2's relay|through Hunter2's relay")
    need(v, L, "relay join: 3 players", 0, r"cooperators 1 -> 2")
    tr = need(v, L, "relay party lamp travel (host)", 0, r"travel step \d+/\d+: host_warp to lamp %s" % TOMB)
    need(v, L, "relay travel: guest 1 arrived in m24_01", 1, r"map - -> m24_01", tr or 0)
    need(v, L, "relay host kill", 0, r"test: host_kill: entity \d+ (killed|HP)", tr or 0)
    need(v, L, "relay guest 1 kill", 1, r"test: guest_kill: entity \d+ (killed|HP)", tr or 0)
    sp.check_common(v, L, "relay")


def vpn_check(v, L, tag):
    texts = ["\n".join(L.lines[i]) for i in range(3)]
    rows = [dict(instances.real_net_lines(t)) for t in texts]
    hp = instances._ip(rows[0].get("public (STUN, host)"))
    g1 = instances._ip(rows[1].get("public (STUN, guest)"))
    g2 = instances._ip(rows[2].get("public (STUN, guest)"))
    v.step("%s guest 1 through the VPN (public IPv4 differs from the host's)" % tag, hp and g1 and hp != g1,
           "host %s, guest 1 %s" % ("known" if hp else "no STUN line", "differs" if hp and g1 and hp != g1 else
                                    "the same" if hp and g1 else "no STUN line"))
    v.step("%s guest 2 outside the VPN (the host's public IPv4)" % tag, hp and g2 and hp == g2,
           "the same" if hp and g2 and hp == g2 else "differs or unknown")
    seen = instances._ip(rows[1].get("seen by the host at"))
    if tag == "direct":
        v.step("direct host saw guest 1 at its VPN address", seen and seen == g1,
               "yes" if seen and seen == g1 else "no (%s)" % ("unknown" if not seen else "another address"))


# ---- main ----

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=str(DEFAULT_ROOT))
    ap.add_argument("--build", default=str(REPO))
    ap.add_argument("--game", default=os.environ.get("BB_GAME_DIR", str(AGENT_ROOT / "game")))
    ap.add_argument("--out", default=str(AGENT_ROOT / "realnet_runs" / time.strftime("%Y%m%d_%H%M")))
    ap.add_argument("--phase", default="direct,relay")
    ap.add_argument("--every", type=float, default=10)
    ap.add_argument("--guest2-headless", action="store_true", help="guest 2 headless-lite (no screenshots of it)")
    ap.add_argument("--no-relay-upnp", action="store_true",
                    help="relay: guest 2's UDP port is forwarded on the router by hand (no UPnP mapping here)")
    ap.add_argument("--no-vpn-guard", action="store_true", help="do not stop when guest 1 has the host's public IPv4")
    ap.add_argument("--print-exe", action="store_true", help="set the instances up, print the exe paths, run nothing")
    ap.add_argument("--dry-run", action="store_true", help="print the instances.py commands, run nothing")
    ap.add_argument("--check-only", action="store_true")
    a = ap.parse_args()
    a.real_net = True
    sp.RUN_ROOT[0] = a.root
    if Path(a.game).resolve() == Path(r"E:\games\bloodborne").resolve():
        sys.exit("never the user's game folder as the test game")
    root = Path(a.root).resolve()
    a.root = str(root)
    if a.print_exe or not (root / "inst1" / "out" / "bbport.exe").exists():
        subprocess.run([sys.executable, str(HERE / "instances.py"), "setup", "--count", "3", "--root", str(root),
                        "--build", str(a.build), "--game", str(a.game)], check=True)
    for i, who in enumerate(("host", "guest 1 (VPN split tunnel: include this one)", "guest 2")):
        print("[realnet] inst%d %-46s %s" % (i, who, root / f"inst{i}" / "out" / "bbport.exe"), flush=True)
    if a.print_exe:
        return
    port0 = instances.base_port(root)
    out = Path(a.out)
    v = sp.Verdict()
    checks = {"direct": check_direct, "relay": check_relay}
    for name in [p.strip() for p in a.phase.split(",") if p.strip()]:
        if name not in checks:
            sys.exit("unknown phase %s (direct, relay)" % name)
        mapped = None
        if name == "direct":
            ph = phase_direct(a, port0)
        else:
            pub = None if a.check_only else instances.stun_public_ipv4()
            if not a.check_only and not a.dry_run:
                if not pub:
                    sys.exit("relay: no STUN answer for this PC's public IPv4 (guest 1 needs it to reach guest 2)")
                # the direct phase's host mapping (a killed host never removes it): the host must be closed
                for proto in ("TCP", "UDP"):
                    upnp("remove", port0, proto)
            ph = phase_relay(a, port0, pub)
            if not a.check_only and not a.dry_run and not a.no_relay_upnp:
                ip = lan_ipv4()
                if not ip or not upnp("add", ph["ports"][2], "UDP", ip):
                    sys.exit("relay: the router took no UPnP mapping for guest 2's UDP port %d; forward it by hand "
                             "to this PC and run again with --no-relay-upnp" % ph["ports"][2])
                mapped = ph["ports"][2]
        print("[realnet] %s: ports host %d, guest 1 %d, guest 2 %d" % ((name,) + tuple(ph["ports"])), flush=True)
        if a.dry_run:
            ph = dict(ph)
            print("[realnet] dry run: smoke_party.run_phase would start instances.py run --real-net with", ph, flush=True)
            continue
        guard = None
        try:
            if not a.check_only:
                guard = None if a.no_vpn_guard else VpnGuard(root)
                sp.run_phase(a, ph, out / name)
        finally:
            if guard:
                guard.close()
            if mapped:
                upnp("remove", mapped, "UDP")
            if name == "direct" and not a.check_only:
                for proto in ("TCP", "UDP"):  # the host was killed: its UPnP mapping stays otherwise
                    upnp("remove", port0, proto)
        print("[realnet] verdict %s" % name, flush=True)
        if guard and guard.tripped:
            v.step("%s VPN check" % name, False, guard.tripped)
        checks[name](v, sp.Logs(out / name))
        rep = root / "realnet_report.txt"
        if rep.exists() and not a.check_only:
            (out / name).mkdir(parents=True, exist_ok=True)
            (out / name / "realnet_report.txt").write_text(rep.read_text(encoding="utf-8"), encoding="utf-8")
    if a.dry_run:
        return
    fails = [r for r in v.rows if not r[1]]
    text = "\n".join("%s  %s  %s" % ("PASS" if ok else "FAIL", n, d) for n, ok, d in v.rows)
    text += "\n\n%d steps, %d failed\n" % (len(v.rows), len(fails))
    out.mkdir(parents=True, exist_ok=True)
    (out / "verdict.txt").write_text(text, encoding="utf-8")
    print("[realnet] %d steps, %d failed; %s" % (len(v.rows), len(fails), out / "verdict.txt"))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
