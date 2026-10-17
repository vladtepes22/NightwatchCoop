// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define _CRT_RAND_S
#endif
#include <stdlib.h>

#include "party_crypto.h"

#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include "../../third_party/monocypher/monocypher.h"
}

namespace party::crypto {

void random_bytes(void* out, std::size_t n) {
    auto* p = static_cast<std::uint8_t*>(out);
#if defined(_WIN32)
    while (n) {
        unsigned int v = 0;
        rand_s(&v);
        std::size_t k = n < 4 ? n : 4;
        std::memcpy(p, &v, k);
        p += k;
        n -= k;
    }
#else
    if (std::FILE* f = std::fopen("/dev/urandom", "rb")) {
        std::size_t got = std::fread(p, 1, n, f);
        std::fclose(f);
        if (got == n) return;
    }
    // Not reached on any supported system; never leave the buffer predictable-zero silently.
    for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<std::uint8_t>(std::rand());
#endif
}

Key derive_party_key(const std::string& password, const std::uint8_t secret[8]) {
    std::vector<std::uint8_t> pass(password.begin(), password.end());
    pass.insert(pass.end(), secret, secret + 8);
    static const std::uint8_t kSalt[8] = {'b', 'b', 'p', 'a', 'r', 't', 'y', '1'};
    crypto_argon2_config cfg{};
    cfg.algorithm = CRYPTO_ARGON2_I;
    cfg.nb_blocks = 8192;  // 8 MiB
    cfg.nb_passes = 3;
    cfg.nb_lanes = 1;
    crypto_argon2_inputs in{};
    in.pass = pass.data();
    in.pass_size = static_cast<std::uint32_t>(pass.size());
    in.salt = kSalt;
    in.salt_size = sizeof(kSalt);
    std::vector<std::uint8_t> work(static_cast<std::size_t>(cfg.nb_blocks) * 1024);
    Key key{};
    crypto_argon2(key.data(), 32, work.data(), cfg, in, crypto_argon2_no_extras);
    crypto_wipe(work.data(), work.size());
    crypto_wipe(pass.data(), pass.size());
    return key;
}

static Key keyed(const Key& k, const char* label, const Nonce& a, const Nonce& b) {
    crypto_blake2b_ctx ctx;
    crypto_blake2b_keyed_init(&ctx, 32, k.data(), 32);
    crypto_blake2b_update(&ctx, reinterpret_cast<const std::uint8_t*>(label), std::strlen(label));
    crypto_blake2b_update(&ctx, a.data(), a.size());
    crypto_blake2b_update(&ctx, b.data(), b.size());
    Key out{};
    crypto_blake2b_final(&ctx, out.data());
    return out;
}

Key auth_proof(const Key& party_key, const Nonce& host_nonce, const Nonce& guest_nonce) {
    return keyed(party_key, "bbp-auth", host_nonce, guest_nonce);
}

bool equal32(const std::uint8_t* a, const std::uint8_t* b) { return crypto_verify32(a, b) == 0; }

Key session_key(const Key& party_key, const char* label, const Nonce& host_nonce, const Nonce& guest_nonce) {
    return keyed(party_key, label, host_nonce, guest_nonce);
}

void wipe(void* p, std::size_t n) { crypto_wipe(p, n); }

static void counter_nonce(std::uint8_t nonce[24], std::uint64_t c) {
    std::memset(nonce, 0, 24);
    for (int i = 0; i < 8; ++i) nonce[i] = static_cast<std::uint8_t>(c >> (8 * i));
}

void Aead::reset() {
    crypto_wipe(key_.data(), key_.size());
    counter_ = 0;
    ready_ = false;
}

void Aead::seal(std::uint8_t* cipher, std::uint8_t mac[kMacSize], const std::uint8_t* ad, std::size_t ad_size,
                const std::uint8_t* text, std::size_t text_size) {
    std::uint8_t nonce[24];
    counter_nonce(nonce, counter_++);
    crypto_aead_lock(cipher, mac, key_.data(), nonce, ad, ad_size, text, text_size);
}

bool Aead::open(std::uint8_t* text, const std::uint8_t mac[kMacSize], const std::uint8_t* ad, std::size_t ad_size,
                const std::uint8_t* cipher, std::size_t cipher_size) {
    std::uint8_t nonce[24];
    counter_nonce(nonce, counter_);
    if (crypto_aead_unlock(text, mac, key_.data(), nonce, ad, ad_size, cipher, cipher_size) != 0) return false;
    ++counter_;
    return true;
}

Key blake2b256(const void* data, std::size_t n) {
    Key out{};
    crypto_blake2b(out.data(), 32, static_cast<const std::uint8_t*>(data), n);
    return out;
}

}  // namespace party::crypto
