# Open party (0.7.8)

A host with no password that takes plain-address joins (`host:port`, no party code) from its own LAN or VPN.
Code: `gpu/shim/party/party_open_rules.h` (the pure rule, `tests/test_party_open.cpp`), `open_peer_allowed` in
`party_runtime.cpp`, the open key in `party_link.cpp`; the announcement flag in `net/party_discovery.cpp` (flags bit 1)
and the lobby (`net/party_vpnlobby.h`: no password flag = open).

## Switches (all off by default)

| Where | What |
|---|---|
| bbport.ini | `party_open=1` (live: read on every join) |
| env | `BB_PARTY_OPEN` (wins over the ini) |
| Launcher | Party tab, host only: *Open party: no password, join by address (LAN/VPN)*; greyed out while a password is set |
| Game | *System > Party access > Open party (no password)*, host's row (a guest sees *Host's access*, read only) |

## Rules

- A password always wins: with one, nothing is open.
- Scope: `party_open` on = any private or VPN adapter subnet of the host (LAN, Radmin, Tailscale, Nightwatch VPN);
  only the Nightwatch VPN lobby on = the tunnel's subnet. Refused: a public address, the adapter's own gateway, IPv6
  and guests a member relays.
- The open key is public to everyone on those networks (the address check is the only gate): the link's encryption
  does not protect an open party from other players there.
- The host's LAN announcement carries the *open* flag whenever the scope is not empty (also for the VPN-lobby case);
  the launcher's lists show it in the *Pass* column as `open` and join it with an empty password.

## Joining

Launcher: *Parties found* / *Nightwatch VPN games* -> **Join** asks for a password only for a game that has one. By
hand: paste `host:port` into *Party code* and leave *Password* empty. Smoke: phase `openjoin` (group `open`).

## Level range (0.7.9)

A passwordless join by plain address is refused when the guest's level is outside `floor(X*0.8-20) .. floor(X*1.2+20)` of the
host's level X (host 100: 60-140), with *Can't join HOST: your level N is outside their range (a-b)*. A party code or a password
ignores the range (as in the game). Only a new member is checked at the HELLO / AUTH, then its vitals level once known; a
resume is not re-gated. See `invasions.md` "The level range".
