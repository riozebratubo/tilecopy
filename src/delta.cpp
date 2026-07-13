#include "delta.h"

#include "hash.h"
#include "util.h"

#include <windows.h>

#include <format>
#include <vector>

namespace tc {

namespace {

struct HandleCloser {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~HandleCloser() { if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
};

bool fail(DeltaResult& r, const std::wstring& what) {
    r.error = std::format(L"{}: {}", what, win32_error_message(::GetLastError()));
    return false;
}

std::int64_t filetime_to_i64(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<std::int64_t>(u.QuadPart);
}

bool write_at(HANDLE h, std::uint64_t offset, const void* data, DWORD len) {
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) return false;
    DWORD written = 0;
    return ::WriteFile(h, data, len, &written, nullptr) && written == len;
}

} // namespace

DeltaResult delta_copy_file(const std::filesystem::path& src, const std::filesystem::path& dst,
                            FileRecord& record, bool had_record, bool db_only,
                            std::uint64_t chunk_size) {
    DeltaResult res;
    const std::wstring sext = extended_path(src);

    HandleCloser hs{::CreateFileW(sext.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (hs.h == INVALID_HANDLE_VALUE) {
        fail(res, L"cannot open source");
        return res;
    }

    LARGE_INTEGER li{};
    if (!::GetFileSizeEx(hs.h, &li)) {
        fail(res, L"cannot get source size");
        return res;
    }
    const std::uint64_t src_size = static_cast<std::uint64_t>(li.QuadPart);

    FILETIME ft_write{};
    if (!::GetFileTime(hs.h, nullptr, nullptr, &ft_write)) {
        fail(res, L"cannot get source time");
        return res;
    }
    const std::int64_t src_write = filetime_to_i64(ft_write);

    std::wstring dext;
    HandleCloser hd;
    bool full_copy = true;

    if (!db_only) {
        dext = extended_path(dst);
        const DWORD dattrs = ::GetFileAttributesW(dext.c_str());
        const bool dst_is_plain_file =
            dattrs != INVALID_FILE_ATTRIBUTES &&
            !(dattrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));

        // Fast path: source unchanged since the database was built and the
        // destination still has the recorded size — nothing to do.
        if (had_record && dst_is_plain_file && record.source_write_time == src_write &&
            record.file_size == src_size) {
            HandleCloser hprobe{::CreateFileW(dext.c_str(), FILE_READ_ATTRIBUTES,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                              OPEN_EXISTING, 0, nullptr)};
            LARGE_INTEGER dli{};
            if (hprobe.h != INVALID_HANDLE_VALUE && ::GetFileSizeEx(hprobe.h, &dli) &&
                static_cast<std::uint64_t>(dli.QuadPart) == src_size) {
                res.ok = true;
                res.skipped = true;
                res.bytes_skipped = src_size;
                return res;
            }
        }

        if (dst_is_plain_file && (dattrs & FILE_ATTRIBUTE_READONLY))
            ::SetFileAttributesW(dext.c_str(), dattrs & ~FILE_ATTRIBUTE_READONLY);

        // Delta is only trustworthy when the destination matches what the
        // database recorded after the last copy.
        if (had_record && dst_is_plain_file) {
            hd.h = ::CreateFileW(dext.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hd.h != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER dli{};
                if (::GetFileSizeEx(hd.h, &dli) &&
                    static_cast<std::uint64_t>(dli.QuadPart) == record.file_size) {
                    full_copy = false;
                } else {
                    ::CloseHandle(hd.h);
                    hd.h = INVALID_HANDLE_VALUE;
                }
            }
        }

        if (full_copy) {
            hd.h = ::CreateFileW(dext.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hd.h == INVALID_HANDLE_VALUE) {
                fail(res, L"cannot create destination");
                return res;
            }
        }
    }

    thread_local Sha256Hasher hasher;
    if (!hasher.valid()) {
        res.error = L"SHA-256 provider unavailable";
        return res;
    }

    std::vector<std::uint8_t> buf(chunk_size);
    std::vector<Sha256> new_chunks;
    new_chunks.reserve(static_cast<size_t>(src_size / chunk_size + 1));

    std::uint64_t offset = 0;
    while (offset < src_size || (src_size == 0 && offset == 0)) {
        if (src_size == 0) break;
        DWORD read = 0;
        if (!::ReadFile(hs.h, buf.data(), static_cast<DWORD>(chunk_size), &read, nullptr)) {
            fail(res, L"read failed");
            return res;
        }
        if (read == 0) break;

        const Sha256 h = hasher.hash(buf.data(), read);
        const size_t idx = new_chunks.size();
        new_chunks.push_back(h);

        if (!db_only) {
            const bool unchanged =
                !full_copy && idx < record.chunks.size() && record.chunks[idx] == h;
            if (unchanged) {
                res.bytes_skipped += read;
            } else {
                if (!write_at(hd.h, offset, buf.data(), read)) {
                    fail(res, L"write failed");
                    return res;
                }
                res.bytes_written += read;
            }
        }
        offset += read;
    }

    if (!db_only) {
        // Truncate (or confirm) the destination length; matters when the
        // source shrank under delta mode.
        LARGE_INTEGER endpos;
        endpos.QuadPart = static_cast<LONGLONG>(offset);
        if (!::SetFilePointerEx(hd.h, endpos, nullptr, FILE_BEGIN) || !::SetEndOfFile(hd.h)) {
            fail(res, L"cannot set destination size");
            return res;
        }
        res.delta_used = !full_copy;
    }

    record.file_size = offset;
    record.source_write_time = src_write;
    record.chunks = std::move(new_chunks);
    res.ok = true;
    return res;
}

} // namespace tc
