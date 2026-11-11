/* Hides network addresses in the game's log output (0.8.1, NW-019b146): 1.2.3.4 -> 1.2.x.x,
 * 2001:db8:a:b::1 -> 2001:db8:x:x:x:x:x:x. Loopback (127.x, ::1), 0.0.0.0 and 255.255.255.255 stay.
 * The same rule as scripts/ipmask.py (tests/ipmask_vectors.txt pins both). BB_LOG_RAW_IPS=1 (or bbport.ini
 * log_raw_ips=1) keeps the addresses in the log, for a network problem that needs them. */
#ifndef RUNTIME_IPMASK_H
#define RUNTIME_IPMASK_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Writes the masked copy of in[0..n) to out and returns its length. out must hold 2 * n + 16 bytes (an IPv6
 * address in its compressed form grows). in and out must not overlap. */
size_t runtime_ipmask(const char *in, size_t n, char *out);
#ifdef __cplusplus
}
#endif
#endif
