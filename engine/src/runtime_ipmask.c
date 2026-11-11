/* Hides network addresses in log text: see runtime_ipmask.h. */
#include "runtime_ipmask.h"
#include <string.h>

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int is_word(char c) { return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

/* IPv4 at s[i..n): 4 groups of 1-3 digits; returns the length (0: not an address) and the first two octets' text span. */
static size_t v4_at(const char *s, size_t i, size_t n, size_t *g2_end, int *keep) {
    size_t p = i, ends[4];
    int val[4];
    for (int g = 0; g < 4; ++g) {
        size_t b = p;
        int v = 0;
        while (p < n && is_digit(s[p]) && p - b < 3) { v = v * 10 + (s[p] - '0'); ++p; }
        if (p == b) return 0;
        ends[g] = p;
        val[g] = v;
        if (g < 3) {
            if (p >= n || s[p] != '.') return 0;
            ++p;
        }
    }
    /* (?![\w.]*\d): no digit in the run of word characters and dots that follows */
    for (size_t q = p; q < n && (is_word(s[q]) || s[q] == '.'); ++q)
        if (is_digit(s[q])) return 0;
    for (int g = 0; g < 4; ++g) if (val[g] > 255) return 0;
    *keep = val[0] == 127 || (val[0] == 0 && val[1] == 0 && val[2] == 0 && val[3] == 0) ||
            (val[0] == 255 && val[1] == 255 && val[2] == 255 && val[3] == 255);
    *g2_end = ends[1];
    return p - i;
}

/* IPv6 at s[i..n): a run of hex digits and colons, 8 groups or a "::" with 2 or more groups. */
static size_t v6_at(const char *s, size_t i, size_t n, char *out, size_t *olen) {
    size_t p = i;
    while (p < n && (is_hex(s[p]) || s[p] == ':')) ++p;
    if (p < n && is_word(s[p])) return 0; /* (?![\w:]) */
    size_t len = p - i;
    if (len < 2) return 0;
    int groups = 0, dbl = 0;
    size_t gl = 0;
    for (size_t k = 0; k <= len; ++k) {
        if (k == len || s[i + k] == ':') {
            if (gl > 4) return 0;
            if (gl) ++groups;
            if (k < len && k + 1 < len && s[i + k + 1] == ':') dbl = 1;
            gl = 0;
        } else {
            ++gl;
        }
    }
    int colons = 0;
    for (size_t k = 0; k < len; ++k) if (s[i + k] == ':') ++colons;
    if (colons < 2 || colons > 7) return 0;
    if (!(groups == 8 || (dbl && groups >= 2))) return 0;
    /* the first two groups before any "::" */
    size_t o = 0;
    int lead = 0;
    size_t k = 0;
    while (k < len && lead < 2) {
        if (s[i + k] == ':') {
            if (k + 1 < len && s[i + k + 1] == ':') break;
            ++k;
            continue;
        }
        size_t b = k;
        while (k < len && s[i + k] != ':') ++k;
        if (lead) out[o++] = ':';
        memcpy(out + o, s + i + b, k - b);
        o += k - b;
        ++lead;
    }
    for (; lead < 8; ++lead) {
        if (lead) out[o++] = ':';
        out[o++] = 'x';
    }
    *olen = o;
    return len;
}

size_t runtime_ipmask(const char *in, size_t n, char *out) {
    size_t o = 0, i = 0;
    while (i < n) {
        char c = in[i];
        char prev = i ? in[i - 1] : 0;
        char buf[48];
        size_t bl = 0, g2 = 0, l6 = 0, l4 = 0;
        int keep = 0;
        if ((is_hex(c) || c == ':') && !is_word(prev) && prev != ':') l6 = v6_at(in, i, n, buf, &bl);
        if (l6) {
            memcpy(out + o, buf, bl);
            o += bl;
            i += l6;
            continue;
        }
        if (is_digit(c) && !is_word(prev) && prev != '.') l4 = v4_at(in, i, n, &g2, &keep);
        if (l4) {
            if (keep) {
                memcpy(out + o, in + i, l4);
                o += l4;
            } else {
                memcpy(out + o, in + i, g2 - i);
                o += g2 - i;
                memcpy(out + o, ".x.x", 4);
                o += 4;
            }
            i += l4;
            continue;
        }
        out[o++] = c;
        ++i;
    }
    return o;
}
