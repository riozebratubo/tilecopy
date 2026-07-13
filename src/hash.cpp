#include "hash.h"

#include <windows.h>
#include <bcrypt.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace tc {

Sha256Hasher::Sha256Hasher() {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (NT_SUCCESS(::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                                 BCRYPT_HASH_REUSABLE_FLAG))) {
        alg_ = alg;
    }
}

Sha256Hasher::~Sha256Hasher() {
    if (alg_) ::BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(alg_), 0);
}

Sha256 Sha256Hasher::hash(const void* data, std::size_t len) {
    Sha256 out{};
    ::BCryptHash(static_cast<BCRYPT_ALG_HANDLE>(alg_), nullptr, 0,
                 static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(len),
                 out.data(), static_cast<ULONG>(out.size()));
    return out;
}

} // namespace tc
