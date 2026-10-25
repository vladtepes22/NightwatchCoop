#!/usr/bin/env python3
"""Party network doctor: reads co-op logs from bbport or Nightwatch Co-op and explains, in plain words
ready to paste into Discord, why players cannot join or keep dropping.

  python scripts/net_doctor.py LOG [LOG ...]        (one log per player is best: host and guests)
  python scripts/net_doctor.py --evidence LOG ...   (also show the log lines behind each finding)
  python scripts/net_doctor.py --redactnames --redactips LOG ...   (safe to post publicly)

Give the logs of ONE session: the host's and every guest's (bbport logs\\last_session.log /
previous_session.log, Nightwatch nightwatch-*.log, or the same text saved as .txt). Dropping the
files onto net_doctor.bat does the same.

The reply names the players and their addresses, so you can tell who has to do what. Before posting
it publicly: --redactnames turns names into Host / Guest 1 / Guest 2 ..., --redactips hides public
IP addresses completely and shortens VPN / LAN ones to their first number (26.x.x.x). Standard
library only.
"""
import argparse
import ipaddress
import re
import sys
from collections import Counter
from pathlib import Path

VPN_NAMES = ("Radmin", "Hamachi", "ZeroTier", "Tailscale", "WireGuard")

R = {
    "role": re.compile(r"party networking on \((host|guest), as ([^,]+), port (\d+)\)"),
    "role_old": re.compile(r"party networking on \((host|guest)"),
    "build": re.compile(r"^(Nightwatch Co-op v[^,]+)"),
    "coop_nw": re.compile(r"co-op visy v([\w.+-]+)"),
    "version": re.compile(r"version check: eboot ([0-9a-f]+)\.*, patches ([0-9a-f]+)\.*, mods ([0-9a-f]+)"),
    "build_id_full": re.compile(r"Party: build (.+?), params"),
    "max": re.compile(r"Party: party rules: max (\d) players"),
    "local": re.compile(r"Net: local address (\S+) (from BB_PARTY_LOCAL_IP|on) ?\(?(?:adapter )?([^\[\];)]*)"),
    "public": re.compile(r"Party: public address (\S+?):\d+"),
    "nat": re.compile(r"Party: NAT: (cone \(port open\)|cone \(no open port\)|symmetric|open|[a-z ]+?)[;:]"),
    "upnp_no": re.compile(r"UPnP: no UPnP router found|UPnP unavailable"),
    "upnp_ok": re.compile(r"UPnP: port \d+ mapped|UPnP: mapped"),
    "joining": re.compile(r"Party: joining (\S+?):(\d+) as (\S+)(?: \((LAN code|Internet code|address)\))?"),
    "connected": re.compile(r"Party: link: connected: slot (\d+)"),
    "no_traffic": re.compile(r"Party: link: reconnecting: no traffic for"),
    "refused": re.compile(r"refused by the host: (.+)$"),
    "guest_joined": re.compile(r"Party: link: (\S+) (joined|rejoined) \(slot (\d+), from (\S+?):\d+\)"),
    "lost": re.compile(r"Party: link: lost (\S+) \(slot \d+\): (.+?);"),
    "outage": re.compile(r"Net: outage resume: (\S+?):\d+ heard again after ([\d.]+) s"),
    "host_stun_silent": re.compile(r"stun: no answer from (\S+?):\d+; the host gets our LAN address only"),
    "guest_public": re.compile(r"Party: guest code \S+ \(UDP (\S+?):\d+, stun"),
    "host_udp_back": re.compile(r"summon: UDP to the host works again"),
    "no_public_udp": re.compile(r"summon: UDP to (\S+) is blocked: its game has only the LAN address"),
    "not_heard": re.compile(r"direct summon: slot \d+ \((\S+)\) reports a failure: its UDP is not heard"),
    "summon_refused": re.compile(r"direct summon: slot \d+ \((\S+)\) refused \(([^;)]+)"),
    "punch_local": re.compile(r"Net: punch (\S+): using its local address (\S+?):\d+ \(heard.*?; (\S+?):\d+ stays its game address\)"),
    "punching": re.compile(r"Party: link: punching towards a guest at (\S+?):\d+"),
    "lan_fallback": re.compile(r"LAN fallback: found host at (\S+?):\d+ \(it reports (\S+?);"),
    "lan_fallback_fail": re.compile(r"LAN fallback: the host's address (\S+?):\d+ did not answer"),
    "discovery": re.compile(r"LAN discovery: answered a probe from (\S+?):\d+"),
    "roster": re.compile(r"Party: roster: (.+)$"),
    "prologue": re.compile(r"members in their prologue \(no signs shown to or from them\): (.+)$"),
    "ng": re.compile(r"NG cycle (\d+)"),
    "left_room": re.compile(r"member \d+ \((\S+)\) left room \d+ \(leave_room\)"),
}
ROSTER_ENTRY = re.compile(r"(\S+?)\(slot (\d+), ([a-z_]+)(?:, lost)?, map \d+, (\d+) ms\)")


def is_vpn_ip(ip):
    try:
        a = ipaddress.ip_address(ip)
    except ValueError:
        return False
    return a in ipaddress.ip_network("26.0.0.0/8") or a in ipaddress.ip_network("25.0.0.0/8") or \
        a in ipaddress.ip_network("100.64.0.0/10")


def is_public(ip):
    try:
        a = ipaddress.ip_address(ip)
    except ValueError:
        return False
    return a.is_global and not is_vpn_ip(ip)


def vpn_kind(ip):
    if ip.startswith("26."):
        return "Radmin"
    if ip.startswith("25."):
        return "Hamachi"
    if ip.startswith("100."):
        return "Tailscale"
    return "VPN"


class Log:
    def __init__(self, path):
        self.path = Path(path)
        self.name = None
        self.role = None
        self.build = None
        self.version = None
        self.build_id = None
        self.max_players = None
        self.local = None
        self.local_forced = False
        self.local_adapter = ""
        self.vpn_candidates = []
        self.tether = False
        self.public = None
        self.nat = None
        self.upnp_no = False
        self.upnp_ok = False
        self.joining = None
        self.connected = False
        self.no_traffic = 0
        self.refused = Counter()
        self.guests = {}
        self.lost = Counter()
        self.outages = []
        self.not_heard = Counter()
        self.no_public_udp = Counter()
        self.host_stun_silent = 0
        self.guest_public = None
        self.host_udp_back = False
        self.summon_refused = Counter()
        self.punch_local = []
        self.punching = 0
        self.lan_fallback = None
        self.lan_fallback_fail = False
        self.discovery_probes = 0
        self.pings = {}
        self.in_world = Counter()
        self.prologue_last = None
        self.ng = None
        self.left_room = Counter()
        self.evidence = {}
        self.started = None
        self.coop = None
        self.vpn_probe = {}  # adapter label -> "online" / "offline" (0.7.6 builds log it)

    def note(self, key, lineno, line):
        self.evidence.setdefault(key, []).append((lineno, line.strip()[:220]))


def _minutes(day, hhmm):
    """Minutes since 2000-01-01 for a 'YYYY-MM-DD' day and minutes in the day (ordering only)."""
    y, mo, d = (int(x) for x in day.split("-"))
    return ((y - 2000) * 372 + (mo - 1) * 31 + (d - 1)) * 1440 + hhmm


def parse(path):
    lg = Log(path)
    # Nightwatch names a log after the time it was closed: nightwatch-YYYYMMDD_HHMMSS.log
    m = re.search(r"(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})\d{2}", Path(path).name)
    lg.ended = _minutes(f"{m.group(1)}-{m.group(2)}-{m.group(3)}", int(m.group(4)) * 60 + int(m.group(5))) \
        if m and Path(path).name.startswith("nightwatch-") else None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            if n == 1 or (lg.build is None and n < 5):
                m = R["build"].match(line)
                if m:
                    lg.build = m.group(1).strip()
                m = re.search(r"(\d{4}-\d{2}-\d{2}) (\d{2}):(\d{2})(?::(\d{2}))?", line)
                if m and lg.started is None:
                    lg.started = (m.group(1), int(m.group(2)) * 60 + int(m.group(3)))
            if lg.coop is None and n < 6:
                m = R["coop_nw"].search(line)
                if m:
                    lg.coop = m.group(1)
            if "[coop]" not in line and "Party" not in line and "Net:" not in line:
                continue
            if lg.role is None:
                m = R["role"].search(line)
                if m:
                    lg.role, lg.name = m.group(1), m.group(2)
                    continue
                m = R["role_old"].search(line)
                if m:
                    lg.role = m.group(1)
            m = R["version"].search(line)
            if m:
                lg.version = m.groups()
            m = R["build_id_full"].search(line)
            if m:
                lg.build_id = m.group(1)
                if lg.coop is None and not m.group(1).startswith("nightwatch"):
                    lg.coop = m.group(1).split()[0]
            m = R["max"].search(line)
            if m:
                lg.max_players = int(m.group(1))
            m = R["local"].search(line)
            if m and lg.local is None:
                lg.local = m.group(1)
                lg.local_forced = m.group(2) == "from BB_PARTY_LOCAL_IP"
                lg.local_adapter = m.group(3)
                lg.tether = lg.tether or "Remote NDIS" in line
                for seg in line.split(";"):
                    cand = re.search(r"(\d+\.\d+\.\d+\.\d+) ([^/;]+?) /", seg)
                    if not cand or "offline VPN" in seg:  # 0.7.6: an offline VPN is listed but skipped
                        continue
                    if any(v.lower() in cand.group(2).lower() for v in VPN_NAMES) and cand.group(1) != lg.local:
                        lg.vpn_candidates.append((cand.group(1), cand.group(2).strip()))
                lg.note("local", n, line)
            m = R["public"].search(line)
            if m:
                lg.public = m.group(1)
            m = R["nat"].search(line)
            if m:
                lg.nat = m.group(1).strip()
                lg.note("nat", n, line)
            if R["upnp_no"].search(line):
                lg.upnp_no = True
            if R["upnp_ok"].search(line):
                lg.upnp_ok = True
            m = R["joining"].search(line)
            if m:
                lg.joining = (m.group(1), m.group(4) or "")
                if lg.name is None:
                    lg.name = m.group(3)
            if R["connected"].search(line):
                lg.connected = True
            if R["no_traffic"].search(line):
                lg.no_traffic += 1
                lg.note("no_traffic", n, line)
            m = R["refused"].search(line)
            if m:
                lg.refused[m.group(1).strip()] += 1
                lg.note("refused", n, line)
            m = R["guest_joined"].search(line)
            if m:
                lg.guests[m.group(1)] = m.group(4)
            m = R["lost"].search(line)
            if m:
                lg.lost[m.group(1)] += 1
                lg.note("lost", n, line)
            m = R["outage"].search(line)
            if m:
                lg.outages.append((m.group(1), float(m.group(2))))
                lg.note("outage", n, line)
            if R["host_stun_silent"].search(line):
                lg.host_stun_silent += 1
                lg.note("host_stun_silent", n, line)
            m = R["guest_public"].search(line)
            if m:
                lg.guest_public = m.group(1)
            if R["host_udp_back"].search(line):
                lg.host_udp_back = True
            m = R["no_public_udp"].search(line)
            if m:
                lg.no_public_udp[m.group(1)] += 1
                lg.note("no_public_udp", n, line)
            m = R["not_heard"].search(line)
            if m:
                lg.not_heard[m.group(1)] += 1
                lg.note("not_heard", n, line)
            m = R["summon_refused"].search(line)
            if m:
                lg.summon_refused[(m.group(1), m.group(2).strip())] += 1
            m = R["punch_local"].search(line)
            if m:
                lg.punch_local.append((m.group(1), m.group(2), m.group(3)))
                lg.note("punch_local", n, line)
            if R["punching"].search(line):
                lg.punching += 1
                lg.note("punching", n, line)
            m = R["lan_fallback"].search(line)
            if m:
                lg.lan_fallback = (m.group(1), m.group(2))
                lg.note("lan_fallback", n, line)
            if R["lan_fallback_fail"].search(line):
                lg.lan_fallback_fail = True
            if "Remote NDIS" in line and "Net:" in line:
                lg.tether = True
            m = re.search(r"vpn adapter if\d+ \(([^)]+)\): (online|OFFLINE)", line)
            if m:
                lg.vpn_probe[m.group(1)] = m.group(2).lower()
            if R["discovery"].search(line):
                lg.discovery_probes += 1
            m = R["roster"].search(line)
            if m:
                for e in ROSTER_ENTRY.finditer(m.group(1)):
                    who, _slot, state, ms = e.groups()
                    if state in ("home", "in_host_world") and int(ms) > 0:
                        lg.pings.setdefault(who, []).append(int(ms))
                    if state == "in_host_world":
                        lg.in_world[who] += 1
            m = R["prologue"].search(line)
            if m:
                who = m.group(1).strip()
                lg.prologue_last = None if who == "none" else (who, n)
            m = R["ng"].search(line)
            if m:
                lg.ng = int(m.group(1))
            m = R["left_room"].search(line)
            if m:
                lg.left_room[m.group(1)] += 1
    return lg


def hide(ip, _show=None):
    return "?" if ip is None else ip


IP_RX = re.compile(r"\b(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})\b")


def redact(text, logs, names, ips):
    if names:
        order = []
        host = [l.name for l in logs if l.role == "host" and l.name]
        if host:
            order.append(host[0])
        for l in logs:
            for n in [l.name] + list(l.guests) + list(l.pings):
                if n and n not in order:
                    order.append(n)
        alias = {}
        guest_no = 0
        for n in order:
            if host and n == host[0]:
                alias[n] = "Host"
            else:
                guest_no += 1
                alias[n] = f"Guest {guest_no}"
        for n in sorted(alias, key=len, reverse=True):
            text = re.sub(r"(?<![\w-])" + re.escape(n) + r"(?![\w-])", lambda _m, a=alias[n]: a, text)
        # Windows paths carry the user name
        text = re.sub(r"([A-Za-z]:\\Users\\)[^\\\s]+", lambda m: m.group(1) + "<user>", text)
    if ips:
        text = re.sub(r"(setx BB_PARTY_LOCAL_IP |local address |to `|print `)(\d+\.\d+\.\d+\.\d+)",
                      lambda m: m.group(1) + f"<your {vpn_kind(m.group(2))} IP>", text)

        def mask(m):
            ip = m.group(0)
            if is_public(ip):
                return "<public IP>"
            return m.group(1) + ".x.x.x"
        text = IP_RX.sub(mask, text)
    return text


def diagnose(logs, show_ips):
    issues = []  # (priority, text, fix)
    hosts = [l for l in logs if l.role == "host"]
    guests = [l for l in logs if l.role == "guest"]
    host = hosts[0] if hosts else None
    by_name = {l.name: l for l in logs if l.name}

    flagged = set()  # (kind, player) already reported

    def add(prio, text, fix=None, key=None):
        if key:
            flagged.add(key)
        issues.append((prio, text, fix))

    starts = [_minutes(*l.started) for l in logs if l.started]
    ends = [l.ended for l in logs if getattr(l, "ended", None)]
    if len(starts) > 1:
        apart = (ends and max(starts) > min(ends) + 1) or max(starts) - min(starts) > 20
        if apart:
            txt = ", ".join(f"{l.name or l.path.name} {l.started[1] // 60:02d}:{l.started[1] % 60:02d}"
                            for l in logs if l.started)
            add(0, f"**These logs may be from different sessions** (game started: {txt}). Findings that compare "
                   f"players can be wrong.",
                "For a reliable check, send the logs of ONE attempt from everyone: the newest log right after it "
                "happened, before starting the game again.")

    # Same game and mods on every PC
    vers = {l.version for l in logs if l.version}
    if len(vers) > 1:
        rows = ", ".join(f"{l.name or l.path.name}: eboot {l.version[0][:6]} / patches {l.version[1][:6]} / mods {l.version[2][:6]}"
                         for l in logs if l.version)
        add(1, f"**Different game files.** Not everyone runs the same eboot, patches or gameplay mods ({rows}).",
            "Everyone needs the exact same eboot.bin, the same patch selection and the same gameplay mod files.")
    builds = {l.build_id for l in logs if l.build_id}
    if len(builds) > 1:
        txt = ", ".join(f"{l.name or l.path.name}: {version_text(l)}" for l in logs)
        add(1, f"**Different versions.** The players run different builds ({txt}).",
            "Everyone installs the same version of the game / Nightwatch.")

    for l in logs:
        who = l.name or l.path.name
        for reason, cnt in l.refused.items():
            add(1, f"**{who} was refused by the host:** \"{reason}\"" + (f" ({cnt}x)" if cnt > 1 else ""),
                refusal_fix(reason))

    # Host reachability
    if host:
        hn = host.name or "the host"
        if host.tether:
            add(1, f"**{hn} is on a phone hotspot (USB tethering).** Mobile carriers share one address between "
                   f"many users, so nobody can connect in directly, and the connection drops often.",
                f"Host from a home internet connection if anyone can. Otherwise everyone uses a VPN "
                f"(Tailscale, Radmin or Hamachi) and the host's VPN address.")
        if host.nat and "no open port" in host.nat and not host.upnp_ok and not host.local_forced                 and not (host.local and is_vpn_ip(host.local)):  # over a VPN the open port does not matter
            add(2, f"**{hn} has no open port** (UPnP not available on the router). Guests can only get in by "
                   f"hole punching, which fails if a guest's router is strict.",
                f"{hn}: turn on UPnP in the router, or forward UDP + TCP 9307 to the PC, or use a VPN.")
    host_reachable = bool(host and host.nat and ("port open" in host.nat or host.nat == "open"))
    for l in logs:
        who = l.name or l.path.name
        if l.nat == "symmetric":
            role = "host" if l.role == "host" else "guest"
            on_vpn = bool(l.local and is_vpn_ip(l.local))
            if role == "guest" and (host_reachable or on_vpn):
                continue  # a guest only needs to reach out; strict NAT matters for hole punching only
            add(2 if role == "guest" else 1,
                f"**{who} is behind a strict NAT** (a strict router or the internet provider's own NAT). "
                f"Hole punching cannot reach it.",
                f"{who}: turn on UPnP or forward UDP + TCP 9307; if the router's WAN address differs from "
                f"your public IP, your provider uses carrier NAT: ask them for a public IP, or use a VPN.")

    # VPN set-up
    vpn_in_use = any(l.local and is_vpn_ip(l.local) for l in logs) or any(
        l.joining and is_vpn_ip(l.joining[0]) for l in logs) or any(
        is_vpn_ip(ip) for l in logs for ip in l.guests.values())
    for l in logs:
        who = l.name or l.path.name
        if l.vpn_candidates and not l.local_forced and (l.local is None or not is_vpn_ip(l.local)):
            ip, adapter = l.vpn_candidates[0]
            kind = vpn_kind(ip)
            prio = 1 if vpn_in_use else 3
            flagged.add(("vpn_unused", who))
            known_online = any(v == "online" and kind.lower() in k.lower() for k, v in l.vpn_probe.items())
            if not known_online and not vpn_in_use:
                # older builds cannot tell an offline VPN from an online one: do not claim it is in use
                add(4, f"{who} has {kind} installed ({ip}); the game used the home network address. Fine if you "
                       f"do not play over {kind}; if you do, set BB_PARTY_LOCAL_IP to the {kind} address.", None)
                continue
            add(prio, f"**{who} has {kind} running, but the game uses the home network address** "
                      f"({hide(l.local, show_ips)}) instead of the {kind} one ({ip}).",
                setx_steps(who, "BB_PARTY_LOCAL_IP", ip, f"make the game use {kind}: ") +
                f"\n   The new log then says \"local address {ip} from BB_PARTY_LOCAL_IP\".")

    # Self-alias: a game took its own address for another player's
    for l in logs:
        who = l.name or l.path.name
        for peer, local_used, _game in l.punch_local:
            if l.local and local_used == l.local:
                add(1, f"**{who}'s game thinks {peer} is at {who}'s own address** ({hide(local_used, show_ips)}), "
                       f"so it ends up talking to itself and drops out.",
                    f"Every player checks `BB_PARTY_LOCAL_IP`: press the Windows key, type `cmd`, Enter, then "
                    f"type `echo %BB_PARTY_LOCAL_IP%` and Enter. It must print that player's OWN VPN address "
                    f"(shown in the VPN app). If not, set it with `setx BB_PARTY_LOCAL_IP <own VPN address>`, close "
                    f"the window, restart the launcher (or the PC). Co-op 0.7.5 and newer fix the remaining case.")
                break
            other = by_name.get(peer)
            if other and other.local and local_used != other.local and not is_vpn_ip(local_used) and \
                    any(is_vpn_ip(x) for x in (l.local or "", other.local or "")) and \
                    ("vpn_unused", who) not in flagged:
                add(2, f"**{who} talks to {peer} on a home network address** ({hide(local_used, show_ips)}) "
                       f"instead of the VPN one.",
                    f"Both set `BB_PARTY_LOCAL_IP` to their own VPN address (Windows key, `cmd`, "
                    f"`setx BB_PARTY_LOCAL_IP <own VPN address>`, close the window) and fully restart the launcher.")
                break

    # A guest whose game never got a public UDP address (its STUN on the party port got no answer)
    if host:
        for g, cnt in host.no_public_udp.items():
            if host.in_world.get(g):
                continue
            gl = by_name.get(g)
            if gl and gl.host_stun_silent and gl.guest_public:
                # the guest's UDP reaches the internet (its own STUN worked) but not the host's party port
                hn = host.name or "the host"
                back = " It started answering later in the session." if gl.host_udp_back else ""
                add(1, f"**{hn}'s party port does not answer {g}'s UDP** ({g}'s own internet UDP works). "
                       f"The party link (TCP) gets through, the game traffic (UDP) does not, so {g} is never "
                       f"summoned.{back}",
                    f"{hn}: allow the game in Windows Firewall for INBOUND UDP + TCP 9307 (and in any antivirus "
                    f"firewall); the router mapping alone is not enough. Then restart the launcher and host again.")
                continue
            add(1, f"**{g}'s game cannot send UDP to the internet**: it only knows its home address, so the host "
                   f"cannot summon it ({cnt}x). {g} is in the party (that part uses TCP) but stays in its own world; "
                   f"the in-game 'still in the prologue' / 'no public UDP' notices come from this.",
                f"{g} (or the host, for incoming UDP): allow the game in Windows Firewall for UDP + TCP 9307 (and in any antivirus with its own "
                f"firewall), check the router does not block outgoing UDP, then restart the launcher. Or both of you "
                f"use a VPN (Radmin / Tailscale) and set BB_PARTY_LOCAL_IP to your own VPN address.")

    # Game traffic not arriving after the party link is up
    if host:
        for g, cnt in host.not_heard.items():
            if cnt < 2:
                continue
            if host.in_world.get(g):
                add(4, f"{g}'s first summons failed ({cnt}x, game traffic not heard), then {g} got into the "
                       f"host's world.", None)
                continue
            add(1, f"**{g} connects to the party but the game's own traffic never reaches the host** "
                   f"({cnt} failed summons). Shared progress works, the summon does not.",
                f"{host.name or 'Host'} and {g}: if you play over a VPN, each sets `BB_PARTY_LOCAL_IP` to "
                f"their OWN VPN address (Windows key, type `cmd`, Enter, `setx BB_PARTY_LOCAL_IP <own VPN "
                f"address>`, Enter, close the window, then fully restart the launcher or the PC). Allow the game "
                f"through Windows Firewall (UDP + TCP 9307). If it still fails, send both logs from that attempt.")

    # Guest link never up
    for l in guests:
        who = l.name or l.path.name
        if l.no_traffic >= 3 and not l.connected:
            target = hide(l.joining[0], show_ips) if l.joining else "the host"
            add(1, f"**{who} never reached the host** ({target}): {l.no_traffic} tries, no answer.",
                "Check that the host is really hosting right now, that both are on the same VPN network (and "
                "it shows connected, not relayed/offline), and that the host's firewall allows the game.")
        elif l.no_traffic >= 3:
            add(2, f"**{who}'s connection to the host keeps dying** ({l.no_traffic} reconnects).",
                "Usually a weak link: phone hotspot, Wi-Fi far from the router, a relayed VPN. Try a cable, "
                "or a different VPN.")
        if l.lan_fallback:
            found, reports = l.lan_fallback
            add(4, f"{who}'s LAN code pointed at the wrong address; the game found the host at "
                   f"{hide(found, show_ips)} by searching (the host advertised {hide(reports, show_ips)}).",
                "Fixed in co-op 0.7.6. Until then the host sets `BB_PARTY_LOCAL_IP` to its VPN address (Windows "
                "key, `cmd`, `setx BB_PARTY_LOCAL_IP <VPN address>`, close the window, restart the launcher).")

    # Hole punching
    if host and host.punching and not host.guests:
        add(2, f"**Hole punching from the host failed** ({host.punching}x). The guest's router or provider "
               f"is too strict for it.",
            "Use a VPN, or let the player with an open port (UPnP or forwarded 9307) host.")

    # Drops and lag
    for l in logs:
        who = l.name or l.path.name
        ip_name = {ip: g for g, ip in l.guests.items()}
        if l.role == "guest" and l.joining:
            ip_name.setdefault(l.joining[0], "the host")
        per_peer = {}
        for ip, sec in l.outages:
            if sec >= 4:
                per_peer.setdefault(ip_name.get(ip, "another player" if l.role == "guest" else ip), []).append(sec)
        for peer, outs in per_peer.items():
            if len(outs) < 3:
                continue
            span = f"{min(outs):.0f}-{max(outs):.0f} s" if round(max(outs)) > round(min(outs)) else f"{max(outs):.0f} s"
            flagged.add(("freeze", peer))
            add(2, f"**The link between {who} and {peer} keeps freezing** ({len(outs)} dropouts of "
                   f"{span}). After a long one the game ends the session and the guest is sent home.",
                "Find the weak link: phone hotspot, relayed VPN (Hamachi shows 'relayed'), Wi-Fi, downloads "
                "running. The player with the steadiest connection should host.")
        for g, cnt in l.lost.items():
            if cnt >= 2 and ("freeze", g) not in flagged:
                add(3, f"**{g} lost the link {cnt} times** in {who}'s log.", None)
        med = {p: sorted(v)[len(v) // 2] for p, v in l.pings.items() if v}
        bad = {p: ms for p, ms in med.items() if ms >= 250}
        if bad and l.role == "host":
            txt = ", ".join(f"{p} ~{ms} ms" for p, ms in bad.items())
            add(3, f"**High ping:** {txt}. Above ~250 ms co-op lags and drops more.",
                "Closer VPN relay, wired connection, or the player with the best connection hosts.")
        if l.role == "host":
            for g, cnt in l.left_room.items():
                if cnt >= 3 and g not in l.not_heard:
                    add(3, f"**{g} keeps leaving and re-entering the host's world** ({cnt}x).", None)

    # Not network, but blocks joining
    for l in logs:
        if l.prologue_last and l.role == "host":
            add(1, f"**Not network: {l.prologue_last[0]} counts as still in the prologue**, so no summons "
                   f"happen. After an ending (NG+) this is a known bug, fixed in co-op 0.7.6.",
                setx_steps(l.prologue_last[0], "BB_PARTY_START", "immediate",
                           "until co-op 0.7.6, skip the prologue check: ") +
                "\n   In a first playthrough, finish the first Hunter's Dream visit (take a weapon) instead.")
            break
    ngs = {l.name or l.path.name: l.ng for l in logs if l.ng is not None}
    if len(set(ngs.values())) > 1:
        txt = ", ".join(f"{n} NG+{c}" if c else f"{n} first playthrough" for n, c in ngs.items())
        add(2, f"**Not network: the saves are in different New Game+ cycles** ({txt}). A guest still plays in "
               f"the host's world, but its own save stays in its own cycle.",
            "If a save jumped a cycle after a party ending, do not keep playing it: ask in the support channel "
            "before restoring a save backup.")

    if host and host.max_players:
        joined = len(host.guests)
        if joined + 1 > host.max_players:
            add(3, f"More players tried than the host allows (max {host.max_players}).",
                "Host sets Max players higher (all players must use the same value).")
    return issues


def setx_steps(who, var, value, why=""):
    """Plain steps for setting a Windows user environment variable and making the game see it."""
    return (f"{who}: {why}set `{var}` to `{value}`:\n"
            f"   a. Press the Windows key, type `cmd` and press Enter (a black Command Prompt window opens).\n"
            f"   b. Type `setx {var} {value}` and press Enter. It must answer `SUCCESS: Specified value was saved.`\n"
            f"   c. Close that window. setx only reaches programs started AFTER it.\n"
            f"   d. Close the game and the launcher completely (also Steam or whatever starts it), or simply "
            f"restart the PC.\n"
            f"   e. Check: open a NEW Command Prompt, type `echo %{var}%`, press Enter: it must print `{value}`.\n"
            f"   f. Start the launcher again and host / join.")


def refusal_fix(reason):
    r = reason.lower()
    if "full" in r:
        return "The party is full: the host raises Max players, or someone leaves."
    if "password" in r:
        return "Check the password with the host (and use the current party code)."
    if "another hosting session" in r or "party code" in r:
        return "Ask the host for the current party code. A plain IP:port only works if the host set a password."
    if "rules differ" in r or "max" in r:
        return "Everyone sets the same party rules the host uses (Max players and the 4-player options)."
    if "version" in r or "differ" in r or "eboot" in r or "mods" in r or "patches" in r:
        return "Everyone needs the same co-op version, eboot.bin, patches and gameplay mods."
    if "name" in r:
        return "Pick a different player name (1-16 letters, digits, _ or -), unique in the party."
    if "kicked" in r:
        return "The host kicked this player; the host can start a new party code."
    return None


def version_text(l):
    if l.build:
        return f"{l.build} (co-op {l.coop or 'older than 0.7.2'})"
    if l.coop:
        return f"bbport {l.coop}"
    return "version ?"


def player_line(l, show_ips):
    who = l.name or "?"
    parts = [f"{who} ({l.role or '?'})"]
    parts.append(version_text(l))
    if l.local:
        src = "set by BB_PARTY_LOCAL_IP" if l.local_forced else "automatic"
        parts.append(f"address {hide(l.local, show_ips)} ({src})")
    if l.tether:
        parts.append("phone hotspot")
    if l.nat:
        parts.append(f"NAT {l.nat}")
    if l.role == "guest":
        parts.append("reached the host's world" if l.in_world.get(l.name) else
                     ("party link up" if l.connected else "never connected"))
    if l.role == "host" and l.guests:
        ok = [g for g in l.guests if l.in_world.get(g)]
        parts.append("guests in the world: " + (", ".join(ok) if ok else "none") +
                     f" of {len(l.guests)}")
    return " | ".join(parts)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Explain co-op connection problems from party logs.")
    ap.add_argument("logs", nargs="+", help="log files (host and guests)")
    ap.add_argument("--evidence", action="store_true", help="show the log lines behind the findings")
    ap.add_argument("--redactnames", action="store_true", help="replace player names with Host / Guest N")
    ap.add_argument("--redactips", action="store_true", help="hide public IPs, shorten VPN/LAN IPs")
    a = ap.parse_args(argv)

    logs = []
    for p in a.logs:
        if not Path(p).is_file():
            print(f"not a file: {p}", file=sys.stderr)
            continue
        lg = parse(p)
        if lg.role is None:
            print(f"(skipped {Path(p).name}: no party networking in this log)", file=sys.stderr)
            continue
        logs.append(lg)
    if not logs:
        print("No co-op logs found. Give the logs of a party session (host and guests).")
        return 1

    issues = diagnose(logs, True)
    out = ["**Co-op connection check**"]
    for l in logs:
        out.append("- " + player_line(l, True))
    if not any(l.role == "host" for l in logs):
        out.append("_No host log given: the host's log shows much more (who reached it, why summons failed)._")
    out.append("")
    if not issues:
        out.append("No network problem found in these logs. If something still goes wrong, send the logs "
                   "from the exact attempt (host and guests), right after it happens.")
    else:
        seen = set()
        issues.sort(key=lambda x: x[0])
        out.append("**What's wrong**")
        fixes = []
        for prio, text, fix in issues:
            if text in seen:
                continue
            seen.add(text)
            out.append(f"{len(seen)}. {text}")
            if fix and fix not in fixes:
                fixes.append(fix)
        if fixes:
            out.append("")
            out.append("**What to try**")
            for i, f in enumerate(fixes, 1):
                out.append(f"{i}. {f}")
    if a.evidence:
        out.append("")
        out.append("```")
        for l in logs:
            for key, rows in l.evidence.items():
                for n, line in rows[:3]:
                    out.append(f"{l.path.name}:{n}: {line}")
        out.append("```")
    print(redact("\n".join(out), logs, a.redactnames, a.redactips))
    return 0


if __name__ == "__main__":
    sys.exit(main())
