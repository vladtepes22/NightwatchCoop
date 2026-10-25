"""scripts/net_doctor.py: findings from small synthetic party logs, and the redaction options."""
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("net_doctor", ROOT / "scripts" / "net_doctor.py")
nd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nd)

HOST = """Nightwatch Co-op v0.2.3 beta 3, engine x
[coop] Net: party networking on (host, as HostA, port 9307)
[coop] Party: host HostA, port 9307, up to 4 players; version check: eboot aaaa11..., patches bbbb22..., mods cccc33... (6 gameplay patches, no gameplay mods)
[coop] Net: local address 192.168.50.4 on Ethernet 2 / Realtek [default route, gateway 192.168.50.1]; candidates: 26.121.14.68 Radmin VPN / Famatech Radmin VPN Ethernet Adapter (score 6); 192.168.50.4 Ethernet 2 / Realtek (score 114); BB_PARTY_LOCAL_IP overrides
[coop] Party: public address 203.0.113.9:9307 (stun stun.l.google.com:19302)
[coop] Party: NAT: cone (port open); port 9307 open: guests connect directly
[coop] Party: link: GuestB joined (slot 1, from 26.76.171.64:59920)
[coop] Party B2: direct summon: slot 1 (GuestB) reports a failure: its UDP is not heard on the party port: no summon
[coop] Party B2: direct summon: slot 1 (GuestB) reports a failure: its UDP is not heard on the party port: no summon
"""
GUEST = """Nightwatch Co-op v0.2.3 beta 3, engine x
[coop] Net: party networking on (guest, as GuestB, port 9307)
[coop] Party: guest GuestB, port 9307, up to 4 players; version check: eboot aaaa11..., patches bbbb22..., mods dddd44... (6 gameplay patches, 1 gameplay mods)
[coop] Party: joining 26.121.14.68:9307 as GuestB
[coop] Net: local address 26.76.171.64 from BB_PARTY_LOCAL_IP (adapter Radmin VPN / Famatech Radmin VPN Ethernet Adapter)
[coop] Party: NAT: symmetric; symmetric NAT (mobile / carrier NAT): hole punching cannot reach you.
[coop] Party: link: connected: slot 1
[coop] Net: punch HostA: using its local address 26.76.171.64:9307 (heard, the public one too; 203.0.113.9:9307 stays its game address)
"""


def run(tmp_path, capsys, *flags):
    h = tmp_path / "host.log"
    g = tmp_path / "guest.log"
    h.write_text(HOST, encoding="utf-8")
    g.write_text(GUEST, encoding="utf-8")
    assert nd.main([*flags, str(h), str(g)]) == 0
    return capsys.readouterr().out


def test_findings(tmp_path, capsys):
    out = run(tmp_path, capsys)
    assert "HostA (host)" in out and "GuestB (guest)" in out
    assert "Different game files" in out                      # mods hash differs
    assert "setx BB_PARTY_LOCAL_IP 26.121.14.68" in out       # host has Radmin but uses the LAN address
    assert "GuestB's game thinks HostA is at GuestB's own address" in out
    assert "never reaches the host" in out                    # UDP not heard
    assert "strict NAT" not in out                            # host has an open port: the guest only reaches out


def test_redaction(tmp_path, capsys):
    out = run(tmp_path, capsys, "--redactnames", "--redactips")
    assert "HostA" not in out and "GuestB" not in out
    assert "Host (host)" in out and "Guest 1 (guest)" in out
    assert "26.121.14.68" not in out and "26.x.x.x" in out
    assert "203.0.113.9" not in out


def test_different_sessions(tmp_path, capsys):
    h = tmp_path / "nightwatch-20261024_204859.log"   # closed 20:48
    g = tmp_path / "nightwatch-20261024_210000.log"
    h.write_text(HOST.replace("engine x", "engine x, 2026-10-24 20:39:01"), encoding="utf-8")
    g.write_text(GUEST.replace("engine x", "engine x, 2026-10-24 20:54:08"), encoding="utf-8")
    assert nd.main([str(h), str(g)]) == 0
    assert "different sessions" in capsys.readouterr().out


def test_not_heard_then_joined(tmp_path, capsys):
    h = tmp_path / "host.log"
    h.write_text(HOST + "[coop] Party: roster: HostA(slot 0, home, map 1, 0 ms), GuestB(slot 1, in_host_world, map 1, 60 ms)\n",
                 encoding="utf-8")
    assert nd.main([str(h)]) == 0
    out = capsys.readouterr().out
    assert "never reaches the host" not in out
    assert "then GuestB got into the host's world" in out


def test_guest_without_public_udp(tmp_path, capsys):
    h = tmp_path / "host.log"
    h.write_text(HOST + "[coop] Party: summon: UDP to GuestB is blocked: its game has only the LAN address "
                 "10.0.0.102:9307 (no public UDP endpoint: the STUN on our party port gave it no answer)\n",
                 encoding="utf-8")
    assert nd.main([str(h)]) == 0
    assert "GuestB's game cannot send UDP to the internet" in capsys.readouterr().out


def test_vpn_installed_not_claimed_running(tmp_path, capsys):
    # host has Radmin as a candidate but nobody plays over a VPN: a note, not 'running but unused'
    h = tmp_path / "host.log"
    h.write_text(HOST.replace("[coop] Party: link: GuestB joined (slot 1, from 26.76.171.64:59920)\n", "")
                     .replace("its UDP is not heard", "x"), encoding="utf-8")
    assert nd.main([str(h)]) == 0
    out = capsys.readouterr().out
    assert "running, but the game uses" not in out and "installed" in out


def test_offline_vpn_candidate_ignored(tmp_path, capsys):
    h = tmp_path / "host.log"
    h.write_text(HOST.replace("(score 6);", "(score 6) (offline VPN, skipped);"), encoding="utf-8")
    assert nd.main([str(h)]) == 0
    out = capsys.readouterr().out
    assert "Radmin" not in out.split("**What")[1] if "**What" in out else True


def test_host_port_silent_to_guest_udp(tmp_path, capsys):
    h = tmp_path / "host.log"
    g = tmp_path / "guest.log"
    h.write_text(HOST + "[coop] Party: summon: UDP to GuestB is blocked: its game has only the LAN address "
                 "10.0.0.102:9307 (no public UDP endpoint)\n", encoding="utf-8")
    g.write_text(GUEST + "[coop] Party: guest code BBG1-XXXX (UDP 198.51.100.9:23332, stun stun.l.google.com:19302)\n"
                 "[coop] Net: stun: no answer from 203.0.113.9:9307; the host gets our LAN address only\n",
                 encoding="utf-8")
    assert nd.main([str(h), str(g)]) == 0
    out = capsys.readouterr().out
    assert "HostA's party port does not answer GuestB's UDP" in out and "INBOUND" in out


def test_no_open_port_warning_on_vpn_host(tmp_path, capsys):
    h = tmp_path / "host.log"
    h.write_text(HOST.replace("local address 192.168.50.4 on", "local address 26.121.14.68 from BB_PARTY_LOCAL_IP (adapter Radmin VPN) x on")
                 .replace("cone (port open); port 9307 open: guests connect directly",
                          "cone (no open port); x"), encoding="utf-8")
    assert nd.main([str(h)]) == 0
    assert "has no open port" not in capsys.readouterr().out


def test_one_log_per_player(tmp_path, capsys):
    """A guest's last_session + previous_session are two sessions of one player: one line, the log that fits the
    host's (Nightwatch name = close time), and no 'different sessions' warning for overlapping logs."""
    h = tmp_path / "nightwatch-20261025_185810.log"
    h.write_text(HOST.replace("engine x", "engine x, 2026-10-25 17:49:00"), encoding="utf-8")
    early, late = tmp_path / "a" / "previous_session.log", tmp_path / "b" / "last_session.log"
    for p, t in ((early, "18:53:10"), (late, "19:08:30")):
        p.parent.mkdir()
        p.write_text(GUEST.replace("engine x", f"engine x, 2026-10-25 {t}"), encoding="utf-8")
    assert nd.main(["--redactnames", str(h), str(late), str(early)]) == 0
    out = capsys.readouterr().out
    assert out.count("(guest)") == 1
    assert "Guest 1 sent 2 logs" in out and "used 18:53" in out
    assert "different sessions" not in out
