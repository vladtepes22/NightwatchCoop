// SPDX-License-Identifier: GPL-3.0-or-later
// PartyLink cryptography (monocypher 4, third_party/monocypher):
//   party key   = Argon2i(password || code secret, salt "bbparty1")      (32 bytes, derived once)
//   AUTH proof  = keyed BLAKE2b-256(key = party key, "bbp-auth" || host nonce || guest nonce)
//   session key = keyed BLAKE2b-256(key = party key, label || host nonce || guest nonce),
//                 label "bbp-h2g" (host -> guest) / "bbp-g2h" (guest -> host)
//   frames      = XChaCha20-Poly1305 (crypto_aead_lock), nonce = u64 LE counter || 16 zero bytes,
//                 one counter per direction starting at 0, additional data = the frame header.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace party::crypto {

using Key = std::array<std::uint8_t, 32>;
using Nonce = std::array<std::uint8_t, 24>;
constexpr std::size_t kMacSize = 16;

// Cryptographically random bytes (RtlGenRandom via rand_s on Windows, /dev/urandom elsewhere).
void random_bytes(void* out, std::size_t n);

// Argon2i, 8 MiB, 3 passes (~20-40 ms): run once per PartyLink start, never per connection.
Key derive_party_key(const std::string& password, const std::uint8_t secret[8]);

Key auth_proof(const Key& party_key, const Nonce& host_nonce, const Nonce& guest_nonce);
// Constant-time equality of two 32-byte values.
bool equal32(const std::uint8_t* a, const std::uint8_t* b);
Key session_key(const Key& party_key, const char* label, const Nonce& host_nonce, const Nonce& guest_nonce);
void wipe(void* p, std::size_t n);

// One direction of an encrypted stream: a key and its message counter. Both ends advance the
// counter on every frame, so the nonce is never sent and never repeats under one key.
class Aead {
public:
    void init(const Key& key) {
        key_ = key;
        counter_ = 0;
        ready_ = true;
    }
    void reset();
    bool ready() const { return ready_; }
    std::uint64_t counter() const { return counter_; }
    // cipher = encrypted text (text_size bytes), mac = 16 bytes.
    void seal(std::uint8_t* cipher, std::uint8_t mac[kMacSize], const std::uint8_t* ad, std::size_t ad_size,
              const std::uint8_t* text, std::size_t text_size);
    // False (and the counter unchanged) when the frame is forged or out of order.
    bool open(std::uint8_t* text, const std::uint8_t mac[kMacSize], const std::uint8_t* ad, std::size_t ad_size,
              const std::uint8_t* cipher, std::size_t cipher_size);
    ~Aead() { reset(); }

private:
    Key key_{};
    std::uint64_t counter_ = 0;
    bool ready_ = false;
};

// SHA-256 is not in monocypher; callers hash the eboot themselves. BLAKE2b-256 for anything else
// (e.g. a patches/mods digest built from file names + contents).
Key blake2b256(const void* data, std::size_t n);

}  // namespace party::crypto
