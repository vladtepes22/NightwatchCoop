// SPDX-License-Identifier: GPL-3.0-or-later
// Internal: the few socket calls PartyLink and the address helpers need, Winsock or BSD.
#pragma once

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstdint>
#include <cstdlib>

namespace party::sock {

// Local tests (BB_PARTY_LOOPBACK=1 / BB_MP_LOCAL_TEST=1): every party socket binds 127.0.0.1 only.
// Nothing then listens on the network, so Windows Firewall never prompts for the test programs or
// the harness's per-agent game copies.
inline bool loopback_only() {
    for (const char* n : {"BB_PARTY_LOOPBACK", "BB_MP_LOCAL_TEST"}) {
        const char* v = std::getenv(n);
        if (v && v[0] == '1') return true;
    }
    return false;
}

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalid = INVALID_SOCKET;
using PollFd = WSAPOLLFD;
inline int poll(PollFd* fds, unsigned long n, int timeout_ms) { return WSAPoll(fds, n, timeout_ms); }
inline void close(Socket s) { closesocket(s); }
inline int last_error() { return WSAGetLastError(); }
inline bool would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
inline bool set_nonblocking(Socket s) {
    u_long one = 1;
    return ioctlsocket(s, FIONBIO, &one) == 0;
}
inline bool startup() {
    static const bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
using socklen = int;
#else
using Socket = int;
constexpr Socket kInvalid = -1;
using PollFd = pollfd;
inline int poll(PollFd* fds, unsigned long n, int timeout_ms) { return ::poll(fds, n, timeout_ms); }
inline void close(Socket s) { ::close(s); }
inline int last_error() { return errno; }
inline bool would_block(int e) { return e == EWOULDBLOCK || e == EAGAIN || e == EINPROGRESS; }
inline bool set_nonblocking(Socket s) { return fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) == 0; }
inline bool startup() { return true; }
using socklen = socklen_t;
#endif

inline long send_some(Socket s, const void* p, std::size_t n) {
#if defined(_WIN32)
    return ::send(s, static_cast<const char*>(p), static_cast<int>(n), 0);
#else
    return ::send(s, p, n, MSG_NOSIGNAL);
#endif
}
inline long recv_some(Socket s, void* p, std::size_t n) {
#if defined(_WIN32)
    return ::recv(s, static_cast<char*>(p), static_cast<int>(n), 0);
#else
    return ::recv(s, p, n, 0);
#endif
}

}  // namespace party::sock
