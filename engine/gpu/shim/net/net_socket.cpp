// SPDX-License-Identifier: GPL-3.0-or-later
// derived from droogie/bbhost src/hle/net.cpp @8f2746c
//
// libSceNet / libSceNetCtl for party play (BB_PARTY): the game's sockets on real Winsock.
// UDP (type 2) and TCP (type 1) sockets are host sockets; SOCK_DGRAM_P2P (type 6) sockets
// share one UDP port per instance - the game's P2P port 3658 becomes BB_PARTY_PORT (default
// 9307), bound with SO_EXCLUSIVEADDRUSE - multiplexed by the PS4 kernel's virtual-port header
// [0xff][flags][src vport][dst vport]. Hole-punch probes (fe 'bbhp'), STUN Binding traffic and
// relay frames ([0xfb]['R'|'r']) share that port and never reach the game. The party host
// answers STUN Binding Requests on it and relays between guests that cannot reach each other.
// F2 (0.7.1): every datagram of the port passes the route table (party_route.h): an IPv6 socket
// on the same port number and the member relay ([0xfc] frames) under the IPv4 endpoints above.
//
// Errors follow the runtime's libSceNet convention (runtime_services.c): the call returns
// 0x80410100 | errno and sceNetErrnoLoc's word holds the errno, both FreeBSD numbers.
#include "bbnet_internal.h"
#include "net_stun.h"
#include "netsim.h"
#include "nr_outage.h"
#include "nr_rxgap.h"
#include "party_route.h"
#include "party_udp.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bbnet {
namespace {

using Clock = std::chrono::steady_clock;

// FreeBSD errno values (what the guest reads).
namespace bsd {
constexpr int kEINTR = 4, kEIO = 5, kEBADF = 9, kENOMEM = 12, kEACCES = 13, kEFAULT = 14, kEINVAL = 22,
              kEAGAIN = 35, kEINPROGRESS = 36, kEALREADY = 37, kENOTSOCK = 38, kEDESTADDRREQ = 39,
              kEMSGSIZE = 40, kENOPROTOOPT = 42, kEPROTONOSUPPORT = 43, kEOPNOTSUPP = 45,
              kEAFNOSUPPORT = 47, kEADDRINUSE = 48, kEADDRNOTAVAIL = 49, kENETDOWN = 50, kENETUNREACH = 51,
              kECONNABORTED = 53, kECONNRESET = 54, kENOBUFS = 55, kEISCONN = 56, kENOTCONN = 57,
              kETIMEDOUT = 60, kECONNREFUSED = 61, kEHOSTUNREACH = 65;
}  // namespace bsd

constexpr int kNetErrBase = static_cast<int>(0x80410100);
constexpr int kNetCtlInvalidAddr = static_cast<int>(0x80412107);
constexpr int kNetCtlInvalidCode = static_cast<int>(0x80412104);
// SCE_NET_ERROR_RESOLVER_ENOHOST (shadPS4 net_error.h).
constexpr int kResolverNoHost = static_cast<int>(0x804101ea);

// The socket API behind a few names, so the layer reads the same on Windows and Linux
// (winsock: SOCKET handles, closesocket, WSA error numbers, no MSG_DONTWAIT - the sockets
// are non-blocking there and a blocking call waits with WSAPoll first).
#if defined(_WIN32)
using sockfd_t = SOCKET;
constexpr sockfd_t kBadSock = INVALID_SOCKET;
using socklen_type = int;
using ssize_type = int;
inline void sock_close(sockfd_t fd) { ::closesocket(fd); }
inline int last_sock_error() { return ::WSAGetLastError(); }
// The last socket error as a FreeBSD errno.
int sock_errno() {
    switch (::WSAGetLastError()) {
        case WSAEWOULDBLOCK: return bsd::kEAGAIN;
        case WSAEINPROGRESS: return bsd::kEINPROGRESS;
        case WSAEALREADY: return bsd::kEALREADY;
        case WSAENOTSOCK: return bsd::kEBADF;
        case WSAEDESTADDRREQ: return bsd::kEDESTADDRREQ;
        case WSAEMSGSIZE: return bsd::kEMSGSIZE;
        case WSAENOPROTOOPT: return bsd::kENOPROTOOPT;
        case WSAEPROTONOSUPPORT: return bsd::kEPROTONOSUPPORT;
        case WSAEOPNOTSUPP: return bsd::kEOPNOTSUPP;
        case WSAEAFNOSUPPORT: return bsd::kEAFNOSUPPORT;
        case WSAEADDRINUSE: return bsd::kEADDRINUSE;
        case WSAEADDRNOTAVAIL: return bsd::kEADDRNOTAVAIL;
        case WSAENETDOWN: return bsd::kENETDOWN;
        case WSAENETUNREACH: return bsd::kENETUNREACH;
        case WSAECONNABORTED: return bsd::kECONNABORTED;
        case WSAECONNRESET: return bsd::kECONNRESET;
        case WSAENOBUFS: return bsd::kENOBUFS;
        case WSAEISCONN: return bsd::kEISCONN;
        case WSAENOTCONN: return bsd::kENOTCONN;
        case WSAETIMEDOUT: return bsd::kETIMEDOUT;
        case WSAECONNREFUSED: return bsd::kECONNREFUSED;
        case WSAEHOSTUNREACH: return bsd::kEHOSTUNREACH;
        case WSAEACCES: return bsd::kEACCES;  // a port another program holds exclusively
        case WSAEFAULT: return bsd::kEFAULT;
        case WSAEINVAL: return bsd::kEINVAL;
        case WSAEINTR: return bsd::kEINTR;
        default: return bsd::kEIO;
    }
}
int map_so_error(int e) {
    ::WSASetLastError(e);
    return sock_errno();
}
inline void sock_set_nonblock(sockfd_t fd, bool on) {
    u_long v = on ? 1 : 0;
    ::ioctlsocket(fd, FIONBIO, &v);
}
inline int sock_poll(sockfd_t fd, short events, int ms) {
    WSAPOLLFD p{fd, events, 0};
    return ::WSAPoll(&p, 1, ms);
}
inline int sock_poll_revents(sockfd_t fd, short events, short* revents) {
    WSAPOLLFD p{fd, events, 0};
    const int r = ::WSAPoll(&p, 1, 0);
    *revents = r > 0 ? p.revents : 0;
    return r;
}
inline sockfd_t sock_new(int type) {
    const sockfd_t fd = ::socket(AF_INET, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
    if (fd != INVALID_SOCKET) sock_set_nonblock(fd, true);  // blocking calls wait with WSAPoll
    return fd;
}
inline void sock_setopt_int(sockfd_t fd, int level, int name, int v) {
    ::setsockopt(fd, level, name, reinterpret_cast<const char*>(&v), sizeof(v));
}
#else
using sockfd_t = int;
constexpr sockfd_t kBadSock = -1;
using socklen_type = socklen_t;
using ssize_type = ssize_t;
inline void sock_close(sockfd_t fd) { ::close(fd); }
int errno_to_bsd(int e) {
    switch (e) {
        case EAGAIN: return bsd::kEAGAIN;
        case EINPROGRESS: return bsd::kEINPROGRESS;
        case EALREADY: return bsd::kEALREADY;
        case ENOTSOCK: return bsd::kENOTSOCK;
        case EDESTADDRREQ: return bsd::kEDESTADDRREQ;
        case EMSGSIZE: return bsd::kEMSGSIZE;
        case ENOPROTOOPT: return bsd::kENOPROTOOPT;
        case EPROTONOSUPPORT: return bsd::kEPROTONOSUPPORT;
        case EOPNOTSUPP: return bsd::kEOPNOTSUPP;
        case EAFNOSUPPORT: return bsd::kEAFNOSUPPORT;
        case EADDRINUSE: return bsd::kEADDRINUSE;
        case EADDRNOTAVAIL: return bsd::kEADDRNOTAVAIL;
        case ENETDOWN: return bsd::kENETDOWN;
        case ENETUNREACH: return bsd::kENETUNREACH;
        case ECONNABORTED: return bsd::kECONNABORTED;
        case ECONNRESET: return bsd::kECONNRESET;
        case ENOBUFS: return bsd::kENOBUFS;
        case EISCONN: return bsd::kEISCONN;
        case ENOTCONN: return bsd::kENOTCONN;
        case ETIMEDOUT: return bsd::kETIMEDOUT;
        case ECONNREFUSED: return bsd::kECONNREFUSED;
        case EHOSTUNREACH: return bsd::kEHOSTUNREACH;
        default: return e <= 34 ? e : bsd::kEIO;  // 1..34 coincide
    }
}
int sock_errno() { return errno_to_bsd(errno); }
int map_so_error(int e) { return errno_to_bsd(e); }
inline int last_sock_error() { return errno; }
inline void sock_set_nonblock(sockfd_t fd, bool on) {
    const int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}
inline int sock_poll(sockfd_t fd, short events, int ms) {
    pollfd p{fd, events, 0};
    return ::poll(&p, 1, ms);
}
inline int sock_poll_revents(sockfd_t fd, short events, short* revents) {
    pollfd p{fd, events, 0};
    const int r = ::poll(&p, 1, 0);
    *revents = r > 0 ? p.revents : 0;
    return r;
}
inline sockfd_t sock_new(int type) {
    const sockfd_t fd = ::socket(AF_INET, type, 0);
    if (fd >= 0) sock_set_nonblock(fd, true);  // as on Windows: blocking calls wait with poll
    return fd;
}
inline void sock_setopt_int(sockfd_t fd, int level, int name, int v) { ::setsockopt(fd, level, name, &v, sizeof(v)); }
#endif
inline bool sock_ok(sockfd_t fd) { return fd != kBadSock; }
inline bool would_block() {
#if defined(_WIN32)
    return ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

std::mutex g_link_handler_mu;
std::function<void(std::uint32_t, std::uint16_t, const std::uint8_t*, std::size_t)> g_link_handler;

ssize_type raw_recvfrom(sockfd_t fd, void* buf, std::size_t len, int flags, sockaddr_in* sa, socklen_type* sl) {
    return ::recvfrom(fd, static_cast<char*>(buf), static_cast<int>(len), flags, reinterpret_cast<sockaddr*>(sa), sl);
}
// Every UDP datagram leaves here: through the network simulator when BB_NET_SIM is on.
ssize_type raw_sendto(sockfd_t fd, const void* buf, std::size_t len, const sockaddr_in* sa) {
    if (netsim::enabled() && netsim::config().nat) netsim::nat_note_out(sa->sin_addr.s_addr, sa->sin_port);
    if (netsim::enabled() && netsim::applies_to(netsim::config(), sa->sin_addr.s_addr)) {
        return netsim::sendto(static_cast<std::uintptr_t>(fd), buf, len, sa->sin_addr.s_addr, sa->sin_port);
    }
    return ::sendto(fd, static_cast<const char*>(buf), static_cast<int>(len), 0, reinterpret_cast<const sockaddr*>(sa),
                    sizeof(*sa));
}

// The game's epoll over one wake word instead of the kernel's epoll: a P2P socket is readable
// when its inbox has a datagram; a host socket is polled with a zero timeout; a wait sleeps on
// the word every reader, control change and abort bumps, ten milliseconds at a time for the
// host sockets. The word is read before the sockets are looked at, so a datagram that lands
// between the look and the sleep wakes it.
std::atomic<std::uint32_t> g_wake_word{0};
#if !defined(_WIN32)
std::mutex g_wake_mu;
std::condition_variable g_wake_cv;
#endif
void net_wake() {
    g_wake_word.fetch_add(1, std::memory_order_seq_cst);
#if defined(_WIN32)
    ::WakeByAddressAll(reinterpret_cast<void*>(&g_wake_word));
#else
    std::lock_guard<std::mutex> lk(g_wake_mu);
    g_wake_cv.notify_all();
#endif
}
void wake_wait(std::uint32_t gen, Clock::time_point until) {
    const auto now = Clock::now();
    if (now >= until) return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count();
    if (ms <= 0) {
        // Below a millisecond: the timeout is honoured by yielding, not rounded to zero
        // (KyoPS4x #215: an empty epoll spinning a core on a timeout it ignored).
        while (Clock::now() < until && g_wake_word.load(std::memory_order_seq_cst) == gen) std::this_thread::yield();
        return;
    }
#if defined(_WIN32)
    std::uint32_t expected = gen;
    ::WaitOnAddress(reinterpret_cast<void*>(&g_wake_word), &expected, sizeof(expected), static_cast<DWORD>(ms));
#else
    std::unique_lock<std::mutex> lk(g_wake_mu);
    g_wake_cv.wait_until(lk, until, [&] { return g_wake_word.load(std::memory_order_seq_cst) != gen; });
#endif
}

std::uint16_t bswap16(std::uint16_t v) { return static_cast<std::uint16_t>((v << 8) | (v >> 8)); }
std::uint32_t bswap32(std::uint32_t v) {
    return (v << 24) | ((v << 8) & 0x00ff0000u) | ((v >> 8) & 0x0000ff00u) | (v >> 24);
}

int parse_ipv4(const char* s, std::uint32_t* net) {
    if (!s || !net) return 0;
    unsigned a = 0, b = 0, c = 0, d = 0;
    char extra = 0;
    if (std::sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    *net = bswap32((a << 24) | (b << 16) | (c << 8) | d);
    return 1;
}
void write_ipv4(char* dst, unsigned n, std::uint32_t net) {
    if (!dst || n == 0) return;
    const std::uint32_t host = bswap32(net);
    std::snprintf(dst, n, "%u.%u.%u.%u", (host >> 24) & 0xffu, (host >> 16) & 0xffu, (host >> 8) & 0xffu,
                  host & 0xffu);
}

void wsa_start() {
#if defined(_WIN32)
    static const bool started = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)started;
#endif
}

thread_local int g_net_errno = 0;
int g_net_next = 1;
std::mutex g_net_mu;

int net_err(int bsd_errno) {
    g_net_errno = bsd_errno;
    return kNetErrBase | (bsd_errno & 0xff);
}
// The failing errno of a real call, as the game's sceNetErrnoLoc and return value.
int net_fail() { return net_err(sock_errno()); }
int net_again() { return net_err(bsd::kEAGAIN); }

// SceNetSockaddrIn: {len, family, port (nbo), addr (nbo), vport (nbo), zero[6]}.
struct SceSockaddrIn {
    std::uint8_t len;
    std::uint8_t family;
    std::uint16_t port;
    std::uint32_t addr;
    std::uint16_t vport;
    std::uint8_t zero[6];
};
static_assert(sizeof(SceSockaddrIn) == 16, "SceNetSockaddrIn is 16 bytes");

// The PS4 kernel multiplexes virtual ports over one UDP port with a header on every
// datagram: [0xff][flags][src vport][dst vport] when flag 0x40 says the vports are one byte
// (Bloodborne's are, 40 and 30), two-byte vports without it, four more bytes when flag 0x20
// adds a comid. The header is kept on the wire so a shadPS4 or bbhost peer reads ours.
using udp::kRelayHeader;
using udp::p2p_header;
using udp::p2p_write_header;
constexpr std::uint16_t kGameP2pPort = 3658;

struct Datagram {
    std::vector<std::uint8_t> data;
    std::uint32_t addr = 0;   // nbo
    std::uint16_t port = 0;   // nbo
    std::uint16_t vport = 0;  // host order, the sender's
};
// A P2P game socket's inbox: what the port's reader demultiplexed to its vport.
struct SockQueue {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Datagram> q;
    int sock = 0;
    std::uint16_t vport = 0;
    // Counters for the desync work: what came in, what went out, how deep the inbox got.
    std::uint64_t in_dgrams = 0, in_bytes = 0, out_dgrams = 0, out_bytes = 0, forwarded = 0;
    std::size_t hiwater = 0;
    // When each source (as the game sees it, source_key) last reached this socket: C6 finds a
    // crashed member's connection by its silence.
    std::map<std::uint64_t, Clock::time_point> heard;
    // F5: each source's last small datagram (its keepalive / ack traffic), replayed to the game
    // during a network outage the party link knows about (p2p_outage_keepalive).
    std::map<std::uint64_t, Datagram> last_small;
    // F5 (nr_outage.h): each source's last reliable data record (the keepalive that resets the
    // game's idle deadline), and what the game sent each peer for a replay after its silence.
    std::map<std::uint64_t, Datagram> keepalive;
    std::map<std::uint64_t, nr::PeerReplay> replay;
    std::map<std::uint64_t, SceSockaddrIn> replay_to;
    std::map<std::uint64_t, Clock::time_point> strip_until;  // no dup-ack reports (resume)
    std::uint64_t stripped = 0;
    std::uint64_t replayed = 0;
};
// The party UDP port, shared by every game socket that binds a vport on it (the game binds
// 40 at boot and 30 for a session, both on 3658). A reader thread drains it into the queues.
struct P2pPort {
    sockfd_t fd = kBadSock;
    std::atomic<sockfd_t> fd6{kBadSock};  // F2: the IPv6 socket on the same port number (p2p_set_ipv6)
    std::uint16_t port = 0;  // host order
    std::mutex mu;
    std::map<std::uint16_t, std::shared_ptr<SockQueue>> by_vport;
    std::thread reader;
    std::atomic<bool> stop{false};
    std::uint64_t rx = 0, unmatched = 0, no_header = 0, probes = 0, stun_answered = 0, relayed = 0;
    Clock::time_point last_report{};
    // One STUN Binding exchange at a time (p2p_stun): the reader answers the transaction it
    // finds here (under mu).
    bool stun_pending = false, stun_done = false;
    std::uint8_t stun_txid[net::stun::kTxid] = {};
    std::uint32_t stun_addr = 0;
    std::uint16_t stun_port = 0;
    std::uint32_t stun_server_addr = 0;     // where the pending request went (nbo): only its
    std::uint16_t stun_server_port = 0;     // answer counts
    net::stun::Relay stun_relay;
    std::condition_variable stun_cv;
    // When a datagram last arrived from each source (addr | port << 32, both network
    // order): the hole-punch stops once the peer is heard.
    std::map<std::uint64_t, Clock::time_point> heard;
    std::atomic<std::uint64_t> dropped{0};  // datagrams a full game inbox did not take (kMaxInbox)
};
// Peer input caps (bbport security pass): the party port is on the internet, so neither a
// socket's inbox nor the heard table may grow with what strangers send.
constexpr std::size_t kMaxInbox = 1024;     // datagrams queued for one game socket
constexpr std::size_t kMaxHeard = 4096;     // sources remembered
constexpr std::size_t kSmallDgram = 96;     // F5: a keepalive-sized datagram (replayed in an outage)
// Every IPv4 address of this machine (all adapters, loopback included, plus the forced
// BB_PARTY_LOCAL_IP / chosen LAN address) with its mask, cached for 10 s. Used to recognise our
// own endpoint: a peer in another home can report the very same LAN address as ours.
// BB_PARTY_SELF_GUARD=0 turns the self-endpoint checks off (tests only).
bool self_guard_on() {
    static const bool on = [] {
        const char* v = std::getenv("BB_PARTY_SELF_GUARD");
        return !(v && v[0] == '0');
    }();
    return on;
}
std::vector<udp::OwnIf> own_ifaces() {
    static std::mutex mu;
    static std::vector<udp::OwnIf> cache;
    static Clock::time_point at{};
    std::lock_guard<std::mutex> lk(mu);
    if (!cache.empty() && Clock::now() - at < std::chrono::seconds(10)) return cache;
    std::vector<udp::OwnIf> out;
#if defined(_WIN32)
    ULONG size = 16 * 1024;
    std::vector<std::uint8_t> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 4 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        rc = ::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                    nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc == NO_ERROR) {
        for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
                udp::OwnIf o;
                o.ip = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr;
                const ULONG bits = u->OnLinkPrefixLength;
                o.mask = bits ? bswap32(bits >= 32 ? 0xffffffffu : ~(0xffffffffu >> bits)) : 0;
                out.push_back(o);
            }
        }
    }
#else
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) == 0) {
        for (ifaddrs* i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
            udp::OwnIf o;
            o.ip = reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr;
            o.mask = i->ifa_netmask ? reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr : 0;
            out.push_back(o);
        }
        ::freeifaddrs(list);
    }
#endif
    if (const std::uint32_t l = local_ipv4()) {
        bool have = false;
        for (const udp::OwnIf& o : out) have = have || o.ip == l;
        if (!have) out.push_back({l, 0});
    }
    cache = out;
    at = Clock::now();
    return out;
}
void note_heard_locked(P2pPort& port, std::uint64_t key) {
    const auto now = Clock::now();
    if (port.heard.size() >= kMaxHeard && !port.heard.count(key)) {
        for (auto it = port.heard.begin(); it != port.heard.end();) {
            if (now - it->second > std::chrono::seconds(30)) it = port.heard.erase(it);
            else ++it;
        }
        if (port.heard.size() >= kMaxHeard) port.heard.clear();
    }
    port.heard[key] = now;
}
// The relay (party_udp.h). Client side: once a STUN answer carried BBHOST-RELAY, a datagram for
// another relay port (the server's address, not its STUN port) leaves framed for the STUN port
// and the relay's deliveries come back from the STUN port. The game sees none of it: its peers
// stay server:port both ways. Server side (the party host): STUN answers with a token and a
// relay port, frames forwarded between clients.
udp::RelayClient g_relay;
// A guest's paths to the other guests (party_udp.h PeerPaths): direct while their probes are
// answered, else through the host relay; BB_PARTY_FORCE_RELAY=1 relays them always.
udp::EndpointAliases& aliases() {
    static udp::EndpointAliases* a = new udp::EndpointAliases;
    return *a;
}

void alias_out(sockaddr_in& sa) {
    std::uint32_t a = sa.sin_addr.s_addr;
    std::uint16_t p = sa.sin_port;
    if (aliases().outgoing(&a, &p)) {
        sa.sin_addr.s_addr = a;
        sa.sin_port = p;
    }
}
void alias_in(sockaddr_in& sa) {
    std::uint32_t a = sa.sin_addr.s_addr;
    std::uint16_t p = sa.sin_port;
    if (aliases().incoming(&a, &p)) {
        sa.sin_addr.s_addr = a;
        sa.sin_port = p;
    }
}

udp::PeerPaths& paths() {
    static udp::PeerPaths* p = [] {
        udp::PeerPaths::Config c;
        c.force = udp::force_relay_from_env();
        // 0.7.6: a pair on the host relay stays there 45 s and goes back to direct only after 8 s of
        // unbroken probe answers; two game traffic gaps within 2 min put a pair on the relay
        // (BB_PARTY_PEER_GAP_RELAY=0 off; the hold and stable times are for tests).
        const char* g = std::getenv("BB_PARTY_PEER_GAP_RELAY");
        c.gaps_to_relay = g && g[0] == '0' ? 0 : 2;
        if (const char* h = std::getenv("BB_PARTY_PEER_RELAY_HOLD_MS")) c.hold_ms = std::max(0, std::atoi(h));
        else c.hold_ms = 45000;
        if (const char* s = std::getenv("BB_PARTY_PEER_RELAY_STABLE_MS")) c.stable_ms = std::max(0, std::atoi(s));
        else c.stable_ms = 8000;
        return new udp::PeerPaths(c);
    }();
    return *p;
}
udp::RelayServer g_relay_server;
inline std::uint64_t source_key(std::uint32_t addr, std::uint16_t port_nbo) {
    return addr | (static_cast<std::uint64_t>(port_nbo) << 32);
}

// F2 (0.7.1): the party port's route table (party_route.h): IPv6 and the member relay.
udp::Router& router() {
    static udp::Router* r = new udp::Router;
    return *r;
}
std::atomic<bool> g_ipv6_wanted{false};
void flush_route_notes() {
    for (const std::string& line : router().take_notes()) log("%s", line.c_str());
}
// One datagram as the route table put it on the wire.
ssize_type wire_send(P2pPort& port, const udp::Wire& w) {
    if (!w.v6) {
        sockaddr_in t{};
        t.sin_family = AF_INET;
        t.sin_addr.s_addr = w.addr;
        t.sin_port = w.port;
        return raw_sendto(port.fd, w.data.data(), w.data.size(), &t);
    }
    const sockfd_t fd6 = port.fd6.load();
    if (!sock_ok(fd6)) return -1;
    sockaddr_in6 t6{};
    t6.sin6_family = AF_INET6;
    std::memcpy(&t6.sin6_addr, w.addr6.b.data(), 16);
    t6.sin6_port = w.port;
    return ::sendto(fd6, reinterpret_cast<const char*>(w.data.data()), static_cast<int>(w.data.size()), 0,
                    reinterpret_cast<const sockaddr*>(&t6), sizeof t6);
}
// Every datagram of the party port leaves here: to the IPv4 endpoint `sa` as the layers above
// know it, through the route table (IPv6, a member relay) onto the wire. A datagram the route
// cannot carry yet (a relay handshake, an IPv6 key without IPv6) counts as sent and lost.
ssize_type port_send(P2pPort& port, const void* buf, std::size_t len, const sockaddr_in* sa) {
    std::vector<udp::Wire> out;
    router().outgoing(sa->sin_addr.s_addr, sa->sin_port, static_cast<const std::uint8_t*>(buf), len, Clock::now(),
                      &out);
    ssize_type r = static_cast<ssize_type>(len);
    for (const udp::Wire& w : out)
        if (wire_send(port, w) < 0) r = -1;
    return r;
}

// One line per port every 60 s (and at close): the stream's shape.
void p2p_report_locked(P2pPort& port, const char* when) {
    std::string line;
    char buf[192];
    for (auto& [vp, q] : port.by_vport) {
        std::lock_guard<std::mutex> lk(q->mu);
        std::snprintf(buf, sizeof(buf), " vport %u (sock %d): in %llu/%llu B, out %llu/%llu B, hiwater %zu, fwd %llu;",
                      vp, q->sock, static_cast<unsigned long long>(q->in_dgrams),
                      static_cast<unsigned long long>(q->in_bytes), static_cast<unsigned long long>(q->out_dgrams),
                      static_cast<unsigned long long>(q->out_bytes), q->hiwater,
                      static_cast<unsigned long long>(q->forwarded));
        line += buf;
    }
    log("p2p port %u %s: rx %llu, unmatched %llu, no header %llu, probes %llu, stun %llu, relayed %llu;%s", port.port,
        when, static_cast<unsigned long long>(port.rx), static_cast<unsigned long long>(port.unmatched),
        static_cast<unsigned long long>(port.no_header), static_cast<unsigned long long>(port.probes),
        static_cast<unsigned long long>(port.stun_answered), static_cast<unsigned long long>(port.relayed),
        line.c_str());
}
std::map<std::uint16_t, std::shared_ptr<P2pPort>> g_p2p_ports;  // under g_net_mu
std::atomic<std::uint16_t> g_bound_port{0};

// What a socket shares with its waiters (copied NetSocks point at the same one).
struct SockCtl {
    std::atomic<bool> aborted{false};
    std::atomic<int> rcvtimeo_us{0}, sndtimeo_us{0};
};
struct NetSock {
    int domain = 2;
    int type = 1;
    bool nonblock = false;
    bool listening = false;
    // UDP (2) and TCP (1) sockets are host sockets; SOCK_DGRAM_P2P (6) sockets share the
    // party port through `port` and read from `queue`. Other types are inert handles.
    sockfd_t fd = kBadSock;
    bool p2p = false;
    std::uint16_t vport = 0;  // host order, what the game bound
    std::shared_ptr<P2pPort> port;
    std::shared_ptr<SockQueue> queue;
    std::shared_ptr<SockCtl> ctl = std::make_shared<SockCtl>();
};
struct NetEpoll {
    std::string name;
    std::atomic<bool> aborting{false};
    std::unordered_map<int, std::uint32_t> watch;
    std::unordered_map<int, std::uint64_t> data;  // socket id -> the game's epoll data
};
std::unordered_map<int, NetSock> g_net_socks;
std::unordered_map<int, std::shared_ptr<NetEpoll>> g_net_epolls;
std::unordered_map<int, int> g_net_resolvers;
std::unordered_map<int, std::pair<void*, void*>> g_net_ctl_cbs;
int g_net_ctl_cb_next = 1;

// The first datagrams are always logged (the P2P handshake is a few dozen small packets and
// the runs that fail are the ones without a trace); BB_NET_TRACE=1 removes the cap.
const bool g_net_trace = [] {
    const char* e = std::getenv("BB_NET_TRACE");
    return e && e[0] == '1';
}();
constexpr int kTraceFirst = 400;
// RE aid BB_NET_HEX=N: every game P2P datagram's first N bytes (both ways), for the transport's
// header (F5 outage work).
const int g_net_hex = [] {
    const char* e = std::getenv("BB_NET_HEX");
    return e ? std::max(0, std::min(64, std::atoi(e))) : 0;
}();
// F5: BB_PARTY_OUTAGE_REPLAY=0 turns off the resume replay and the reliable keepalive record.
const bool g_outage_replay = [] {
    const char* e = std::getenv("BB_PARTY_OUTAGE_REPLAY");
    return !(e && e[0] == '0');
}();
// 0.7.6 diagnostics BB_NET_RXGAP_LOG=1 (nr_rxgap.h): was it the peer that went quiet, or this PC's
// reader thread that did not run? One line per silence of 2 s or more when it ends, one line a minute.
const bool g_rxgap_log = [] {
    const char* e = std::getenv("BB_NET_RXGAP_LOG");
    return e && e[0] == '1';
}();
// When the game last took a datagram out of a P2P inbox (steady clock, ns); 0: never.
std::atomic<std::int64_t> g_game_read_ns{0};
// Test aid with BB_NET_RXGAP_LOG: BB_NET_RXGAP_TEST_STALL=<ms>@<s> makes the reader thread sleep <ms> once,
// <s> seconds after it started, to show the log telling a stalled reader from a quiet peer.
struct RxStallTest {
    int ms = 0, at_s = 0;
    bool done = false;
};
RxStallTest rx_stall_test_from_env() {
    RxStallTest t;
    if (const char* e = std::getenv("BB_NET_RXGAP_TEST_STALL")) {
        int ms = 0, s = 0;
        if (std::sscanf(e, "%d@%d", &ms, &s) == 2 && ms > 0) {
            t.ms = ms;
            t.at_s = s;
        }
    }
    return t;
}
std::int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
int dgram_send(int s, const void* buf, std::uint64_t len, int flags, const SceSockaddrIn* to);
std::mutex g_gap_hook_mu;
std::function<void(std::uint32_t, std::uint16_t, double)> g_gap_hook;
// Sends the kept records again (nr_outage.h), off the reader thread, a few at a time.
void resume_replay(int sock, SceSockaddrIn to, std::vector<std::vector<std::uint8_t>> dgs, std::size_t records,
                   double silent_s) {
    char ip[32];
    write_ipv4(ip, sizeof(ip), to.addr);
    log("outage resume: %s:%u heard again after %.1f s: %zu reliable record(s) sent again in %zu datagram(s)", ip,
        bswap16(to.port), silent_s, records, dgs.size());
    std::thread([sock, to, dgs = std::move(dgs)] {
        for (std::size_t i = 0; i < dgs.size(); ++i) {
            dgram_send(sock, dgs[i].data(), dgs[i].size(), 0, &to);
            if (i % 8 == 7) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }).detach();
}
void hex_trace(bool send, unsigned src, unsigned dst, const std::uint8_t* d, std::size_t n) {
    if (!g_net_hex || !d) return;
    char hex[64 * 3 + 1] = {};
    const std::size_t m = std::min<std::size_t>(n, static_cast<std::size_t>(g_net_hex));
    for (std::size_t i = 0; i < m; ++i) std::snprintf(hex + 3 * i, 4, "%02x ", d[i]);
    log("hex %s %u->%u %zu: %s", send ? "out" : "in ", src, dst, n, hex);
}
bool trace_now(std::atomic<int>& count) {
    if (g_net_trace) return true;
    return count.fetch_add(1, std::memory_order_relaxed) < kTraceFirst;
}

int net_alloc() { return g_net_next++; }

std::atomic<PacketTraceFn> g_packet_trace{nullptr};
std::atomic<P2pRecvHookFn> g_p2p_recv_hook{nullptr};

// bbport: the host answers a peer's STUN Binding Request (and its relay HELLO).
void answer_stun(P2pPort& port, const std::uint8_t* buf, std::size_t n, const sockaddr_in& sa) {
    std::uint8_t out[net::stun::kMaxResponse];
    std::string note;
    const std::size_t len =
        g_relay_server.answer_stun(buf, n, sa.sin_addr.s_addr, sa.sin_port, settings().host, out, &note);
    if (!len) return;
    if (!note.empty()) log("%s", note.c_str());
    port_send(port, out, len, &sa);
    std::lock_guard<std::mutex> lk(port.mu);
    ++port.stun_answered;
}
// bbport: the host forwards a guest's relay frame to the client owning the destination port.
void relay_forward(P2pPort& port, std::uint8_t* buf, std::size_t n, const sockaddr_in& sa) {
    std::uint8_t* frame = nullptr;
    std::size_t len = 0;
    std::uint32_t to_addr = 0;
    std::uint16_t to_port = 0;
    std::string note;
    const bool ok = g_relay_server.forward(buf, n, sa.sin_addr.s_addr, sa.sin_port, &frame, &len, &to_addr, &to_port,
                                           &note);
    if (!note.empty()) log("%s", note.c_str());
    if (!ok) return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = to_addr;
    to.sin_port = to_port;
    port_send(port, frame, len, &to);
    std::lock_guard<std::mutex> lk(port.mu);
    ++port.relayed;
}

// Waits for a datagram on the IPv4 socket or the IPv6 one (kBadSock: none); *r4 / *r6 say which.
int poll2(sockfd_t fd, sockfd_t fd6, int ms, bool* r4, bool* r6) {
    *r4 = *r6 = false;
#if defined(_WIN32)
    WSAPOLLFD p[2] = {{fd, POLLIN, 0}, {fd6, POLLIN, 0}};
    const int r = ::WSAPoll(p, sock_ok(fd6) ? 2 : 1, ms);
#else
    pollfd p[2] = {{fd, POLLIN, 0}, {fd6, POLLIN, 0}};
    const int r = ::poll(p, sock_ok(fd6) ? 2 : 1, ms);
#endif
    if (r <= 0) return r;
    *r4 = p[0].revents != 0;
    *r6 = sock_ok(fd6) && p[1].revents != 0;
    return r;
}

void p2p_reader(std::shared_ptr<P2pPort> port) {
    std::uint8_t buf[udp::kMaxDatagram + kRelayHeader];
    std::atomic<int> logs{0};
    auto last_expire = Clock::now();
    auto last_tick = Clock::now();
    std::vector<std::pair<std::uint32_t, std::uint16_t>> probes, probes6;
    std::vector<std::string> notes;
    std::vector<udp::Wire> rout;
    std::vector<std::uint8_t> inner;
    nr::RxGapMonitor rxgap;
    RxStallTest rx_stall = g_rxgap_log ? rx_stall_test_from_env() : RxStallTest{};
    const auto reader_started = Clock::now();
    while (!port->stop.load(std::memory_order_relaxed)) {
        if (settings().host && Clock::now() - last_expire > std::chrono::seconds(60)) {
            last_expire = Clock::now();
            g_relay_server.expire(last_expire);
        }
        if (Clock::now() - last_tick > std::chrono::milliseconds(100)) {
            last_tick = Clock::now();
            // The route table: relay handshakes, keepalives, expiry (party_route.h).
            rout.clear();
            router().poll(last_tick, &rout);
            for (const udp::Wire& w : rout) wire_send(*port, w);
            if (!settings().host) {
                // Probe every other guest directly (relayed or not), over IPv6 too where it has an
                // IPv6 endpoint, and switch paths on the answers.
                probes.clear();
                probes6.clear();
                notes.clear();
                paths().tick(last_tick, &probes, &notes, &probes6);
                for (const auto& [a, p] : probes) {
                    sockaddr_in t{};
                    t.sin_family = AF_INET;
                    t.sin_addr.s_addr = a;
                    t.sin_port = p;
                    alias_out(t);
                    raw_sendto(port->fd, udp::kProbe, sizeof(udp::kProbe), &t);
                }
                rout.clear();
                for (const auto& [a, p] : probes6) router().outgoing_v6(a, p, udp::kProbe, sizeof(udp::kProbe), &rout);
                for (const udp::Wire& w : rout) wire_send(*port, w);
                for (const std::string& line : notes) log("%s", line.c_str());
                if (!probes6.empty())
                    for (const auto& pv : paths().peers())
                        if (pv.v6) router().route_v6(pv.addr, pv.port, pv.via6);
            }
            flush_route_notes();
            if (rx_stall.ms > 0 && !rx_stall.done && last_tick - reader_started >= std::chrono::seconds(rx_stall.at_s)) {
                rx_stall.done = true;
                log("rx watch test: the reader sleeps %d ms now (BB_NET_RXGAP_TEST_STALL)", rx_stall.ms);
                std::this_thread::sleep_for(std::chrono::milliseconds(rx_stall.ms));
            }
            nr::RxGapMonitor::Summary sum;
            if (g_rxgap_log && rxgap.summary(last_tick, &sum)) {
                std::string peers;
                for (const auto& [k, p] : sum.by_peer) {
                    char ip[32], b[96];
                    write_ipv4(ip, sizeof(ip), static_cast<std::uint32_t>(k & 0xffffffffu));
                    std::snprintf(b, sizeof(b), " %s:%u %llu dgrams, longest silence %.1f s;", ip,
                                  bswap16(static_cast<std::uint16_t>(k >> 32)),
                                  static_cast<unsigned long long>(p.datagrams), p.longest_s);
                    peers += b;
                }
                log("rx watch 60 s: %llu game datagrams from %d source(s), %d silence(s) of 2 s or more (%d not "
                    "logged singly); reader: %llu waits, %llu idle, worst stall %.0f ms%s%s;%s",
                    static_cast<unsigned long long>(sum.datagrams), sum.peers, sum.gaps, sum.suppressed,
                    static_cast<unsigned long long>(sum.polls), static_cast<unsigned long long>(sum.timeouts),
                    sum.worst_stall_ms, sum.worst_stall_ms > 0 ? " in its " : "", sum.stall_kind, peers.c_str());
            }
        }
        bool r4 = false, r6 = false;
        const sockfd_t fd6 = port->fd6.load();
        if (g_rxgap_log) rxgap.poll_begin(Clock::now());
        const int polled = poll2(port->fd, fd6, 200, &r4, &r6);
        if (g_rxgap_log) rxgap.poll_end(Clock::now(), polled == 0, 200);
        if (polled <= 0) continue;
        sockaddr_in sa{};
        udp::Wire from;
        ssize_type n = -1;
        if (r4) {
            socklen_type sl = sizeof(sa);
            n = raw_recvfrom(port->fd, buf, sizeof(buf), 0, &sa, &sl);
            // A simulated NAT (BB_NET_SIM nat=1): nothing from an endpoint we have not sent to.
            if (n >= 0 && netsim::enabled() && netsim::config().nat &&
                netsim::applies_to(netsim::config(), sa.sin_addr.s_addr) &&
                !netsim::nat_allow_in(sa.sin_addr.s_addr, sa.sin_port))
                continue;
            from.addr = sa.sin_addr.s_addr;
            from.port = sa.sin_port;
        }
        if (n < 0 && r6) {
            sockaddr_in6 s6{};
            socklen_type sl6 = sizeof(s6);
            n = ::recvfrom(fd6, reinterpret_cast<char*>(buf), static_cast<int>(sizeof(buf)), 0,
                           reinterpret_cast<sockaddr*>(&s6), &sl6);
            from.v6 = true;
            std::memcpy(from.addr6.b.data(), &s6.sin6_addr, 16);
            from.port = s6.sin6_port;
        }
        if (n < 0) continue;  // Windows: an ICMP port unreachable surfaces as WSAECONNRESET
        // Our own hole-punch probe looped back (a peer in another home with the same LAN address as ours,
        // probed at that address): never a datagram from a peer, never "heard".
        if (!from.v6 && self_guard_on() && from.port == bswap16(port->port) &&
            udp::is_probe(buf, static_cast<std::size_t>(n)) &&
            udp::is_own_endpoint(from.addr, from.port, bswap16(port->port), own_ifaces()))
            continue;
        // The route table (party_route.h): an IPv6 source becomes its IPv4 key, member relay frames
        // are answered, forwarded or unwrapped; what is left reaches the layers below as before.
        from.data.assign(buf, buf + n);
        rout.clear();
        bool via6 = false, via_relay = false;
        std::uint32_t ka = 0;
        std::uint16_t kp = 0;
        const bool deliver = router().incoming(from, Clock::now(), &inner, &ka, &kp, &rout, &via6, &via_relay);
        for (const udp::Wire& w : rout) wire_send(*port, w);
        if (!deliver || inner.size() > sizeof(buf)) continue;
        std::memcpy(buf, inner.data(), inner.size());
        n = static_cast<ssize_type>(inner.size());
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = ka;
        sa.sin_port = kp;
        // The party link's UDP path (party_rudp.h) and LAN discovery (party_lan.h: probes / replies).
        if ((n >= 16 && buf[0] == 0xfa && buf[1] == 'L') || (n >= 4 && buf[0] == 0xfa && buf[1] == 'D')) {
            std::function<void(std::uint32_t, std::uint16_t, const std::uint8_t*, std::size_t)> h;
            {
                std::lock_guard<std::mutex> lk(g_link_handler_mu);
                h = g_link_handler;
            }
            if (h) h(sa.sin_addr.s_addr, sa.sin_port, buf, static_cast<std::size_t>(n));
            continue;
        }
        if (udp::is_relay_request(buf, static_cast<std::size_t>(n)) && settings().host) {
            relay_forward(*port, buf, static_cast<std::size_t>(n), sa);
            continue;
        }
        std::uint16_t relayed_from = 0;
        if (g_relay.unwrap(buf, static_cast<std::size_t>(n), sa.sin_addr.s_addr, sa.sin_port, &relayed_from)) {
            // A relay delivery: the datagram of the owner of the source port, which the game
            // knows as server:port.
            std::memmove(buf, buf + udp::kDeliveryHeader, static_cast<std::size_t>(n) - udp::kDeliveryHeader);
            n -= static_cast<ssize_type>(udp::kDeliveryHeader);
            sa.sin_port = relayed_from;
            // A peer we know by its direct address: the game sees it from there, whichever path.
            std::uint32_t da = 0;
            std::uint16_t dp = 0;
            if (paths().direct_of_relay(bswap16(relayed_from), &da, &dp)) {
                sa.sin_addr.s_addr = da;
                sa.sin_port = dp;
            }
        }
        std::uint16_t src = 0, dst = 0;
        const std::size_t hdr = p2p_header(buf, static_cast<std::size_t>(n), &src, &dst);
        std::shared_ptr<SockQueue> q;
        bool forwarded = false;
        if (!hdr) {
            // Not the game's: a peer's hole-punch probe, a STUN request (answered) or the
            // STUN server's answer to our request. None reaches a game socket.
            if (udp::is_probe(buf, static_cast<std::size_t>(n))) {
                if (udp::is_probe_ack(buf, static_cast<std::size_t>(n))) {
                    std::uint32_t pa = sa.sin_addr.s_addr;
                    std::uint16_t pp = sa.sin_port;
                    aliases().incoming(&pa, &pp);  // an aliased peer's ack counts for its primary
                    if (via6) paths().on_ack6(pa, pp, Clock::now());
                    else paths().on_ack(pa, pp, Clock::now());
                } else if (via6) {  // the path works: answered the way the probe came
                    udp::Wire ack;
                    ack.v6 = true;
                    ack.addr6 = from.addr6;
                    ack.port = from.port;
                    ack.data.assign(udp::kProbeAck, udp::kProbeAck + sizeof(udp::kProbeAck));
                    wire_send(*port, ack);
                } else if (via_relay) {
                    port_send(*port, udp::kProbeAck, sizeof(udp::kProbeAck), &sa);
                } else {
                    raw_sendto(port->fd, udp::kProbeAck, sizeof(udp::kProbeAck), &sa);
                }
                std::lock_guard<std::mutex> lk(port->mu);
                ++port->rx;
                ++port->probes;
                note_heard_locked(*port, source_key(sa.sin_addr.s_addr, sa.sin_port));
                continue;
            }
            if (n >= 20 && buf[0] == 0x00 && buf[1] == 0x01) {
                {
                    std::lock_guard<std::mutex> lk(port->mu);
                    ++port->rx;
                    note_heard_locked(*port, source_key(sa.sin_addr.s_addr, sa.sin_port));
                }
                answer_stun(*port, buf, static_cast<std::size_t>(n), sa);
                continue;
            }
            if (n >= 20 && buf[0] == 0x01 && buf[1] == 0x01) {
                std::lock_guard<std::mutex> lk(port->mu);
                std::uint32_t a = 0;
                std::uint16_t p = 0;
                if (port->stun_pending && sa.sin_addr.s_addr == port->stun_server_addr &&
                    sa.sin_port == port->stun_server_port &&
                    net::stun::parse_binding_response(buf, static_cast<std::size_t>(n), port->stun_txid, &a, &p,
                                                      &port->stun_relay)) {
                    ++port->rx;
                    port->stun_addr = a;
                    port->stun_port = p;
                    port->stun_done = true;
                    port->stun_pending = false;
                    port->stun_cv.notify_all();
                    continue;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(port->mu);
            ++port->rx;
            note_heard_locked(*port, source_key(sa.sin_addr.s_addr, sa.sin_port));
            if (!hdr) ++port->no_header;
            auto it = hdr ? port->by_vport.find(dst) : port->by_vport.end();
            if (it != port->by_vport.end()) {
                q = it->second;
            } else if (!port->by_vport.empty()) {
                // No socket on that vport (or no header): the first bound vport takes it, as
                // the shadPS4 fork forwarded a PS4 peer's vport 30 to the game's 40.
                q = port->by_vport.begin()->second;
                forwarded = true;
                if (hdr) ++port->unmatched;
            }
            const auto now = Clock::now();
            if (now - port->last_report > std::chrono::seconds(60)) {
                port->last_report = now;
                p2p_report_locked(*port, "60 s");
            }
        }
        if (!q) continue;
        alias_in(sa);  // the game knows the peer by its primary
        Datagram dg;
        dg.data.assign(buf + hdr, buf + n);
        dg.addr = sa.sin_addr.s_addr;
        dg.port = sa.sin_port;
        dg.vport = src;
        if (PacketTraceFn tf = g_packet_trace.load(std::memory_order_relaxed))
            tf(false, src, dst, dg.data.data(), dg.data.size());
        hex_trace(false, src, dst, dg.data.data(), dg.data.size());
        if (g_rxgap_log) {
            nr::RxGapMonitor::Gap gap;
            if (rxgap.heard(source_key(dg.addr, dg.port), Clock::now(), &gap)) {
                char ip[32];
                write_ipv4(ip, sizeof(ip), dg.addr);
                std::size_t inbox = 0;
                {
                    std::lock_guard<std::mutex> lk(q->mu);
                    inbox = q->q.size();
                }
                const std::int64_t gr = g_game_read_ns.load(std::memory_order_relaxed);
                char read[48];
                if (gr) std::snprintf(read, sizeof(read), "%lld ms ago", static_cast<long long>((steady_ns() - gr) / 1000000));
                else std::snprintf(read, sizeof(read), "never");
                log("rx gap: %s:%u was silent %.1f s; %llu datagram(s) from the other %d source(s) meanwhile, %d of them "
                    "silent now too; this reader's worst stall in that time %.0f ms%s%s; the game took from the inbox %s "
                    "(%zu queued)",
                    ip, bswap16(dg.port), gap.silent_s, static_cast<unsigned long long>(gap.others), gap.peers - 1,
                    gap.silent_peers, gap.worst_stall_ms, gap.worst_stall_ms > 0 ? " in its " : "", gap.stall_kind, read,
                    inbox);
            }
        }
        if (trace_now(logs)) {
            char ip[32];
            write_ipv4(ip, sizeof(ip), sa.sin_addr.s_addr);
            log("recv %d <- %s:%u vport %u->%u: %zu bytes%s", q->sock, ip, bswap16(sa.sin_port), src, dst,
                dg.data.size(), hdr ? (dst == q->vport ? "" : " (forwarded)") : " (no p2p header)");
        }
        {
            std::lock_guard<std::mutex> lk(q->mu);
            ++q->in_dgrams;
            q->in_bytes += dg.data.size();
            const std::uint64_t key = source_key(dg.addr, dg.port);
            const auto now = Clock::now();
            Clock::time_point prev{};
            if (auto h = q->heard.find(key); h != q->heard.end()) prev = h->second;
            if (q->heard.size() < kMaxHeard || q->heard.count(key)) q->heard[key] = now;
            if (std::vector<std::uint8_t> ka = nr::keepalive_from(dg.data.data(), dg.data.size()); !ka.empty()) {
                if (q->keepalive.size() < kMaxHeard || q->keepalive.count(key)) {
                    Datagram k = dg;
                    k.data = std::move(ka);
                    q->keepalive[key] = std::move(k);
                }
            }
            if (auto r = q->replay.find(key); r != q->replay.end() && g_outage_replay) {
                std::size_t records = 0;
                auto dgs = r->second.on_heard(now, prev, &records);
                auto to = q->replay_to.find(key);
                if (!dgs.empty() && to != q->replay_to.end()) {
                    // A guest-to-guest pair that goes silent again and again moves to the host relay.
                    if (!settings().host) {
                        std::string why;
                        paths().note_gap(dg.addr, dg.port, now, &why);
                        if (!why.empty()) log("%s", why.c_str());
                    }
                    {
                        std::function<void(std::uint32_t, std::uint16_t, double)> hook;
                        {
                            std::lock_guard<std::mutex> hl(g_gap_hook_mu);
                            hook = g_gap_hook;
                        }
                        if (hook) hook(dg.addr, dg.port, std::chrono::duration<double>(now - prev).count());
                    }
                    resume_replay(q->sock, to->second, std::move(dgs), records,
                                  std::chrono::duration<double>(now - prev).count());
                    q->strip_until[key] = now + std::chrono::seconds(3);
                }
            }
            if (auto su = q->strip_until.find(key); su != q->strip_until.end()) {
                if (now >= su->second) {
                    log("outage resume: %llu duplicate-ack report(s) from that peer held back meanwhile",
                        static_cast<unsigned long long>(q->stripped));
                    q->strip_until.erase(su);
                    q->stripped = 0;
                } else {
                    std::vector<std::uint8_t> kept;
                    if (const std::size_t gone = nr::strip_dup_acks(dg.data.data(), dg.data.size(), &kept)) {
                        q->stripped += gone;
                        dg.data = std::move(kept);
                    }
                }
            }
            if (dg.data.empty()) continue;  // only held-back records in it
            if (dg.data.size() <= kSmallDgram &&
                (q->last_small.size() < kMaxHeard || q->last_small.count(source_key(dg.addr, dg.port))))
                q->last_small[source_key(dg.addr, dg.port)] = dg;
            if (forwarded) ++q->forwarded;
            if (q->q.size() >= kMaxInbox) {  // the game is not reading (or a flood): oldest go
                q->q.pop_front();
                const std::uint64_t d = port->dropped.fetch_add(1) + 1;
                if (d % 1000 == 1)
                    log("p2p port %u: a full inbox dropped datagrams (%llu so far)", port->port,
                        static_cast<unsigned long long>(d));
            }
            q->q.push_back(std::move(dg));
            if (q->q.size() > q->hiwater) q->hiwater = q->q.size();
        }
        q->cv.notify_all();
        net_wake();
    }
}

// F2 (0.7.1): the IPv6 socket on the party port's number ([::]:port, IPv6 only; [::1] when the
// IPv4 side is loopback only). Without it the port is IPv4 only, as before.
void open_v6(P2pPort& port) {
    if (sock_ok(port.fd6.load()) || !port.port) return;
#if defined(_WIN32)
    const sockfd_t s = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
#else
    const sockfd_t s = ::socket(AF_INET6, SOCK_DGRAM, 0);
#endif
    if (!sock_ok(s)) {
        log("party port %u: no IPv6 socket (errno %d): IPv4 only", port.port, sock_errno());
        return;
    }
    sock_setopt_int(s, IPPROTO_IPV6, IPV6_V6ONLY, 1);
#if defined(_WIN32)
    sock_setopt_int(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
    BOOL no_reset = FALSE;
    DWORD ret = 0;
    ::WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &no_reset, sizeof(no_reset), nullptr, 0, &ret,
               nullptr, nullptr);
#endif
    sock_set_nonblock(s, true);
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_port = bswap16(port.port);
    const bool lb = forced_local_ipv4() == bswap32(0x7f000001u);
    if (lb) a.sin6_addr = in6addr_loopback;
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        log("party port %u: IPv6 bind failed (errno %d): IPv4 only", port.port, sock_errno());
        sock_close(s);
        return;
    }
    port.fd6.store(s);
    log("party port %u: IPv6 too (%s)", port.port, lb ? "[::1], loopback test" : "[::]");
}

// Under g_net_mu. The instance's party UDP port, opened on first use (the game's first P2P
// bind, or the session layer's STUN query before it).
std::shared_ptr<P2pPort> p2p_port_open(std::uint16_t want) {
    wsa_start();
    auto pit = g_p2p_ports.find(want);
    if (pit != g_p2p_ports.end()) return pit->second;
    const std::uint16_t bound = g_bound_port.load();
    if (bound) {
        pit = g_p2p_ports.find(bound);
        if (pit != g_p2p_ports.end()) return pit->second;
    }
    auto port = std::make_shared<P2pPort>();
    // The port is this instance's alone. With SO_REUSEADDR a second instance on the machine
    // bound it too and the kernel handed it our peers' datagrams (bbhost, 2026-10-06);
    // Windows also lets a later SO_REUSEADDR socket take a port unless the first is
    // exclusive. When another program has it, the next free port is ours.
    constexpr int kTries = 16;
    for (int i = 0; i < kTries; ++i) {
        const auto at = static_cast<std::uint16_t>(want + i);
        port->fd = sock_new(SOCK_DGRAM);
        if (!sock_ok(port->fd)) return nullptr;
#if defined(_WIN32)
        sock_setopt_int(port->fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
        // An ICMP port-unreachable from a peer that went away must not fail later reads.
        BOOL no_reset = FALSE;
        DWORD ret = 0;
        ::WSAIoctl(port->fd, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &no_reset, sizeof(no_reset), nullptr,
                   0, &ret, nullptr, nullptr);
#endif
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = bswap16(at);
        // Local tests bind loopback only (no Windows Firewall prompt): see party_sock.h.
        sa.sin_addr.s_addr = forced_local_ipv4() == bswap32(0x7f000001u) ? bswap32(0x7f000001u) : INADDR_ANY;
        if (::bind(port->fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0) {
            port->port = at;
            break;
        }
        const int e = sock_errno();
        sock_close(port->fd);
        port->fd = kBadSock;
        if (e != bsd::kEADDRINUSE && e != bsd::kEACCES) {
            log("party port %u: bind failed (errno %d)", at, e);
            return nullptr;
        }
        log("party port %u is in use by another program%s", at, i + 1 < kTries ? "; trying the next" : "");
    }
    if (!sock_ok(port->fd)) return nullptr;
    if (port->port != want) log("party port %u (BB_PARTY_PORT %u was taken); peers reach this game there", port->port, want);
    else log("party port %u: listening on this PC (whether the internet reaches it: the Party NAT line)", port->port);
    g_bound_port.store(port->port);
    if (g_ipv6_wanted.load()) open_v6(*port);
    port->reader = std::thread(p2p_reader, port);
    g_p2p_ports[port->port] = port;
    return port;
}

// Under g_net_mu. Detaches a P2P socket from its port; the port's reader stops with its last
// socket (unless the session layer holds the port open).
std::atomic<bool> g_port_pinned{false};
void p2p_detach(NetSock& sock) {
    if (!sock.port) return;
    std::shared_ptr<P2pPort> port = sock.port;
    bool empty = false;
    {
        std::lock_guard<std::mutex> lk(port->mu);
        p2p_report_locked(*port, "at a socket's close");
        auto it = port->by_vport.find(sock.vport);
        if (it != port->by_vport.end() && it->second == sock.queue) port->by_vport.erase(it);
        empty = port->by_vport.empty();
    }
    sock.port.reset();
    if (empty && !g_port_pinned.load()) {
        port->stop.store(true, std::memory_order_relaxed);
        if (port->reader.joinable()) port->reader.join();
        if (sock_ok(port->fd)) sock_close(port->fd);
        port->fd = kBadSock;
        if (const sockfd_t f6 = port->fd6.exchange(kBadSock); sock_ok(f6)) sock_close(f6);
        g_p2p_ports.erase(port->port);
        g_bound_port.store(0);
    }
}

// A copy of the socket's runtime state, without holding g_net_mu across a blocking call.
bool sock_state(int s, NetSock* out) {
    std::lock_guard<std::mutex> lock(g_net_mu);
    auto it = g_net_socks.find(s);
    if (it == g_net_socks.end()) return false;
    *out = it->second;
    return true;
}

// Waits (blocking socket) until fd is ready for `events`; returns 0, or the game's error
// (EINTR when aborted, EAGAIN at the socket's timeout).
int wait_ready(const NetSock& sock, short events, int timeout_us) {
    const auto deadline = timeout_us > 0 ? Clock::now() + std::chrono::microseconds(timeout_us) : Clock::time_point::max();
    for (;;) {
        if (sock.ctl->aborted.load()) return net_err(bsd::kEINTR);
        const int r = sock_poll(sock.fd, events, 50);
        if (r > 0) return 0;
        if (r < 0) return net_fail();
        if (Clock::now() >= deadline) return net_again();
    }
}

void fill_sce_addr(SceSockaddrIn* out, std::uint32_t addr, std::uint16_t port_nbo, std::uint16_t vport_host) {
    std::memset(out, 0, sizeof(*out));
    out->len = 16;
    out->family = 2;
    out->port = port_nbo;
    out->addr = addr;
    out->vport = bswap16(vport_host);
}

// Reads one datagram (or stream bytes): from the socket's queue for P2P, the fd otherwise.
int sock_recv(int s, void* buf, std::uint64_t len, int flags, SceSockaddrIn* from) {
    NetSock sock;
    if (!sock_state(s, &sock)) return net_err(bsd::kEBADF);
    const bool dontwait = sock.nonblock || (flags & 0x80);  // 0x80: SCE_NET_MSG_DONTWAIT
    const bool peek = flags & 0x2;
    if (sock.p2p) {
        if (!sock.queue) return net_again();
        if (P2pRecvHookFn h = g_p2p_recv_hook.load(std::memory_order_relaxed)) h(s, 0);
        SockQueue& q = *sock.queue;
        Datagram dg;
        {
            std::unique_lock<std::mutex> lk(q.mu);
            if (q.q.empty()) {
                if (dontwait) return net_again();
                const int to = sock.ctl->rcvtimeo_us.load();
                auto ready = [&] { return !q.q.empty() || sock.ctl->aborted.load(); };
                if (to > 0) {
                    if (!q.cv.wait_for(lk, std::chrono::microseconds(to), ready)) return net_again();
                } else {
                    q.cv.wait(lk, ready);
                }
                if (q.q.empty()) return net_err(bsd::kEINTR);
            }
            if (g_rxgap_log) g_game_read_ns.store(steady_ns(), std::memory_order_relaxed);
            if (peek) {
                dg = q.q.front();
            } else {
                dg = std::move(q.q.front());
                q.q.pop_front();
            }
        }
        const std::size_t take = dg.data.size() < len ? dg.data.size() : static_cast<std::size_t>(len);
        if (take < dg.data.size()) {
            static std::atomic<int> cut{0};
            if (cut.fetch_add(1) < 20)
                log("recv: a %zu-byte datagram cut to the game's %llu-byte buffer", dg.data.size(),
                    static_cast<unsigned long long>(len));
        }
        if (buf && take) std::memcpy(buf, dg.data.data(), take);
        if (from) fill_sce_addr(from, dg.addr, dg.port, dg.vport);
        return static_cast<int>(take);
    }
    if (!sock_ok(sock.fd)) return dontwait ? net_again() : net_err(bsd::kENOTCONN);
    for (;;) {
        sockaddr_in sa{};
        socklen_type sl = sizeof(sa);
        const ssize_type n = raw_recvfrom(sock.fd, buf, static_cast<std::size_t>(len), peek ? MSG_PEEK : 0, &sa, &sl);
        if (n >= 0) {
            if (from) fill_sce_addr(from, sa.sin_addr.s_addr, sa.sin_port, 0);
            return static_cast<int>(n);
        }
#if defined(_WIN32)
        if (sock.type == 2 && ::WSAGetLastError() == WSAECONNRESET) continue;  // a stale ICMP report
#endif
        if (!would_block()) return net_fail();
        if (dontwait) return net_again();
        const int r = wait_ready(sock, POLLIN, sock.ctl->rcvtimeo_us.load());
        if (r) return r;
    }
}

// Sends one datagram, with the P2P header in front on a P2P socket (and the relay frame
// around it toward a relayed peer).
int dgram_send(int s, const void* buf, std::uint64_t len, int flags, const SceSockaddrIn* to) {
    NetSock sock;
    if (!sock_state(s, &sock)) return net_err(bsd::kEBADF);
    // F5: the session transport, catching up after an outage, packs datagrams past its own MTU
    // (1232; live up to 2042 bytes) and the peer's game reads at most its MTU: the records past it
    // were cut off and their gap stalled the stream. Such a datagram leaves as several, split at
    // record boundaries (each record stands alone).
    if (sock.p2p && buf && len > nr::kGameMtu && g_outage_replay) {
        std::vector<std::vector<std::uint8_t>> parts;
        if (nr::split_records(static_cast<const std::uint8_t*>(buf), static_cast<std::size_t>(len), nr::kGameMtu,
                              &parts) &&
            parts.size() > 1) {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 10)
                log("send: a %llu-byte game datagram split into %zu at record boundaries",
                    static_cast<unsigned long long>(len), parts.size());
            for (const auto& part : parts) {
                const int r = dgram_send(s, part.data(), part.size(), flags, to);
                if (r < 0) return r;
            }
            return static_cast<int>(len);
        }
    }
    const sockfd_t fd = sock.port ? sock.port->fd : sock.fd;
    if (!sock_ok(fd)) return static_cast<int>(len);
    if (!to) return net_err(bsd::kEDESTADDRREQ);
    if (sock.p2p)
        if (P2pRecvHookFn h = g_p2p_recv_hook.load(std::memory_order_relaxed)) h(s, 2);
    std::uint8_t pkt[udp::kMaxDatagram + kRelayHeader];
    std::uint8_t* body = pkt + kRelayHeader;
    std::size_t hdr = 0;
    const std::uint16_t dst_vport = bswap16(to->vport);
    if (sock.p2p) hdr = p2p_write_header(body, sock.vport, dst_vport);
    if (hdr + len > udp::kMaxDatagram) return net_err(bsd::kEMSGSIZE);
    if (buf && len) std::memcpy(body + hdr, buf, static_cast<std::size_t>(len));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = to->port;
    sa.sin_addr.s_addr = to->addr;
    (void)flags;
    ssize_type n;
    std::uint32_t stun_addr = 0;
    std::uint16_t stun_port = 0;
    // A guest whose direct path does not answer goes through its relay port on the host.
    std::uint32_t via_addr = to->addr;
    std::uint16_t via_port = to->port;
    std::uint16_t relay_port = 0;
    if (sock.p2p && paths().relayed(to->addr, to->port, &relay_port)) {
        std::uint32_t server = 0;
        if (g_relay.get(&server, nullptr)) {
            via_addr = server;
            via_port = bswap16(relay_port);
        }
    }
    if (sock.p2p && g_relay.frame_for(via_addr, via_port, pkt, &stun_addr, &stun_port)) {
        sockaddr_in stun_sa{};
        stun_sa.sin_family = AF_INET;
        stun_sa.sin_addr.s_addr = stun_addr;
        stun_sa.sin_port = stun_port;
        n = sock.port ? port_send(*sock.port, pkt, kRelayHeader + hdr + static_cast<std::size_t>(len), &stun_sa)
                      : raw_sendto(fd, pkt, kRelayHeader + hdr + static_cast<std::size_t>(len), &stun_sa);
        if (n >= 0) n -= static_cast<ssize_type>(kRelayHeader);
    } else {
        if (sock.p2p) alias_out(sa);  // a peer heard on its LAN address
        n = sock.p2p && sock.port ? port_send(*sock.port, body, hdr + static_cast<std::size_t>(len), &sa)
                                  : raw_sendto(fd, body, hdr + static_cast<std::size_t>(len), &sa);
    }
    if (PacketTraceFn tf = g_packet_trace.load(std::memory_order_relaxed); tf && sock.p2p && buf)
        tf(true, sock.vport, dst_vport, static_cast<const unsigned char*>(buf), static_cast<std::size_t>(len));
    if (sock.p2p) hex_trace(true, sock.vport, dst_vport, static_cast<const std::uint8_t*>(buf), static_cast<std::size_t>(len));
    if (sock.p2p && sock.queue && buf && g_outage_replay) {
        const std::uint64_t key = source_key(to->addr, to->port);
        std::lock_guard<std::mutex> lk(sock.queue->mu);
        auto h = sock.queue->heard.find(key);
        // Only peers the game has heard (bounded by the heard table): never a stranger's address.
        if (h != sock.queue->heard.end()) {
            sock.queue->replay[key].on_send(Clock::now(), h->second, static_cast<const std::uint8_t*>(buf),
                                            static_cast<std::size_t>(len));
            sock.queue->replay_to[key] = *to;
        }
    }
    static std::atomic<int> logs{0};
    if (sock.p2p && trace_now(logs)) {
        char ip[32];
        write_ipv4(ip, sizeof(ip), to->addr);
        log("send %d -> %s:%u vport %u->%u: %llu bytes -> %lld", s, ip, bswap16(to->port), sock.vport, dst_vport,
            static_cast<unsigned long long>(len), static_cast<long long>(n));
    }
    if (n < 0) {
        // A UDP send never waits here: the datagram is dropped as a full queue would.
        if (would_block()) return sock.nonblock || (flags & 0x80) ? net_again() : static_cast<int>(len);
        return net_fail();
    }
    if (sock.queue) {
        std::lock_guard<std::mutex> lk(sock.queue->mu);
        ++sock.queue->out_dgrams;
        sock.queue->out_bytes += len;
    }
    return static_cast<int>(len);
}

// Maps a guest port to the host port: the game's P2P port 3658 becomes the party port.
std::uint16_t host_port_for(std::uint16_t guest_port) {
    return guest_port == kGameP2pPort ? settings().party_port : guest_port;
}

// ---- libSceNet -------------------------------------------------------------------------------

BBNET_ABI int net_init() {
    BBNET_GUEST_RETURN();
    wsa_start();
    log("sceNetInit (party networking, port %u)", settings().party_port);
    return 0;
}
BBNET_ABI int net_term() { return 0; }
BBNET_ABI int* net_errno_loc() { return &g_net_errno; }

BBNET_ABI int net_pool_create(const char* name, int size, int) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_net_mu);
    const int id = net_alloc();
    log("sceNetPoolCreate %s size=%d -> %d", name ? name : "", size, id);
    return id;
}
BBNET_ABI int net_pool_destroy(int) { return 0; }

BBNET_ABI int net_socket(const char* name, int domain, int type, int) {
    BBNET_GUEST_RETURN();
    wsa_start();
    std::lock_guard<std::mutex> lock(g_net_mu);
    if (domain != 2) return net_err(bsd::kEAFNOSUPPORT);
    const int id = net_alloc();
    NetSock sock;
    sock.domain = domain;
    sock.type = type;
    const char* kind = "inert";
    if (type == 2) {
        sock.fd = sock_new(SOCK_DGRAM);
        if (!sock_ok(sock.fd)) return net_fail();
        kind = "udp";
    } else if (type == 1) {
        sock.fd = sock_new(SOCK_STREAM);
        if (!sock_ok(sock.fd)) return net_fail();
        kind = "tcp";
    } else if (type == 6) {
        sock.p2p = true;
        sock.queue = std::make_shared<SockQueue>();
        sock.queue->sock = id;
        kind = "udp p2p";
    }
    g_net_socks[id] = sock;
    log("sceNetSocket %s domain=%d type=%d -> %d (%s)", name ? name : "", domain, type, id, kind);
    return id;
}
BBNET_ABI int net_socket_close(int s) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_net_mu);
    auto it = g_net_socks.find(s);
    if (it == g_net_socks.end()) return net_err(bsd::kEBADF);
    it->second.ctl->aborted.store(true);
    if (it->second.queue) it->second.queue->cv.notify_all();
    if (sock_ok(it->second.fd)) sock_close(it->second.fd);
    p2p_detach(it->second);
    for (auto& [eid, ep] : g_net_epolls) {
        ep->watch.erase(s);
        ep->data.erase(s);
    }
    g_net_socks.erase(it);
    net_wake();
    return 0;
}
BBNET_ABI int net_socket_abort(int s, int) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_net_mu);
    auto it = g_net_socks.find(s);
    if (it == g_net_socks.end()) return net_err(bsd::kEBADF);
    it->second.ctl->aborted.store(true);
    if (it->second.queue) it->second.queue->cv.notify_all();
    net_wake();
    return 0;
}
BBNET_ABI int net_bind(int s, const void* addr, int len) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_net_mu);
    auto it = g_net_socks.find(s);
    if (it == g_net_socks.end()) return net_err(bsd::kEBADF);
    NetSock& sock = it->second;
    SceSockaddrIn in{};
    if (addr && len >= static_cast<int>(sizeof(in))) std::memcpy(&in, addr, sizeof(in));
    if (sock.p2p) {
        // A P2P socket binds the instance's party port, not the game's 3658: two instances
        // may share this machine. The vport is the game's.
        const std::uint16_t want = settings().party_port;
        std::shared_ptr<P2pPort> port = p2p_port_open(want);
        if (!port) {
            const int r = net_fail();
            log("sceNetBind %d port %u vport %u -> udp %u failed", s, bswap16(in.port), bswap16(in.vport), want);
            return r;
        }
        p2p_detach(sock);
        sock.vport = bswap16(in.vport);
        sock.queue->vport = sock.vport;
        sock.port = port;
        {
            std::lock_guard<std::mutex> lk(port->mu);
            port->by_vport[sock.vport] = sock.queue;
        }
        log("sceNetBind %d port %u vport %u -> udp %u (p2p)", s, bswap16(in.port), sock.vport, port->port);
        return 0;
    }
    if (!sock_ok(sock.fd)) return 0;
    const std::uint16_t guest_port = bswap16(in.port);
    const std::uint16_t host_port = host_port_for(guest_port);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = bswap16(host_port);
    sa.sin_addr.s_addr = in.addr;
    // Local tests: the game's own sockets listen on loopback only too (no Windows Firewall prompt).
    if (in.addr == 0 && forced_local_ipv4() == bswap32(0x7f000001u)) sa.sin_addr.s_addr = bswap32(0x7f000001u);
#if defined(_WIN32)
    if (host_port != guest_port) sock_setopt_int(sock.fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#endif
    if (::bind(sock.fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        const int r = net_fail();
        log("sceNetBind %d port %u failed (errno %d)", s, host_port, g_net_errno);
        return r;
    }
    log("sceNetBind %d port %u%s (%s)", s, host_port, host_port != guest_port ? " (game 3658)" : "",
        sock.type == 1 ? "tcp" : "udp");
    return 0;
}
BBNET_ABI int net_listen(int s, int backlog) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_net_mu);
    auto it = g_net_socks.find(s);
    if (it == g_net_socks.end()) return net_err(bsd::kEBADF);
    it->second.listening = true;
    if (sock_ok(it->second.fd) && it->second.type == 1) {
        if (::listen(it->second.fd, backlog > 0 ? backlog : 8) < 0) return net_fail();
    }
    return 0;
}
BBNET_ABI int net_accept(int s, void* addr, unsigned* addrlen) {
    BBNET_GUEST_RETURN();
    NetSock sock;
    if (!sock_state(s, &sock)) return net_err(bsd::kEBADF);
    if (!sock_ok(sock.fd) || sock.type != 1) return net_err(bsd::kEOPNOTSUPP);
    for (;;) {
        sockaddr_in sa{};
        socklen_type sl = sizeof(sa);
        const sockfd_t fd = ::accept(sock.fd, reinterpret_cast<sockaddr*>(&sa), &sl);
        if (sock_ok(fd)) {
            sock_set_nonblock(fd, true);
            std::lock_guard<std::mutex> lock(g_net_mu);
            const int id = net_alloc();
            NetSock ns;
            ns.domain = 2;
            ns.type = 1;
            ns.fd = fd;
            g_net_socks[id] = ns;
            if (addr && addrlen && *addrlen >= 16) {
                SceSockaddrIn in;
                fill_sce_addr(&in, sa.sin_addr.s_addr, sa.sin_port, 0);
                std::memcpy(addr, &in, 16);
                *addrlen = 16;
            }
            return id;
        }
        if (!would_block()) return net_fail();
        if (sock.nonblock) return net_again();
        const int r = wait_ready(sock, POLLIN, 0);
        if (r) return r;
    }
}
BBNET_ABI int net_connect(int s, const void* addr, int len) {
    BBNET_GUEST_RETURN();
    NetSock sock;
    if (!sock_state(s, &sock)) return net_err(bsd::kEBADF);
    if (!addr || len < 16) return net_err(bsd::kEINVAL);
    if (!sock_ok(sock.fd)) return 0;  // P2P / inert: addressed per datagram
    SceSockaddrIn in{};
    std::memcpy(&in, addr, sizeof(in));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = in.port;
    sa.sin_addr.s_addr = in.addr;
    if (::connect(sock.fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0) return 0;
#if defined(_WIN32)
    const bool pending = ::WSAGetLastError() == WSAEWOULDBLOCK || ::WSAGetLastError() == WSAEINPROGRESS;
#else
    const bool pending = errno == EINPROGRESS;
#endif
    if (!pending) return net_fail();
    if (sock.nonblock) return net_err(bsd::kEINPROGRESS);
    const int sndto = sock.ctl->sndtimeo_us.load();
    const int r = wait_ready(sock, POLLOUT, sndto > 0 ? sndto : 30 * 1000 * 1000);
    if (r) return r == net_again() ? net_err(bsd::kETIMEDOUT) : r;
    int err = 0;
    socklen_type el = sizeof(err);
    ::getsockopt(sock.fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el);
    if (err) return net_err(map_so_error(err));
    return 0;
}
BBNET_ABI int net_shutdown(int s, int how) {
    BBNET_GUEST_RETURN();
    NetSock sock;
    if (!sock_state(s, &sock)) return net_err(bsd::kEBADF);
    if (sock_ok(sock.fd) && sock.type == 1 && ::shutdown(sock.fd, how) < 0) return net_fail();
    return 0;
}
BBNET_ABI int net_setsockopt(int s, int level, int name, const void* val, int len) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_net_mu);
    auto it = g_net_socks.find(s);
    if (it == g_net_socks.end()) return net_err(bsd::kEBADF);
    NetSock& sock = it->second;
    int v = 0;
    if (val && len >= 4) std::memcpy(&v, val, 4);
    if (level == 0xffff) {
        switch (name) {
            case 0x1200: sock.nonblock = v != 0; break;  // SCE_NET_SO_NBIO (host sockets stay non-blocking)
            case 0x1005: sock.ctl->sndtimeo_us.store(v); break;  // SO_SNDTIMEO (microseconds)
            case 0x1006: sock.ctl->rcvtimeo_us.store(v); break;  // SO_RCVTIMEO
            case 0x1001:
            case 0x1002:
                if (sock_ok(sock.fd)) sock_setopt_int(sock.fd, SOL_SOCKET, name == 0x1001 ? SO_SNDBUF : SO_RCVBUF, v);
                break;
            case 0x0004:  // SO_REUSEADDR: not on Windows, where it lets another program take the port
#if !defined(_WIN32)
                if (sock_ok(sock.fd)) sock_setopt_int(sock.fd, SOL_SOCKET, SO_REUSEADDR, v);
#endif
                break;
            case 0x0020:  // SO_BROADCAST
                if (sock_ok(sock.fd)) sock_setopt_int(sock.fd, SOL_SOCKET, SO_BROADCAST, v);
                break;
            default: break;
        }
    } else if (level == 6 && name == 1 && sock_ok(sock.fd) && sock.type == 1) {  // TCP_NODELAY
        sock_setopt_int(sock.fd, IPPROTO_TCP, TCP_NODELAY, v);
    }
    if (g_net_trace) log("sceNetSetsockopt %d level 0x%x name 0x%x = %d", s, level, name, v);
    return 0;
}
BBNET_ABI int net_getsockopt(int s, int level, int name, void* val, unsigned* len) {
    BBNET_GUEST_RETURN();
    NetSock sock;
    if (!sock_state(s, &sock)) return net_err(bsd::kEBADF);
    if (!val || !len || *len < 4) return net_err(bsd::kEINVAL);
    int v = 0;
    if (level == 0xffff) {
        switch (name) {
            case 0x1200: v = sock.nonblock; break;
            case 0x1005: v = sock.ctl->sndtimeo_us.load(); break;
            case 0x1006: v = sock.ctl->rcvtimeo_us.load(); break;
            case 0x1008: v = sock.type; break;  // SO_TYPE
            case 0x1007:                        // SO_ERROR
                if (sock_ok(sock.fd)) {
                    int err = 0;
                    socklen_type el = sizeof(err);
                    ::getsockopt(sock.fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el);
                    v = err ? map_so_error(err) : 0;
                }
                break;
            case 0x1001:
            case 0x1002:
                if (sock_ok(sock.fd)) {
                    socklen_type vl = sizeof(v);
                    ::getsockopt(sock.fd, SOL_SOCKET, name == 0x1001 ? SO_SNDBUF : SO_RCVBUF, reinterpret_cast<char*>(&v),
                                 &vl);
                }
                break;
            default: break;
        }
    }
    std::memcpy(val, &v, 4);
    *len = 4;
    return 0;
}
BBNET_ABI int net_getsockname(int s, void* addr, unsigned* addrlen) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_net_mu);
    auto it = g_net_socks.find(s);
    if (it == g_net_socks.end()) return net_err(bsd::kEBADF);
    if (addr && addrlen && *addrlen >= 16) {
        SceSockaddrIn in{};
        in.len = 16;
        in.family = 2;
        const sockfd_t fd = it->second.port ? it->second.port->fd : it->second.fd;
        if (sock_ok(fd)) {
            sockaddr_in sa{};
            socklen_type sl = sizeof(sa);
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &sl) == 0) {
                in.port = sa.sin_port;
                in.addr = sa.sin_addr.s_addr;
            }
            in.vport = bswap16(it->second.vport);
        }
        std::memcpy(addr, &in, 16);
        *addrlen = 16;
    }
    return 0;
}

BBNET_ABI int net_recv(int s, void* buf, std::uint64_t len, int flags) {
    BBNET_GUEST_RETURN();
    return sock_recv(s, buf, len, flags, nullptr);
}
BBNET_ABI int net_send(int s, const void* buf, std::uint64_t len, int flags) {
    BBNET_GUEST_RETURN();
    NetSock sock;
    if (!sock_state(s, &sock)) return net_err(bsd::kEBADF);
    if (!sock_ok(sock.fd)) return static_cast<int>(len);
    const bool dontwait = sock.nonblock || (flags & 0x80);
    std::uint64_t done = 0;
    for (;;) {
#if defined(MSG_NOSIGNAL)
        constexpr int kSendFlags = MSG_NOSIGNAL;
#else
        constexpr int kSendFlags = 0;
#endif
        const int n = static_cast<int>(
            ::send(sock.fd, static_cast<const char*>(buf) + done, static_cast<int>(len - done), kSendFlags));
        if (n >= 0) {
            done += static_cast<std::uint64_t>(n);
            if (done >= len || sock.type != 1 || dontwait) return static_cast<int>(done);
            continue;
        }
        if (!would_block()) return done ? static_cast<int>(done) : net_fail();
        if (dontwait) return done ? static_cast<int>(done) : net_again();
        const int r = wait_ready(sock, POLLOUT, sock.ctl->sndtimeo_us.load());
        if (r) return done ? static_cast<int>(done) : r;
    }
}
BBNET_ABI int net_recvfrom(int s, void* buf, std::uint64_t len, int flags, void* from, unsigned* fromlen) {
    BBNET_GUEST_RETURN();
    SceSockaddrIn in{};
    const int r = sock_recv(s, buf, len, flags, &in);
    if (r >= 0 && from && fromlen && *fromlen >= 16) {
        std::memcpy(from, &in, 16);
        *fromlen = 16;
    }
    return r;
}
BBNET_ABI int net_sendto(int s, const void* buf, std::uint64_t len, int flags, const void* to, int tolen) {
    BBNET_GUEST_RETURN();
    if (!to || tolen < 16) return net_send(s, buf, len, flags);
    SceSockaddrIn in{};
    std::memcpy(&in, to, 16);
    return dgram_send(s, buf, len, flags, &in);
}

BBNET_ABI std::uint16_t net_htons(std::uint16_t v) { return bswap16(v); }
BBNET_ABI std::uint32_t net_htonl(std::uint32_t v) { return bswap32(v); }

BBNET_ABI int net_inet_pton(int af, const char* src, void* dst) {
    BBNET_GUEST_RETURN();
    if (af != 2 || !dst) return net_err(bsd::kEAFNOSUPPORT);
    std::uint32_t net = 0;
    if (!parse_ipv4(src, &net)) return 0;
    std::memcpy(dst, &net, 4);
    return 1;
}
BBNET_ABI const char* net_inet_ntop(int af, const void* src, char* dst, unsigned size) {
    BBNET_GUEST_RETURN();
    if (af != 2 || !src || !dst || size < 8) {
        net_err(af != 2 ? bsd::kEAFNOSUPPORT : bsd::kEINVAL);
        return nullptr;
    }
    std::uint32_t net = 0;
    std::memcpy(&net, src, 4);
    write_ipv4(dst, size, net);
    return dst;
}

BBNET_ABI int net_epoll_create(const char* name, int) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_net_mu);
    const int id = net_alloc();
    auto ep = std::make_shared<NetEpoll>();
    if (name) ep->name = name;
    g_net_epolls[id] = std::move(ep);
    log("sceNetEpollCreate %s -> %d", name ? name : "", id);
    return id;
}
BBNET_ABI int net_epoll_destroy(int id) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_net_mu);
    auto it = g_net_epolls.find(id);
    if (it == g_net_epolls.end()) return net_err(bsd::kEBADF);
    it->second->aborting.store(true);
    g_net_epolls.erase(it);
    net_wake();
    return 0;
}
// The game's SceNetEpollEvent: {u32 events, u32 pad, u64 ident, u64 data}.
struct SceEpollEvent {
    std::uint32_t events;
    std::uint32_t pad;
    std::uint64_t ident;
    std::uint64_t data;
};
BBNET_ABI int net_epoll_control(int id, int op, int sock, void* event) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_net_mu);
    auto it = g_net_epolls.find(id);
    if (it == g_net_epolls.end()) return net_err(bsd::kEBADF);
    NetEpoll& ep = *it->second;
    SceEpollEvent sev{};
    if (event) std::memcpy(&sev, event, sizeof(sev));
    if (op == 3) {  // DEL
        ep.watch.erase(sock);
        ep.data.erase(sock);
    } else {  // ADD (1) / MOD (2)
        ep.watch[sock] = sev.events;
        ep.data[sock] = sev.data;
    }
    net_wake();
    if (g_net_trace) log("sceNetEpollControl %d op %d sock %d events 0x%x", id, op, sock, sev.events);
    return 0;
}
BBNET_ABI int net_epoll_wait(int id, void* events, int maxevents, int timeout) {
    BBNET_GUEST_RETURN();
    if (P2pRecvHookFn h = g_p2p_recv_hook.load(std::memory_order_relaxed)) h(id, 1);
    std::shared_ptr<NetEpoll> ep;
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        auto it = g_net_epolls.find(id);
        if (it == g_net_epolls.end()) return net_err(bsd::kEBADF);
        ep = it->second;
    }
    // timeout is in microseconds, -1 forever.
    const auto deadline =
        timeout < 0 ? Clock::time_point::max() : Clock::now() + std::chrono::microseconds(timeout);
    for (;;) {
        // Before the look: a wake after it changes the word, and the sleep returns at once.
        const std::uint32_t gen = g_wake_word.load(std::memory_order_seq_cst);
        int out = 0;
        bool host_sockets = false;
        {
            std::lock_guard<std::mutex> lock(g_net_mu);
            if (!g_net_epolls.count(id)) return net_err(bsd::kEBADF);
            for (const auto& [sock, want] : ep->watch) {
                if (out >= maxevents) break;
                auto sit = g_net_socks.find(sock);
                if (sit == g_net_socks.end()) continue;
                const NetSock& ns = sit->second;
                std::uint32_t ready = 0;
                if (ns.p2p && ns.queue) {
                    std::lock_guard<std::mutex> lk(ns.queue->mu);
                    if ((want & 0x1) && !ns.queue->q.empty()) ready |= 0x1;
                    if (want & 0x2) ready |= 0x2;  // a datagram send never waits
                } else if (sock_ok(ns.fd)) {
                    host_sockets = true;
                    short ev = 0, rev = 0;
                    if (want & 0x1) ev |= POLLIN;
                    if (want & 0x2) ev |= POLLOUT;
                    if (sock_poll_revents(ns.fd, ev ? ev : POLLIN, &rev) > 0) {
                        if (rev & (POLLIN | POLLHUP)) ready |= want & 0x1;
                        if (rev & POLLOUT) ready |= want & 0x2;
                        if (rev & POLLERR) ready |= 0x8;
                        if (rev & POLLHUP) ready |= 0x10;
                    }
                }
                if (!ready) continue;
                SceEpollEvent sev{};
                sev.events = ready;
                sev.ident = static_cast<std::uint64_t>(sock);
                auto d = ep->data.find(sock);
                sev.data = d == ep->data.end() ? 0 : d->second;
                if (events) {
                    std::memcpy(static_cast<std::uint8_t*>(events) + static_cast<std::size_t>(out) * sizeof(sev), &sev,
                                sizeof(sev));
                }
                ++out;
            }
        }
        if (out) return out;
        if (ep->aborting.exchange(false)) return 0;
        const auto now = Clock::now();
        if (now >= deadline) return 0;
        // Host sockets have no reader to wake us: they are looked at every 10 ms.
        const auto slice = host_sockets ? std::chrono::milliseconds(10) : std::chrono::milliseconds(100);
        const Clock::time_point until = deadline - now < slice ? deadline : now + slice;
        wake_wait(gen, until);
    }
}
BBNET_ABI int net_epoll_abort(int id, int) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_net_mu);
    auto it = g_net_epolls.find(id);
    if (it == g_net_epolls.end()) return net_err(bsd::kEBADF);
    it->second->aborting.store(true);
    net_wake();
    return 0;
}

BBNET_ABI int net_resolver_create(const char*, int memid, int) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_net_mu);
    const int id = net_alloc();
    g_net_resolvers[id] = memid;
    return id;
}
BBNET_ABI int net_resolver_destroy(int id) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_net_mu);
    g_net_resolvers.erase(id);
    return 0;
}
// sceNetResolverStartNtoa(rid, hostname, SceNetInAddr* addr, timeout, retry, flags): a real lookup.
BBNET_ABI int net_resolver_ntoa(int id, const char* hostname, void* addr, int, int, int) {
    BBNET_GUEST_RETURN();
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        if (!g_net_resolvers.count(id)) return net_err(bsd::kEBADF);
    }
    if (!hostname || !addr) return net_err(bsd::kEINVAL);
    std::uint32_t a = 0;
    if (!parse_ipv4(hostname, &a)) {
        wsa_start();
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* res = nullptr;
        if (::getaddrinfo(hostname, nullptr, &hints, &res) != 0 || !res) {
            log("sceNetResolverStartNtoa %s: no such host", hostname);
            g_net_errno = kResolverNoHost & 0xff;
            return kResolverNoHost;
        }
        a = reinterpret_cast<const sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
        ::freeaddrinfo(res);
    }
    std::memcpy(addr, &a, 4);
    return 0;
}
// sceNetResolverStartAton(rid, const SceNetInAddr* addr, char* hostname, len, timeout, retry, flags).
BBNET_ABI int net_resolver_aton(int id, const void* addr, char* hostname, int len, int, int, int) {
    BBNET_GUEST_RETURN();
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        if (!g_net_resolvers.count(id)) return net_err(bsd::kEBADF);
    }
    if (!addr || !hostname || len <= 0) return net_err(bsd::kEINVAL);
    wsa_start();
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    std::memcpy(&sa.sin_addr.s_addr, addr, 4);
    char host[256];
    if (::getnameinfo(reinterpret_cast<const sockaddr*>(&sa), sizeof(sa), host, sizeof(host), nullptr, 0, 0) != 0) {
        write_ipv4(host, sizeof(host), sa.sin_addr.s_addr);
    }
    std::snprintf(hostname, static_cast<std::size_t>(len), "%s", host);
    return 0;
}

// ---- libSceNetCtl ------------------------------------------------------------------------------

namespace {
std::mutex g_vpn_mu;
std::string g_vpn_in_use;       // "label|ip"
void set_vpn_in_use(const std::string& v) {
    std::lock_guard<std::mutex> lk(g_vpn_mu);
    g_vpn_in_use = v;
}
}  // namespace

struct LocalNet {
    std::uint32_t ip = 0, mask = 0, gateway = 0, dns1 = 0, dns2 = 0;  // network order
    std::uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    std::uint32_t mtu = 1500;
};
// This machine's LAN address (the party's LAN code, sceNetCtl's IP_ADDRESS). Forced by
// BB_PARTY_LOCAL_IP (any address, loopback included; the adapter carrying it supplies mask and
// gateway) or BB_PARTY_LOOPBACK=1 / BB_MP_LOCAL_TEST=1 (127.0.0.1). Otherwise the best-scoring
// adapter (lan_adapter_score): a real adapter with the default route first, Hyper-V / WSL /
// VirtualBox / VPN adapters last. `how` describes the choice and the candidates (logged).
LocalNet query_local_net(std::string* how) {
    LocalNet out;
    std::string source;
    const std::uint32_t want = forced_local_ipv4(&source);
    const std::string pref = vpn_preference();
    std::string candidates;
    LanAdapter chosen{};
    char text[32];
    auto note = [&](const LanAdapter& info, int score) {
        if (want) return;
        write_ipv4(text, sizeof(text), info.ip);
        candidates += std::string(candidates.empty() ? "" : "; ") + text + " " + info.name +
                      (info.vpn_offline ? " (offline VPN, skipped)"
                       : score < 0 ? " (unusable)" : " (score " + std::to_string(score) + ")");
    };
#if defined(_WIN32)
    ULONG size = 32 * 1024;
    std::vector<std::uint8_t> buf(size);
    const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST;
    ULONG r = ::GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        r = ::GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    // The interface the system routes the Internet through (a route lookup; nothing is sent).
    DWORD default_if = 0;
    std::uint32_t probe = 0;
    parse_ipv4("8.8.8.8", &probe);
    if (::GetBestInterface(probe, &default_if) != NO_ERROR) default_if = 0;
    auto narrow = [](const wchar_t* w) {
        std::string s;
        if (!w) return s;
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            s.resize(static_cast<std::size_t>(n - 1));
            ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
        }
        return s;
    };
    int best = -1;
    std::uint32_t best_metric = 0;
    for (auto* a = r == NO_ERROR ? reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()) : nullptr; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        LanAdapter info;
        info.name = narrow(a->FriendlyName);
        const std::string desc = narrow(a->Description);
        if (!desc.empty() && desc != info.name) info.name += " / " + desc;
        info.loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        info.tunnel = a->IfType == IF_TYPE_TUNNEL;
        info.physical = a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211;
        info.default_route = default_if && a->IfIndex == default_if;
        info.metric = a->Ipv4Metric;
        if (!want) {
            // An overlay VPN's adapter stays Up with its address while the VPN app is off (0.7.6).
            for (auto* u = a->FirstUnicastAddress; u && !info.ip; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family == AF_INET)
                    info.ip = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr;
            }
            const VpnKind vk = vpn_kind(info);
            if (vk != VpnKind::None && vk != VpnKind::Other) {
                std::string why;
                if (!vpn_adapter_online(a->IfIndex, vk, pref, &why)) info.vpn_offline = true;
            }
        }
        std::uint32_t gateway = 0;
        for (auto* g = a->FirstGatewayAddress; g; g = g->Next) {
            if (g->Address.lpSockaddr->sa_family != AF_INET) continue;
            gateway = reinterpret_cast<sockaddr_in*>(g->Address.lpSockaddr)->sin_addr.s_addr;
            if (gateway) break;
        }
        info.gateway = gateway != 0;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            info.ip = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr;
            int score = lan_adapter_score(info);
            if (score >= 0 && !want) {
                if (const int v = vpn_adapter_score(info, pref); v >= 0) score = v;
            }
            note(info, score);
            if (want) score = info.ip == want ? 1 : -1;
            if (score < 0) continue;
            if (score < best || (score == best && info.metric >= best_metric)) continue;
            best = score;
            best_metric = info.metric;
            chosen = info;
            out.ip = info.ip;
            const ULONG prefix = u->OnLinkPrefixLength;
            out.mask = prefix ? bswap32(prefix >= 32 ? 0xffffffffu : ~(0xffffffffu >> prefix)) : 0;
            out.gateway = gateway;
            out.dns1 = out.dns2 = 0;
            int nd = 0;
            for (auto* d = a->FirstDnsServerAddress; d && nd < 2; d = d->Next) {
                if (d->Address.lpSockaddr->sa_family != AF_INET) continue;
                (nd++ ? out.dns2 : out.dns1) = reinterpret_cast<sockaddr_in*>(d->Address.lpSockaddr)->sin_addr.s_addr;
            }
            if (a->PhysicalAddressLength == 6) std::memcpy(out.mac, a->PhysicalAddress, 6);
            if (a->Mtu && a->Mtu < 65536) out.mtu = a->Mtu;
        }
    }
#else
    ifaddrs* list = nullptr;
    int best = -1;
    if (::getifaddrs(&list) == 0) {
        for (ifaddrs* i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if (!(i->ifa_flags & IFF_UP)) continue;
            LanAdapter info;
            info.name = i->ifa_name ? i->ifa_name : "";
            info.ip = reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr;
            info.loopback = (i->ifa_flags & IFF_LOOPBACK) != 0;
            info.physical = info.name.rfind("en", 0) == 0 || info.name.rfind("eth", 0) == 0 ||
                            info.name.rfind("wl", 0) == 0;
            int score = lan_adapter_score(info);
            if (score >= 0 && !want) {
                if (const int v = vpn_adapter_score(info, pref); v >= 0) score = v;
            }
            note(info, score);
            if (want) score = info.ip == want ? 1 : -1;
            if (score <= best) continue;
            best = score;
            chosen = info;
            out.ip = info.ip;
            out.mask = i->ifa_netmask ? reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr : 0;
        }
        ::freeifaddrs(list);
    }
#endif
    std::string line;
    if (want) {
        write_ipv4(text, sizeof(text), want);
        line = std::string(text) + " from " + source;
        if (out.ip == want) {
            line += " (adapter " + chosen.name + ")";
        } else {
            line += " (on no adapter of this machine)";
            out = LocalNet{};
            out.ip = want;
        }
        if ((bswap32(want) >> 24) == 127) {
            out.mask = bswap32(0xff000000u);
            out.gateway = 0;
        }
    } else if (out.ip) {
        write_ipv4(text, sizeof(text), out.ip);
        line = std::string(text) + " on " + chosen.name;
        std::string tags;
        auto tag = [&](const std::string& t) { tags += (tags.empty() ? "" : ", ") + t; };
        if (chosen.default_route) tag("default route");
        if (chosen.gateway) {
            char gw[32];
            write_ipv4(gw, sizeof(gw), out.gateway);
            tag(std::string("gateway ") + gw);
        }
        if (vpn_adapter_score(chosen, pref) >= 0) tag("hosting over " + vpn_label(chosen) + ", party_vpn=" + pref);
        else if (adapter_is_virtual(chosen.name)) tag("virtual adapter: no better one");
        if (!tags.empty()) line += " [" + tags + "]";
        line += "; candidates: " + candidates + "; BB_PARTY_LOCAL_IP overrides";
        set_vpn_in_use(vpn_adapter_score(chosen, pref) >= 0 ? vpn_label(chosen) + "|" + text : std::string());
    } else {
        line = "none (no usable IPv4 adapter" + (candidates.empty() ? std::string() : ": " + candidates) + ")";
    }
    if (how) *how = line;
    return out;
}
const LocalNet& local_net() {
    static const LocalNet net = [] {
        wsa_start();
        std::string how;
        LocalNet n = query_local_net(&how);
        log("local address %s", how.c_str());
        return n;
    }();
    return net;
}

BBNET_ABI int netctl_get_state(int* state) {
    BBNET_GUEST_RETURN();
    if (!state) return kNetCtlInvalidAddr;
    *state = 3;  // IPOBTAINED
    return 0;
}
// sceNetCtlGetInfo(code, SceNetCtlInfo*): only the member the code names is written.
BBNET_ABI int netctl_get_info(int code, void* info) {
    BBNET_GUEST_RETURN();
    if (!info) return kNetCtlInvalidAddr;
    const LocalNet& n = local_net();
    auto put_u32 = [&](std::uint32_t v) { std::memcpy(info, &v, 4); };
    auto put_ip = [&](std::uint32_t a) {
        std::memset(info, 0, 16);
        write_ipv4(static_cast<char*>(info), 16, a);
    };
    switch (code) {
        case 1: put_u32(0); break;               // DEVICE: wired
        case 2: std::memcpy(info, n.mac, 6); break;  // ETHER_ADDR
        case 3: put_u32(n.mtu); break;           // MTU
        case 4: put_u32(1); break;               // LINK: up
        case 11: put_u32(0); break;              // IP_CONFIG: DHCP
        case 12: std::memset(info, 0, 256); break;  // DHCP_HOSTNAME
        case 13: std::memset(info, 0, 128); break;  // PPPOE_AUTH_NAME
        case 14: put_ip(n.ip ? n.ip : bswap32(0x7f000001u)); break;  // IP_ADDRESS
        case 15: put_ip(n.mask ? n.mask : bswap32(0xff000000u)); break;  // NETMASK
        case 16: put_ip(n.gateway); break;       // DEFAULT_ROUTE
        case 17: put_ip(n.dns1); break;          // PRIMARY_DNS
        case 18: put_ip(n.dns2); break;          // SECONDARY_DNS
        case 19: put_u32(0); break;              // HTTP_PROXY_CONFIG: off
        case 20: std::memset(info, 0, 256); break;  // HTTP_PROXY_SERVER
        case 21: std::memset(info, 0, 2); break;    // HTTP_PROXY_PORT
        case 5: case 6: case 7: case 8: case 9: case 10:  // Wi-Fi members: wired, none
            std::memset(info, 0, code == 6 ? 36 : 8);
            break;
        default: return kNetCtlInvalidCode;
    }
    return 0;
}
BBNET_ABI int netctl_register(void* fn, void* arg, int* cid) {
    BBNET_GUEST_RETURN();
    if (!cid) return kNetCtlInvalidAddr;
    std::lock_guard<std::mutex> lock(g_net_mu);
    const int id = g_net_ctl_cb_next++;
    g_net_ctl_cbs[id] = {fn, arg};
    *cid = id;
    return 0;
}
BBNET_ABI int netctl_unregister(int cid) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lock(g_net_mu);
    g_net_ctl_cbs.erase(cid);
    return 0;
}
BBNET_ABI int netctl_check() { return 0; }  // the state never changes: no events
// SceNetCtlNatInfo {u32 size, int stunStatus, int natType, SceNetInAddr mappedAddr}.
BBNET_ABI int netctl_nat(std::uint32_t* info) {
    BBNET_GUEST_RETURN();
    if (!info) return kNetCtlInvalidAddr;
    const LocalNet& n = local_net();
    info[1] = 1;  // STUN ok
    info[2] = 2;  // NAT type 2 (moderate): the party host's relay covers type 3 peers
    info[3] = n.ip ? n.ip : bswap32(0x7f000001u);
    return 0;
}

}  // namespace

std::uint32_t forced_local_ipv4(std::string* source) {
    auto on = [](const char* name) {
        const char* v = std::getenv(name);
        return v && *v && v[0] != '0';
    };
    const char* forced = std::getenv("BB_PARTY_LOCAL_IP");
    if (forced && *forced) {
        std::uint32_t ip = 0;
        if (parse_ipv4(forced, &ip) && ip) {
            if (source) *source = "BB_PARTY_LOCAL_IP";
            return ip;
        }
        log("BB_PARTY_LOCAL_IP=%s is not an IPv4 address; ignored", forced);
    }
    for (const char* name : {"BB_PARTY_LOOPBACK", "BB_MP_LOCAL_TEST"}) {
        if (on(name)) {
            if (source) *source = std::string(name) + "=1";
            return bswap32(0x7f000001u);
        }
    }
    return 0;
}

bool adapter_is_virtual(const std::string& name) {
    std::string n;
    for (char c : name) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* const kMarks[] = {
        "hyper-v", "vethernet", "wsl", "virtualbox", "vbox", "vmware", "vmnet", "virtual", "docker",
        "tap-windows", "tap adapter", "tap-", "openvpn", "wintun", "wireguard", "tailscale", "zerotier",
        "hamachi", "radmin", "nordlynx", "vpn", "anyconnect", "fortinet", "pangp", "loopback", "bluetooth"};
    for (const char* m : kMarks) {
        if (n.find(m) != std::string::npos) return true;
    }
    return false;
}

int lan_adapter_score(const LanAdapter& a) {
    if (!a.ip || a.loopback || a.tunnel || a.vpn_offline) return -1;
    const std::uint32_t host = bswap32(a.ip);
    if ((host >> 24) == 127 || (host >> 16) == 0xa9feu) return -1;  // loopback; 169.254/16: no DHCP answer
    int score = adapter_is_virtual(a.name) ? 0 : 100;
    if (a.default_route) score += 8;
    if (a.gateway) score += 4;
    if (a.physical) score += 2;
    return score;
}

namespace {
std::string lower_text(const std::string& s) {
    std::string n;
    for (char c : s) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return n;
}
}  // namespace

VpnKind vpn_kind(const LanAdapter& a) {
    const std::string n = lower_text(a.name);
    auto has = [&](const char* m) { return n.find(m) != std::string::npos; };
    if (has("radmin")) return VpnKind::Radmin;
    if (has("hamachi")) return VpnKind::Hamachi;
    if (has("zerotier")) return VpnKind::ZeroTier;
    if (has("tailscale")) return VpnKind::Tailscale;
    if (!adapter_is_virtual(a.name)) return VpnKind::None;
    const std::uint32_t host = bswap32(a.ip);
    if ((host >> 24) == 26) return VpnKind::Radmin;
    if ((host >> 24) == 25) return VpnKind::Hamachi;
    if ((host >> 22) == (0x64400000u >> 22)) return VpnKind::Tailscale;  // 100.64.0.0/10
    if (has("vpn") || has("tap") || has("wintun") || has("wireguard") || has("nordlynx") || has("anyconnect") ||
        has("fortinet") || has("pangp"))
        return VpnKind::Other;
    return VpnKind::None;
}

std::string vpn_label(const LanAdapter& a) {
    switch (vpn_kind(a)) {
        case VpnKind::Radmin: return "Radmin VPN";
        case VpnKind::Hamachi: return "Hamachi";
        case VpnKind::ZeroTier: return "ZeroTier";
        case VpnKind::Tailscale: return "Tailscale";
        default: break;
    }
    const std::size_t slash = a.name.find(" / ");
    return slash == std::string::npos ? a.name : a.name.substr(0, slash);
}

std::string vpn_pref_normalize(const std::string& pref) {
    std::size_t b = 0, e = pref.size();
    while (b < e && std::isspace(static_cast<unsigned char>(pref[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(pref[e - 1]))) --e;
    const std::string t = pref.substr(b, e - b);
    const std::string l = lower_text(t);
    if (t.empty() || l == "auto" || l == "on" || l == "1") return "auto";
    if (l == "off" || l == "0" || l == "none" || l == "no") return "off";
    return t;
}

int vpn_adapter_score(const LanAdapter& a, const std::string& pref_in) {
    const std::string pref = vpn_pref_normalize(pref_in);
    if (pref == "off" || lan_adapter_score(a) < 0) return -1;
    const VpnKind k = vpn_kind(a);
    if (pref == "auto") {
        switch (k) {
            case VpnKind::Radmin: return 1004;
            case VpnKind::Hamachi: return 1003;
            case VpnKind::Tailscale: return 1002;
            case VpnKind::ZeroTier: return 1001;
            default: return -1;
        }
    }
    return lower_text(a.name).find(lower_text(pref)) != std::string::npos ? 1000 : -1;
}

// ---- Is the VPN really online? (0.7.6) --------------------------------------------------------
VpnState vpn_state_from_radmin_log(const std::string& text) {
    // Lines like "2026.10.23 19:58:26.453<TAB>INFO<TAB>Connected to server (17)". The last of these wins.
    VpnState st = VpnState::Unknown;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.find("Connected to server") != std::string::npos) {
            st = VpnState::Online;
        } else if (line.find("Disconnected from server") != std::string::npos ||
                   line.find("Switched Off") != std::string::npos || line.find("Service starting") != std::string::npos) {
            st = VpnState::Offline;
        }
    }
    return st;
}

VpnState vpn_state_from_tailscale_json(const std::string& json) {
    const std::size_t k = json.find("\"BackendState\"");
    if (k == std::string::npos) return VpnState::Unknown;
    const std::size_t colon = json.find(':', k);
    if (colon == std::string::npos) return VpnState::Unknown;
    const std::size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return VpnState::Unknown;
    const std::size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return VpnState::Unknown;
    const std::string v = json.substr(q1 + 1, q2 - q1 - 1);
    if (v == "Running") return VpnState::Online;
    if (v == "Stopped" || v == "NeedsLogin" || v == "NeedsMachineAuth" || v == "NoState") return VpnState::Offline;
    return VpnState::Unknown;  // Starting: no verdict
}

VpnState vpn_state_from_zerotier_info(const std::string& text) {
    if (text.find("200 info") == std::string::npos) return VpnState::Unknown;
    if (text.find("OFFLINE") != std::string::npos) return VpnState::Offline;
    if (text.find("ONLINE") != std::string::npos || text.find("TUNNELED") != std::string::npos) return VpnState::Online;
    return VpnState::Unknown;
}

bool vpn_counters_rising(const IfCounters& a, const IfCounters& b) {
    if (!a.valid || !b.valid) return false;
    return b.in_octets > a.in_octets || b.in_ucast > a.in_ucast || b.in_nucast > a.in_nucast;
}

bool vpn_decide_online(VpnState vendor, const IfCounters& before, const IfCounters& after, bool counters_trusted,
                       std::string* why) {
    auto say = [&](const char* t) {
        if (why) *why = t;
    };
    if (vendor == VpnState::Online) {
        say("the VPN app reports it connected");
        return true;
    }
    if (vendor == VpnState::Offline) {
        say("the VPN app reports it not connected");
        return false;
    }
    if (!before.valid || !after.valid) {
        say("no evidence, adapter trusted");
        return true;
    }
    if (vpn_counters_rising(before, after)) {
        say("traffic arrives on the adapter");
        return true;
    }
    if (!counters_trusted) {
        say("no traffic on the adapter, but it was named in party_vpn: trusted");
        return true;
    }
    say("nothing arrives on the adapter");
    return false;
}

#if defined(_WIN32)
namespace {
IfCounters read_counters(unsigned if_index) {
    IfCounters c;
    MIB_IF_ROW2 row{};
    row.InterfaceIndex = if_index;
    if (::GetIfEntry2(&row) == NO_ERROR) {
        c.valid = true;
        c.in_octets = row.InOctets;
        c.in_ucast = row.InUcastPkts;
        c.in_nucast = row.InNUcastPkts;
    }
    return c;
}

std::string env_text(const char* name) {
    const char* e = std::getenv(name);
    return e ? e : "";
}

// Runs a command line without a window, returns its output ("" on failure or after timeout_ms).
std::string run_capture(const std::string& cmdline, unsigned timeout_ms) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!::CreatePipe(&rd, &wr, &sa, 0)) return {};
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::string cmd = cmdline;
    const BOOL ok = ::CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::CloseHandle(wr);
    std::string out;
    if (ok) {
        // The output is small (a status line or one JSON page): the pipe's buffer holds it while we wait.
        const bool done = ::WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0;
        if (!done) ::TerminateProcess(pi.hProcess, 1);
        char buf[4096];
        DWORD n = 0;
        while (done && out.size() < (1u << 20) && ::ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    }
    ::CloseHandle(rd);
    return out;
}

bool file_exists(const std::string& p) {
    const DWORD a = ::GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string read_file_tail(const std::string& path, std::size_t max_bytes) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    const long from = size > static_cast<long>(max_bytes) ? size - static_cast<long>(max_bytes) : 0;
    std::fseek(f, from, SEEK_SET);
    std::string s(static_cast<std::size_t>(size - from), '\0');
    const std::size_t n = std::fread(&s[0], 1, s.size(), f);
    std::fclose(f);
    s.resize(n);
    // Radmin's log is UTF-16: dropping the zero bytes keeps the ASCII event names, wherever the read began.
    s.erase(std::remove(s.begin(), s.end(), '\0'), s.end());
    return s;
}

// Everything here is local: a file, or the vendor's CLI talking to its own daemon.
VpnState vendor_state(VpnKind kind, std::string* via) {
    switch (kind) {
        case VpnKind::Radmin: {
            const std::string pd = env_text("ProgramData");
            if (pd.empty()) return VpnState::Unknown;
            if (via) *via = "Radmin service.log";
            return vpn_state_from_radmin_log(read_file_tail(pd + "\\Famatech\\Radmin VPN\\service.log", 64 * 1024));
        }
        case VpnKind::Tailscale: {
            const std::string pf = env_text("ProgramFiles");
            const std::string exe = pf + "\\Tailscale\\tailscale.exe";
            if (pf.empty() || !file_exists(exe)) return VpnState::Unknown;
            if (via) *via = "tailscale status";
            return vpn_state_from_tailscale_json(run_capture("\"" + exe + "\" status --json", 4000));
        }
        case VpnKind::ZeroTier: {
            std::string pf = env_text("ProgramFiles(x86)");
            if (pf.empty()) pf = env_text("ProgramFiles");
            const std::string bat = pf + "\\ZeroTier\\One\\zerotier-cli.bat";
            if (pf.empty() || !file_exists(bat)) return VpnState::Unknown;
            if (via) *via = "zerotier-cli info";
            return vpn_state_from_zerotier_info(run_capture("cmd.exe /c \"\"" + bat + "\" info\"", 4000));
        }
        default: break;
    }
    return VpnState::Unknown;
}

struct ProbeEntry {
    std::chrono::steady_clock::time_point at{};
    bool online = true;
    bool logged = false;
    bool last_logged_online = true;
    std::string why;
};
std::mutex g_probe_mu;
std::map<unsigned, ProbeEntry> g_probe;
}  // namespace

bool vpn_adapter_online(unsigned if_index, VpnKind kind, const std::string& pref, std::string* why) {
    if (kind != VpnKind::Radmin && kind != VpnKind::Hamachi && kind != VpnKind::ZeroTier && kind != VpnKind::Tailscale) {
        if (why) *why = "not an overlay VPN";
        return true;
    }
    if (const char* off = std::getenv("BB_PARTY_VPN_PROBE"); off && off[0] == '0') {
        if (why) *why = "probe off (BB_PARTY_VPN_PROBE=0)";
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(g_probe_mu);
        auto it = g_probe.find(if_index);
        if (it != g_probe.end() && now - it->second.at < std::chrono::seconds(5)) {
            if (why) *why = it->second.why;
            return it->second.online;
        }
    }
    std::string via;
    const VpnState vendor = vendor_state(kind, &via);
    IfCounters a, b;
    if (vendor == VpnState::Unknown) {
        a = read_counters(if_index);
        if (a.valid) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            b = read_counters(if_index);
        }
    }
    std::string reason;
    const bool online = vpn_decide_online(vendor, a, b, vpn_pref_normalize(pref) == "auto", &reason);
    if (!via.empty() && vendor != VpnState::Unknown) reason = via + ": " + reason;
    const char* label = kind == VpnKind::Radmin ? "Radmin VPN"
                        : kind == VpnKind::Hamachi ? "Hamachi"
                        : kind == VpnKind::ZeroTier ? "ZeroTier"
                                                    : "Tailscale";
    std::lock_guard<std::mutex> lk(g_probe_mu);
    ProbeEntry& e = g_probe[if_index];
    e.at = now;
    e.online = online;
    e.why = reason;
    if (!e.logged || e.last_logged_online != online) {
        log("vpn adapter if%u (%s): %s (%s)", if_index, label, online ? "online" : "OFFLINE, not used", reason.c_str());
        e.logged = true;
        e.last_logged_online = online;
    }
    if (why) *why = reason;
    return online;
}
#else
bool vpn_adapter_online(unsigned, VpnKind, const std::string&, std::string* why) {
    if (why) *why = "not checked on this platform";
    return true;
}
#endif

int select_vpn_adapter(const std::vector<LanAdapter>& adapters, const std::string& pref) {
    int best = -1, best_score = -1;
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        const int s = vpn_adapter_score(adapters[i], pref);
        if (s > best_score) {
            best_score = s;
            best = static_cast<int>(i);
        }
    }
    return best;
}

std::string vpn_preference() {
    const char* e = std::getenv("BB_PARTY_VPN");
    return vpn_pref_normalize(e ? e : "");
}

std::string vpn_in_use() {
    local_net();  // chosen once; the cache holds g_vpn_in_use
    std::lock_guard<std::mutex> lk(g_vpn_mu);
    return g_vpn_in_use;
}

std::uint32_t query_local_ipv4(std::string* how) {
    wsa_start();
    return query_local_net(how).ip;
}

const Export kNetExports[] = {
    {"sceNetInit", reinterpret_cast<void*>(net_init)},
    {"sceNetTerm", reinterpret_cast<void*>(net_term)},
    {"sceNetErrnoLoc", reinterpret_cast<void*>(net_errno_loc)},
    {"sceNetPoolCreate", reinterpret_cast<void*>(net_pool_create)},
    {"sceNetPoolDestroy", reinterpret_cast<void*>(net_pool_destroy)},
    {"sceNetSocket", reinterpret_cast<void*>(net_socket)},
    {"sceNetSocketClose", reinterpret_cast<void*>(net_socket_close)},
    {"sceNetSocketAbort", reinterpret_cast<void*>(net_socket_abort)},
    {"sceNetBind", reinterpret_cast<void*>(net_bind)},
    {"sceNetListen", reinterpret_cast<void*>(net_listen)},
    {"sceNetAccept", reinterpret_cast<void*>(net_accept)},
    {"sceNetConnect", reinterpret_cast<void*>(net_connect)},
    {"sceNetShutdown", reinterpret_cast<void*>(net_shutdown)},
    {"sceNetSetsockopt", reinterpret_cast<void*>(net_setsockopt)},
    {"sceNetGetsockopt", reinterpret_cast<void*>(net_getsockopt)},
    {"sceNetGetsockname", reinterpret_cast<void*>(net_getsockname)},
    {"sceNetRecv", reinterpret_cast<void*>(net_recv)},
    {"sceNetSend", reinterpret_cast<void*>(net_send)},
    {"sceNetRecvfrom", reinterpret_cast<void*>(net_recvfrom)},
    {"sceNetSendto", reinterpret_cast<void*>(net_sendto)},
    {"sceNetHtons", reinterpret_cast<void*>(net_htons)},
    {"sceNetNtohs", reinterpret_cast<void*>(net_htons)},
    {"sceNetHtonl", reinterpret_cast<void*>(net_htonl)},
    {"sceNetNtohl", reinterpret_cast<void*>(net_htonl)},
    {"sceNetInetPton", reinterpret_cast<void*>(net_inet_pton)},
    {"sceNetInetNtop", reinterpret_cast<void*>(net_inet_ntop)},
    {"sceNetEpollCreate", reinterpret_cast<void*>(net_epoll_create)},
    {"sceNetEpollDestroy", reinterpret_cast<void*>(net_epoll_destroy)},
    {"sceNetEpollControl", reinterpret_cast<void*>(net_epoll_control)},
    {"sceNetEpollWait", reinterpret_cast<void*>(net_epoll_wait)},
    {"sceNetEpollAbort", reinterpret_cast<void*>(net_epoll_abort)},
    {"sceNetResolverCreate", reinterpret_cast<void*>(net_resolver_create)},
    {"sceNetResolverDestroy", reinterpret_cast<void*>(net_resolver_destroy)},
    {"sceNetResolverStartNtoa", reinterpret_cast<void*>(net_resolver_ntoa)},
    {"sceNetResolverStartAton", reinterpret_cast<void*>(net_resolver_aton)},
    {"sceNetCtlGetState", reinterpret_cast<void*>(netctl_get_state)},
    {"sceNetCtlGetInfo", reinterpret_cast<void*>(netctl_get_info)},
    {"sceNetCtlRegisterCallback", reinterpret_cast<void*>(netctl_register)},
    {"sceNetCtlUnregisterCallback", reinterpret_cast<void*>(netctl_unregister)},
    {"sceNetCtlCheckCallback", reinterpret_cast<void*>(netctl_check)},
    {"sceNetCtlGetNatInfo", reinterpret_cast<void*>(netctl_nat)},
    {nullptr, nullptr},
};

// --- The reflexive address, relay and hole punching (session layer, A3+) -----------------------

std::uint16_t p2p_bound_port() { return g_bound_port.load(); }

bool p2p_open() {
    std::lock_guard<std::mutex> lock(g_net_mu);
    if (!p2p_port_open(settings().party_port)) return false;
    g_port_pinned.store(true);  // the session layer keeps it while no game socket is bound
    return true;
}

std::uint32_t local_ipv4() { return local_net().ip; }

bool p2p_stun(const char* host, std::uint16_t sport, int timeout_ms, std::uint32_t* mapped_addr,
              std::uint16_t* mapped_port, bool want_relay, RelayInfo* relay_out) {
    wsa_start();
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    std::uint32_t literal = 0;
    if (parse_ipv4(host, &literal)) {
        sa.sin_addr.s_addr = literal;
    } else {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) {
            log("stun: cannot resolve %s", host ? host : "(null)");
            return false;
        }
        std::memcpy(&sa, res->ai_addr, sizeof(sa));
        ::freeaddrinfo(res);
    }
    sa.sin_port = bswap16(sport);
    std::shared_ptr<P2pPort> port;
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        port = p2p_port_open(settings().party_port);
        if (port) g_port_pinned.store(true);
    }
    if (!port) {
        log("stun: no party port %u to ask from", settings().party_port);
        return false;
    }
    // With the relay: the HELLO carries the token this server gave, else zeros.
    std::uint8_t token[net::stun::kTokenLen] = {};
    bool have_token = false;
    if (want_relay) have_token = g_relay.token_for(sa.sin_addr.s_addr, sa.sin_port, token);
    std::uint8_t req[net::stun::kMaxRequest];
    std::unique_lock<std::mutex> lk(port->mu);
    if (port->stun_pending) return false;  // one at a time
    const std::size_t req_len =
        net::stun::build_binding_request(req, port->stun_txid, want_relay, have_token ? token : nullptr);
    port->stun_pending = true;
    port->stun_done = false;
    port->stun_server_addr = sa.sin_addr.s_addr;
    port->stun_server_port = sa.sin_port;
    lk.unlock();
    (void)port_send(*port, req, req_len, &sa);
    lk.lock();
    port->stun_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return port->stun_done; });
    const bool ok = port->stun_done;
    port->stun_pending = false;
    if (!ok) return false;
    if (mapped_addr) *mapped_addr = port->stun_addr;
    if (mapped_port) *mapped_port = port->stun_port;
    if (want_relay) {
        const net::stun::Relay& relay = port->stun_relay;
        if (relay_out) {
            relay_out->present = relay.present;
            relay_out->vport = relay.vport;
            relay_out->observed_addr = relay.observed_addr;
            relay_out->observed_port = relay.observed_port;
        }
        if (g_relay.on_answer(sa.sin_addr.s_addr, sa.sin_port, relay)) {
            char ip[32];
            write_ipv4(ip, sizeof(ip), sa.sin_addr.s_addr);
            std::uint16_t vp = 0;
            if (g_relay.get(nullptr, &vp)) {
                log("relay on: %s:%u is our port %u; datagrams for its other ports go through it", ip, sport, vp);
            } else {
                log("relay off (%s:%u offers none)", ip, sport);
            }
        }
    }
    return true;
}

bool p2p_relay(std::uint32_t* server, std::uint16_t* vport) { return g_relay.get(server, vport); }

bool p2p_relay_vport_for(std::uint32_t addr, std::uint16_t port_host, std::uint16_t* vport) {
    return g_relay_server.vport_for(addr, port_host, vport);
}

void p2p_set_relay_admit(std::function<bool(std::uint32_t addr)> admit) { g_relay_server.set_admit(std::move(admit)); }

void p2p_set_link_handler(std::function<void(std::uint32_t, std::uint16_t, const std::uint8_t*, std::size_t)> fn) {
    std::lock_guard<std::mutex> lk(g_link_handler_mu);
    g_link_handler = std::move(fn);
}

bool p2p_send_raw(std::uint32_t addr, std::uint16_t port_nbo, const std::uint8_t* d, std::size_t n) {
    std::shared_ptr<P2pPort> port;
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        port = p2p_port_open(settings().party_port);
        if (port) g_port_pinned.store(true);
    }
    if (!port) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = addr;
    sa.sin_port = port_nbo;
    return port_send(*port, d, n, &sa) >= 0;
}

const udp::RelayClient& p2p_relay_client() { return g_relay; }

// F2: IPv6 candidates of other guests (the host's net6 table) by their IPv4 key, applied to
// PeerPaths when the game adds that peer (the table may come first).
std::mutex g_peer6_mu;
std::map<std::uint64_t, udp::Ep6> g_peer6;

void p2p_add_peer(const char* label, std::uint32_t addr, std::uint16_t port_host, std::uint16_t relay_port) {
    if (settings().host || !addr || !port_host) return;
    if (g_relay.is_server(addr, bswap16(port_host))) return;  // the host: its port is the relay
    std::uint16_t own = 0;
    g_relay.get(nullptr, &own);
    if (relay_port && relay_port == own) return;  // ourselves
    if (relay_port) g_relay.add_relay_port(relay_port);
    paths().add(addr, bswap16(port_host), relay_port, Clock::now());
    {
        std::lock_guard<std::mutex> lk(g_peer6_mu);
        if (g_peer6.count(source_key(addr, bswap16(port_host))))
            paths().set_v6(addr, bswap16(port_host), true, Clock::now());
    }
    if (party_trace()) log("paths: %s relay port %u", label ? label : "peer", relay_port);
}

udp::Router& p2p_router() { return router(); }

void p2p_set_ipv6(bool on) {
    g_ipv6_wanted.store(on);
    router().set_ipv6(on);
    if (!on) return;
    std::lock_guard<std::mutex> lock(g_net_mu);
    auto it = g_p2p_ports.find(g_bound_port.load());
    if (it != g_p2p_ports.end()) open_v6(*it->second);
}

bool p2p_ipv6_open() {
    std::lock_guard<std::mutex> lock(g_net_mu);
    auto it = g_p2p_ports.find(g_bound_port.load());
    return it != g_p2p_ports.end() && sock_ok(it->second->fd6.load());
}

void p2p_peer_v6(std::uint32_t addr, std::uint16_t port_host, const udp::Ep6& e6) {
    if (!addr || !port_host) return;
    router().bind_v6(addr, bswap16(port_host), e6, false);
    {
        std::lock_guard<std::mutex> lk(g_peer6_mu);
        if (g_peer6.size() < 64) g_peer6[source_key(addr, bswap16(port_host))] = e6;
    }
    paths().set_v6(addr, bswap16(port_host), true, Clock::now());
    flush_route_notes();
}

bool p2p_send_wire(const udp::Wire& w) {
    std::shared_ptr<P2pPort> port;
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        auto it = g_p2p_ports.find(g_bound_port.load());
        if (it != g_p2p_ports.end()) port = it->second;
    }
    return port && wire_send(*port, w) >= 0;
}

void p2p_punch6(const char* label, const udp::Ep6& e6) {
    std::shared_ptr<P2pPort> port;
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        auto it = g_p2p_ports.find(g_bound_port.load());
        if (it != g_p2p_ports.end()) port = it->second;
    }
    if (!port || !sock_ok(port->fd6.load())) return;
    const std::string name = label ? label : "peer";
    std::thread([port, e6, name] {
        // Opens our IPv6 firewall (most home routers keep a pinhole per outbound flow) towards it.
        udp::Wire w;
        w.v6 = true;
        w.addr6 = e6.addr;
        w.port = e6.port;
        w.data.assign(udp::kProbe, udp::kProbe + sizeof(udp::kProbe));
        int sent = 0;
        for (int i = 0; i < 40 && !port->stop.load(std::memory_order_relaxed); ++i) {
            if (wire_send(*port, w) >= 0) ++sent;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        log("punch %s [%s]:%u over IPv6: %d probes", name.c_str(), udp::ip6_text(e6.addr).c_str(), bswap16(e6.port), sent);
    }).detach();
}

bool local_ipv6(udp::Ip6* out, std::string* how) {
    if (const char* forced = std::getenv("BB_PARTY_IPV6_ADDR"); forced && *forced) {
        if (udp::parse_ip6(forced, out) && !udp::ip6_zero(*out)) {
            if (how) *how = std::string("BB_PARTY_IPV6_ADDR=") + forced;
            return true;
        }
        log("BB_PARTY_IPV6_ADDR=%s is not an IPv6 address; ignored", forced);
    }
#if defined(_WIN32)
    wsa_start();
    ULONG size = 32 * 1024;
    std::vector<std::uint8_t> buf(size);
    const ULONG flags = GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = ::GetAdaptersAddresses(AF_INET6, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        r = ::GetAdaptersAddresses(AF_INET6, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    bool found = false;
    ULONG best_valid = 0, best_pref = 0;
    std::string best_name;
    for (auto* a = r == NO_ERROR ? reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()) : nullptr; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL)
            continue;
        std::string name;
        if (a->FriendlyName) {
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) {
                name.resize(static_cast<std::size_t>(n - 1));
                ::WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, &name[0], n, nullptr, nullptr);
            }
        }
        if (adapter_is_virtual(name)) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET6 || u->DadState != IpDadStatePreferred) continue;
            udp::Ip6 ip;
            std::memcpy(ip.b.data(), &reinterpret_cast<sockaddr_in6*>(u->Address.lpSockaddr)->sin6_addr, 16);
            // Global unicast, not Teredo (2001::/32) or 6to4 (2002::/16): tunnels that rarely pass.
            if (!udp::ip6_global(ip) || (ip.b[0] == 0x20 && ip.b[1] == 0x01 && ip.b[2] == 0 && ip.b[3] == 0) ||
                (ip.b[0] == 0x20 && ip.b[1] == 0x02))
                continue;
            // Privacy first (F2 review L5): the shortest-lived preferred address, i.e. a temporary
            // privacy address where Windows makes them, not the stable one (on some systems built
            // from the adapter's MAC). The host's code then changes when that address rotates.
            if (u->PreferredLifetime == 0) continue;
            if (!found || u->PreferredLifetime < best_pref ||
                (u->PreferredLifetime == best_pref && u->ValidLifetime < best_valid)) {
                found = true;
                *out = ip;
                best_valid = u->ValidLifetime;
                best_pref = u->PreferredLifetime;
                best_name = name;
            }
        }
    }
    if (found && how) *how = "adapter " + best_name;
    if (!found && how) *how = "no global IPv6 address on an adapter";
    return found;
#else
    if (how) *how = "not implemented here";
    return false;
#endif
}

std::string p2p_paths_status() {
    std::string s;
    for (const auto& p : paths().peers()) {
        char ip[32];
        write_ipv4(ip, sizeof(ip), p.addr);
        char b[96];
        std::snprintf(b, sizeof(b), "%s%s:%u %s", s.empty() ? "" : ", ", ip, bswap16(p.port),
                      p.relayed ? "relay" : (p.answered ? "direct" : "probing"));
        s += b;
    }
    return s;
}

void p2p_punch(const char* label, std::uint32_t addr, std::uint16_t port_host, std::uint32_t local_addr,
               std::uint16_t local_port) {
    std::shared_ptr<P2pPort> port;
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        auto it = g_p2p_ports.find(g_bound_port.load());
        if (it != g_p2p_ports.end()) port = it->second;
    }
    if (!port || !addr || !port_host) return;
    {
        std::uint8_t hdr[kRelayHeader];
        std::uint32_t sa_addr = 0;
        std::uint16_t sa_port = 0;
        if (g_relay.frame_for(addr, bswap16(port_host), hdr, &sa_addr, &sa_port)) return;  // the relay needs no hole
    }
    // A guest a member relays for (party_route.h): its traffic goes through that member; raw
    // probes would only open a direct flow beside the relayed one.
    if (router().relayed_guest(addr, bswap16(port_host))) {
        log("punch %s: through a member's relay (no direct punch)", label ? label : "peer");
        return;
    }
    std::vector<sockaddr_in> targets;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = addr;
    a.sin_port = bswap16(port_host);
    targets.push_back(a);
    bool local_ok = local_addr && local_port && (local_addr != addr || local_port != port_host);
    if (local_ok && self_guard_on()) {
        std::uint32_t own_public = 0;
        {
            std::lock_guard<std::mutex> lk(port->mu);
            own_public = port->stun_addr;
        }
        if (!udp::local_candidate_usable(local_addr, bswap16(local_port), bswap16(port->port), own_ifaces(), own_public,
                                         addr)) {
            char lip[32];
            write_ipv4(lip, sizeof(lip), local_addr);
            log("punch %s: ignoring its local address %s:%u (our own address, or not on our network)",
                label ? label : "peer", lip, local_port);
            local_ok = false;
        }
    }
    if (local_ok) {
        sockaddr_in b{};
        b.sin_family = AF_INET;
        b.sin_addr.s_addr = local_addr;
        b.sin_port = bswap16(local_port);
        targets.push_back(b);
    }
    const std::string name = label ? label : "peer";
    std::thread([port, targets, name] {
        const auto start = Clock::now();
        int sent = 0;
        bool heard = false;
        // Anything from either address since the start: the peer is through.
        auto heard_one = [&](const sockaddr_in& t) {
            auto it = port->heard.find(source_key(t.sin_addr.s_addr, t.sin_port));
            return it != port->heard.end() && it->second >= start;
        };
        auto heard_since_start = [&] {
            std::lock_guard<std::mutex> lk(port->mu);
            for (const sockaddr_in& t : targets)
                if (heard_one(t)) return true;
            return false;
        };
        for (int i = 0; i < 40 && !port->stop.load(std::memory_order_relaxed); ++i) {
            for (const sockaddr_in& t : targets) {
                if (raw_sendto(port->fd, udp::kProbe, sizeof(udp::kProbe), &t) >= 0) ++sent;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (heard_since_start()) {
                heard = true;
                break;
            }
        }
        if (heard && targets.size() > 1) {
            // Give the other candidate a moment to answer too, then pick (the local one wins).
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            bool primary_heard, local_heard;
            {
                std::lock_guard<std::mutex> lk(port->mu);
                primary_heard = heard_one(targets[0]);
                local_heard = heard_one(targets[1]);
            }
            char pip[32], lip[32];
            write_ipv4(pip, sizeof(pip), targets[0].sin_addr.s_addr);
            write_ipv4(lip, sizeof(lip), targets[1].sin_addr.s_addr);
            const bool primary_on_net = self_guard_on() && udp::on_own_subnet(targets[0].sin_addr.s_addr, own_ifaces());
            if (udp::punch_choice(primary_heard, local_heard, true, primary_on_net) == 1) {
                aliases().set(targets[0].sin_addr.s_addr, targets[0].sin_port, targets[1].sin_addr.s_addr,
                              targets[1].sin_port);
                log("punch %s: using its local address %s:%u (heard%s; %s:%u stays its game address)", name.c_str(),
                    lip, bswap16(targets[1].sin_port), primary_heard ? ", the public one too" : "", pip,
                    bswap16(targets[0].sin_port));
            } else {
                aliases().clear(targets[0].sin_addr.s_addr, targets[0].sin_port);
            }
        }
        char ip[32];
        write_ipv4(ip, sizeof(ip), targets[0].sin_addr.s_addr);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        log("punch %s %s:%u%s: %d probes, %s after %lld ms", name.c_str(), ip, bswap16(targets[0].sin_port),
            targets.size() > 1 ? " (+local)" : "", sent, heard ? "peer heard" : "nothing heard",
            static_cast<long long>(ms));
    }).detach();
}

std::int64_t p2p_ms_since_heard(std::uint32_t addr, std::uint16_t port_host) {
    std::shared_ptr<P2pPort> port;
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        auto it = g_p2p_ports.find(g_bound_port.load());
        if (it != g_p2p_ports.end()) port = it->second;
    }
    if (!port || !addr || !port_host) return -1;
    std::lock_guard<std::mutex> lk(port->mu);
    auto it = port->heard.find(source_key(addr, bswap16(port_host)));
    if (it == port->heard.end()) return -1;
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - it->second).count();
}

std::string p2p_status() {
    std::shared_ptr<P2pPort> port;
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        auto it = g_p2p_ports.find(g_bound_port.load());
        if (it != g_p2p_ports.end()) port = it->second;
    }
    if (!port) return "party port closed";
    std::lock_guard<std::mutex> lk(port->mu);
    char buf[200];
    std::snprintf(buf, sizeof(buf), "udp %u: rx %llu, %zu vport(s), probes %llu, stun %llu, relayed %llu", port->port,
                  static_cast<unsigned long long>(port->rx), port->by_vport.size(),
                  static_cast<unsigned long long>(port->probes), static_cast<unsigned long long>(port->stun_answered),
                  static_cast<unsigned long long>(port->relayed));
    const std::string pp = p2p_paths_status();
    return pp.empty() ? std::string(buf) : std::string(buf) + "; peers " + pp;
}

// C6 (parfin3): a crashed peer's game is gone; its restarted game comes back from the same
// address and port. Forget what the outage replay (F5) keeps for that source on every game socket,
// so the new game never gets the dead session's reliable records replayed into it (a7b / a7t1: the
// replay right after the restarted guest joined the room, then its title join got no snapshot).
std::size_t p2p_forget_source(std::uint64_t key) {
    std::shared_ptr<P2pPort> port;
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        auto it = g_p2p_ports.find(g_bound_port.load());
        if (it != g_p2p_ports.end()) port = it->second;
    }
    if (!port) return 0;
    std::size_t n = 0;
    std::lock_guard<std::mutex> lk(port->mu);
    for (auto& [vp, q] : port->by_vport) {
        std::lock_guard<std::mutex> ql(q->mu);
        n += q->replay.erase(key) + q->replay_to.erase(key) + q->keepalive.erase(key) + q->last_small.erase(key) +
             q->strip_until.erase(key) + q->heard.erase(key);  // no "last heard": no resume replay
    }
    return n;
}

void set_packet_trace(PacketTraceFn fn) { g_packet_trace.store(fn); }
void set_p2p_recv_hook(P2pRecvHookFn fn) { g_p2p_recv_hook.store(fn); }
// F5: while the party link says a peer is only temporarily lost (a network outage, its slot
// kept), the game hears that peer's last small datagram again every 2 s, so its session's 25 s
// silence timeout does not end the session before the network is back. Sources silent for 2.5 s
// to 120 s only (a live peer is never replayed); the heard times stay real (C6 / the resume check).
std::atomic<bool> g_outage_keepalive{false};
std::atomic<bool> g_outage_keepalive_thread{false};
void outage_keepalive_main() {
    std::uint64_t logged_on = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!g_outage_keepalive.load()) {
            logged_on = 0;
            continue;
        }
        std::vector<std::shared_ptr<SockQueue>> qs;
        {
            std::lock_guard<std::mutex> lock(g_net_mu);
            for (const auto& [id, ns] : g_net_socks)
                if (ns.p2p && ns.queue) qs.push_back(ns.queue);
        }
        const auto now = Clock::now();
        std::uint64_t n = 0;
        for (const auto& q : qs) {
            {
                std::lock_guard<std::mutex> lk(q->mu);
                for (const auto& [key, t] : q->heard) {
                    const auto age = now - t;
                    if (age < std::chrono::milliseconds(2500) || age > std::chrono::seconds(120)) continue;
                    // The last reliable data record (resets the idle deadline); the last small
                    // datagram only for a peer that never sent one.
                    const Datagram* ka = nullptr;
                    if (auto k = q->keepalive.find(key); k != q->keepalive.end()) ka = &k->second;
                    else if (auto l = q->last_small.find(key); l != q->last_small.end()) ka = &l->second;
                    if (!ka || q->q.size() >= kMaxInbox) continue;
                    q->q.push_back(*ka);
                    ++q->replayed;
                    ++n;
                }
            }
            q->cv.notify_all();
        }
        if (n) {
            net_wake();
            if (logged_on++ % 5 == 0) log("outage keepalive: %llu datagram(s) replayed to the game (pass %llu)",
                                          static_cast<unsigned long long>(n), static_cast<unsigned long long>(logged_on));
        }
    }
}

void p2p_outage_keepalive(bool on) {
    if (g_outage_keepalive.exchange(on) == on) return;
    log("outage keepalive %s", on ? "on: the game hears silent peers' last keepalive every 2 s" : "off");
    if (on && !g_outage_keepalive_thread.exchange(true)) std::thread(outage_keepalive_main).detach();
}

void p2p_set_gap_hook(std::function<void(std::uint32_t, std::uint16_t, double)> fn) {
    std::lock_guard<std::mutex> lk(g_gap_hook_mu);
    g_gap_hook = std::move(fn);
}

std::int64_t p2p_game_rx_age_ms() {
    std::vector<std::shared_ptr<SockQueue>> qs;
    {
        std::lock_guard<std::mutex> lock(g_net_mu);
        for (const auto& [id, ns] : g_net_socks)
            if (ns.p2p && ns.queue) qs.push_back(ns.queue);
    }
    bool any = false;
    Clock::time_point last{};
    for (const auto& q : qs) {
        std::lock_guard<std::mutex> lk(q->mu);
        for (const auto& [k, t] : q->heard) {
            if (!any || t > last) last = t;
            any = true;
        }
    }
    if (!any) return -1;
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - last).count();
}
std::map<std::uint64_t, std::int64_t> p2p_heard(int sock) {
    std::map<std::uint64_t, std::int64_t> out;
    NetSock ns;
    if (!sock_state(sock, &ns) || !ns.p2p || !ns.queue) return out;
    std::lock_guard<std::mutex> lk(ns.queue->mu);
    for (const auto& [k, t] : ns.queue->heard)
        out[k] = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    return out;
}

}  // namespace bbnet
