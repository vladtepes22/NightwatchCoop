#!/bin/bash
# Nightwatch VPN on Linux / Steam Deck (Nightwatch Co-op under Wine / Proton).
#
#   nightwatch-vpn-linux.sh setup    add the "Nightwatch" connection to NetworkManager (once; needs its OpenVPN plugin)
#   nightwatch-vpn-linux.sh up       connect
#   nightwatch-vpn-linux.sh down     disconnect
#   nightwatch-vpn-linux.sh status   "active" or "inactive"
#   nightwatch-vpn-linux.sh remove   delete the connection
#
# The launcher writes this script and the files it reads into ~/.local/share/nightwatch-vpn (the profile, Nightwatch.ovpn,
# names the key and certificate files there; a renewal rewrites them, the connection keeps pointing at them). The launcher
# starts the script itself (Wine's `start /unix`) and reads its answer from status.txt: one line, a code and free text.
# You can run it by hand too. It contains no address, key or certificate. No root is needed except to install
# NetworkManager's OpenVPN plugin (networkmanager-openvpn on SteamOS / Arch, network-manager-openvpn on Debian / Ubuntu,
# NetworkManager-openvpn on Fedora).
#
# Inside Steam's Proton container (pressure-vessel) there is no nmcli; the host's is found under /run/host and started with
# the host's own dynamic loader and libraries, and talks to NetworkManager over the system bus the container shares.

set -u
set -f   # the profile's words are never globbed
DIR=${NW_VPN_DIR:-$HOME/.local/share/nightwatch-vpn}
PROFILE=$DIR/Nightwatch.ovpn
CONN=Nightwatch
STATUS=$DIR/status.txt
READY=$DIR/setup.ok
HOST=/run/host

result() {  # result CODE [text]: the launcher's answer, atomically
    mkdir -p "$DIR" 2>/dev/null
    printf '%s\n' "$*" > "$STATUS.new" 2>/dev/null && mv -f "$STATUS.new" "$STATUS" 2>/dev/null
    printf '%s\n' "$*"
}

# ---- nmcli, native or the host's from inside the container ----
NM=()
find_nmcli() {
    if [ -d "$HOST/usr" ] && { [ -n "${PRESSURE_VESSEL_RUNTIME:-}" ] || [ "${container:-}" = pressure-vessel ]; } && [ -x "$HOST/usr/bin/nmcli" ]; then
        local ld="" libs="" f d
        for f in "$HOST/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" "$HOST/usr/lib/ld-linux-x86-64.so.2" \
                 "$HOST/usr/lib64/ld-linux-x86-64.so.2" "$HOST/lib64/ld-linux-x86-64.so.2"; do
            # a regular file only: a symlink here would point into the container's own (different) glibc
            if [ -f "$f" ] && [ ! -L "$f" ]; then ld=$f; break; fi
        done
        for d in usr/lib/x86_64-linux-gnu usr/lib64 usr/lib; do [ -d "$HOST/$d" ] && libs="$libs:$HOST/$d"; done
        if [ -n "$ld" ]; then NM=("$ld" --library-path "${libs#:}" "$HOST/usr/bin/nmcli"); return 0; fi
    fi
    if command -v nmcli >/dev/null 2>&1; then NM=(nmcli); return 0; fi
    return 1
}

plugin_present() {
    local r f
    for r in "" "$HOST"; do
        for f in usr/lib/NetworkManager/VPN usr/lib64/NetworkManager/VPN usr/lib/x86_64-linux-gnu/NetworkManager/VPN etc/NetworkManager/VPN; do
            [ -e "$r/$f/nm-openvpn-service.name" ] && return 0
        done
    done
    return 1
}

nm_running() { [ "$("${NM[@]}" -t -f RUNNING general 2>/dev/null)" = running ]; }
conn_exists() { "${NM[@]}" -t -f NAME connection show 2>/dev/null | grep -qx "$CONN"; }
tidy() { printf '%s' "$1" | tr '\n' ' ' | cut -c1-200; }

# ---- the profile -> NetworkManager's vpn.data (what `nmcli connection import` would make; that command needs the plugin's
# editor library, which is not visible inside the container, so the settings are built here) ----
build_data() {
    local data="connection-type=tls" unmapped="" key val rest proto=udp remote="" host port
    local ca="" cert="" keyf="" tcv2=""
    while IFS= read -r line; do
        line=${line%%#*}; line=${line%%;*}
        # shellcheck disable=SC2086
        set -- $line
        [ $# -ge 1 ] || continue
        key=$1; shift; val=${1:-}
        case $key in
            client|nobind|persist-key|persist-tun|resolv-retry|verb|mute-replay-warnings|auth-nocache|explicit-exit-notify|pull|tls-client|\
            push-peer-info|key-direction|ping|ping-restart|float|rcvbuf|sndbuf|fast-io|mssfix|setenv-safe|script-security|user|group) ;;
            dev) data="$data, dev=${val%%[0-9]*}" ;;
            proto) proto=$val ;;
            remote) host=$1; port=${2:-1194}; [ -n "$remote" ] || remote="$host:$port"; [ -n "${3:-}" ] && proto=$3 ;;
            cipher) data="$data, cipher=$val" ;;
            data-ciphers) data="$data, data-ciphers=$val" ;;
            auth) data="$data, auth=$val" ;;
            remote-cert-tls) data="$data, remote-cert-tls=$val" ;;
            tls-version-min) data="$data, tls-version-min=$val" ;;
            tls-cipher) data="$data, tls-cipher=$val" ;;
            verify-x509-name) data="$data, verify-x509-name=$val:${2:-name}" ;;
            reneg-sec) data="$data, reneg-seconds=$val" ;;
            ca) ca=$val ;; cert) cert=$val ;; key) keyf=$val ;; tls-crypt-v2) tcv2=$val ;;
            *) unmapped="$unmapped $key" ;;
        esac
    done < "$PROFILE"
    [ -n "$remote" ] || return 1
    data="$data, remote=$remote"
    case $proto in tcp*) data="$data, proto-tcp=yes" ;; esac
    data="$data, ca=$DIR/ca.crt, cert=$DIR/client.crt, key=$DIR/client.key, tls-crypt-v2=$DIR/tls-crypt-v2.key"
    VPN_DATA=$data
    UNMAPPED=${unmapped# }
    return 0
}

secure() { chmod 700 "$DIR" 2>/dev/null; chmod 600 "$DIR"/client.key "$DIR"/tls-crypt-v2.key "$DIR"/client.crt "$DIR"/ca.crt "$PROFILE" 2>/dev/null; }

need_nm() {
    find_nmcli || { result "no-nmcli NetworkManager (nmcli) was not found"; return 1; }
    nm_running || { result "nm-down NetworkManager is not running"; return 1; }
    plugin_present || { result "no-plugin NetworkManager's OpenVPN plugin is not installed"; return 1; }
    return 0
}

classify() {  # classify OUTPUT -> prints the code
    case $1 in
        *"nsufficient privileges"*|*"not authorized"*|*"uthorization"*) echo denied ;;
        *) echo failed ;;
    esac
}

cmd_setup() {
    [ -f "$PROFILE" ] || { result "no-profile $PROFILE is missing"; return 1; }
    need_nm || return 1
    VPN_DATA=""; UNMAPPED=""
    build_data || { result "failed the profile has no remote"; return 1; }
    secure
    local out rc
    if conn_exists; then
        out=$("${NM[@]}" connection modify "$CONN" vpn.data "$VPN_DATA" 2>&1); rc=$?
    else
        out=$("${NM[@]}" connection add type vpn vpn-type openvpn con-name "$CONN" ifname -- vpn.data "$VPN_DATA" 2>&1); rc=$?
    fi
    [ $rc -eq 0 ] || { result "$(classify "$out") $(tidy "$out")"; return 1; }
    # a game VPN: never the default route, never DNS, and only when asked
    "${NM[@]}" connection modify "$CONN" connection.autoconnect no ipv4.never-default yes ipv4.ignore-auto-dns yes ipv6.method ignore >/dev/null 2>&1
    : > "$READY"
    result "ok set up${UNMAPPED:+ (not mapped: $UNMAPPED)}"
}

# Without NetworkManager's OpenVPN plugin, run as root (sudo bash this-script up) the profile also works with plain openvpn.
PIDFILE=$DIR/openvpn.pid
plain_up() {
    [ "$(id -u)" = 0 ] && command -v openvpn >/dev/null 2>&1 && [ -f "$PROFILE" ] || return 1
    secure
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then result "ok connected"; return 0; fi
    openvpn --config "$PROFILE" --daemon nightwatch-vpn --writepid "$PIDFILE" --log "$DIR/openvpn.log" --connect-retry-max 3 --resolv-retry 0 >/dev/null 2>&1         || { result "failed openvpn did not start (see $DIR/openvpn.log)"; return 0; }
    local i
    for i in $(seq 1 40); do
        grep -q "Initialization Sequence Completed" "$DIR/openvpn.log" 2>/dev/null && { result "ok connected"; return 0; }
        sleep 1
    done
    result "failed the tunnel did not come up (see $DIR/openvpn.log)"; return 0
}
plain_down() {
    [ -f "$PIDFILE" ] || return 1
    kill "$(cat "$PIDFILE")" 2>/dev/null; rm -f "$PIDFILE"; result "ok disconnected"; return 0
}

cmd_up() {
    if ! plugin_present && plain_up; then return 0; fi
    need_nm || { rm -f "$READY"; return 1; }
    conn_exists || { rm -f "$READY"; result "no-connection the Nightwatch connection is not in NetworkManager"; return 1; }
    secure
    if "${NM[@]}" -t -f NAME connection show --active 2>/dev/null | grep -qx "$CONN"; then result "ok connected"; return 0; fi
    local out
    out=$("${NM[@]}" --wait 75 connection up "$CONN" 2>&1) && { result "ok connected"; return 0; }
    result "$(classify "$out") $(tidy "$out")"
    return 1
}

cmd_down() {
    plain_down && return 0
    find_nmcli || { result "no-nmcli"; return 1; }
    conn_exists || { result "ok not connected"; return 0; }
    local out
    out=$("${NM[@]}" connection down "$CONN" 2>&1)
    # nmcli says "not an active connection" when it is already down; any other complaint (polkit) is an answer
    if "${NM[@]}" -t -f NAME connection show --active 2>/dev/null | grep -qx "$CONN"; then
        result "$(classify "$out") $(tidy "$out")"; return 1
    fi
    result "ok disconnected"
}

cmd_status() {
    [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null && { result "ok active"; return 0; }
    find_nmcli || { result "no-nmcli"; return 1; }
    if "${NM[@]}" -t -f NAME connection show --active 2>/dev/null | grep -qx "$CONN"; then result "ok active"; else result "ok inactive"; fi
}

cmd_remove() {
    find_nmcli || { result "no-nmcli"; return 1; }
    if conn_exists; then
        "${NM[@]}" connection down "$CONN" >/dev/null 2>&1
        "${NM[@]}" connection delete "$CONN" >/dev/null 2>&1 || { result "failed could not delete the connection"; return 1; }
    fi
    rm -f "$READY"
    result "ok removed"
}

case ${1:-} in
    setup) cmd_setup ;;
    up) cmd_up ;;
    down) cmd_down ;;
    status) cmd_status ;;
    remove) cmd_remove ;;
    *) echo "usage: $0 setup|up|down|status|remove" >&2; exit 2 ;;
esac
