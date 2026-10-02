#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace tc {

using Sha256 = std::array<std::uint8_t, 32>;

// One-shot SHA-256 built on Windows CNG. Create one per thread and reuse it;
// instances are not safe for concurrent use.
class Sha256Hasher {
public:
    Sha256Hasher();
    ~Sha256Hasher();
    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;

    bool valid() const { return alg_ != nullptr; }
    // False when CNG fails; out is zeroed then, which collides with the
    // kUnwrittenChunk sentinel, so callers must never use it as a digest.
    bool hash(const void* data, std::size_t len, Sha256& out);

private:
    void* alg_ = nullptr; // BCRYPT_ALG_HANDLE
};

} // namespace tc
