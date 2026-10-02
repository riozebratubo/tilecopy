#include "util.h"

#include <cstdio>
#include <cwctype>
#include <format>
#include <mutex>

namespace tc {

// Hand-rolled WTF-8 instead of the CP_UTF8 conversions: NTFS names may hold
// unpaired surrogates, which WideCharToMultiByte flattens to U+FFFD, so such
// names would never round-trip through the database keys and the files were
// recopied every run. Here a lone surrogate is encoded as its own three-byte
// sequence and decoded back to the identical UTF-16 unit; valid input
// produces byte-identical standard UTF-8.
std::string wide_to_utf8(std::wstring_view w) {
    std::string out;
    out.reserve(w.size() * 3);
    for (size_t i = 0; i < w.size(); ++i) {
        std::uint32_t cp = w[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size() && w[i + 1] >= 0xDC00 &&
            w[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) { // unpaired surrogates land here as-is
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::wstring utf8_to_wide(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const std::uint8_t b = static_cast<std::uint8_t>(s[i]);
        std::uint32_t cp = 0;
        size_t len = 0;
        if (b < 0x80) { cp = b; len = 1; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1Fu; len = 2; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0Fu; len = 3; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07u; len = 4; }
        // Malformed bytes become U+FFFD one at a time, matching what the
        // CP_UTF8 conversion used to do with invalid input.
        bool ok = len != 0 && i + len <= s.size();
        for (size_t k = 1; ok && k < len; ++k) {
            const std::uint8_t c = static_cast<std::uint8_t>(s[i + k]);
            if ((c & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (c & 0x3Fu);
        }
        if (!ok || cp > 0x10FFFF) {
            out.push_back(L'\xFFFD');
            ++i;
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else { // surrogate-range values pass through: the WTF-8 round trip
            out.push_back(static_cast<wchar_t>(cp));
        }
        i += len;
    }
    return out;
}

std::wstring win32_error_message(DWORD err) {
    wchar_t* buf = nullptr;
    const DWORD len = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring msg;
    if (len && buf) {
        msg.assign(buf, len);
        ::LocalFree(buf);
        while (!msg.empty() && (std::iswspace(msg.back()) || msg.back() == L'.'))
            msg.pop_back();
    } else {
        msg = L"unknown error";
    }
    return std::format(L"{} (code {})", msg, err);
}

std::wstring extended_path(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec);
    if (ec) abs = p;
    std::wstring native = abs.lexically_normal().native();
    // lexically_normal keeps a trailing separator only for root paths; strip a
    // non-root trailing backslash so string comparisons stay consistent.
    if (native.size() > 3 && native.back() == L'\\') native.pop_back();
    if (native.starts_with(LR"(\\?\)")) return native;
    if (native.starts_with(LR"(\\)")) return LR"(\\?\UNC\)" + native.substr(2);
    return LR"(\\?\)" + native;
}

std::int64_t filetime_to_i64(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<std::int64_t>(u.QuadPart);
}

std::wstring human_bytes(unsigned long long bytes) {
    constexpr const wchar_t* units[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    if (u == 0) return std::format(L"{} B", bytes);
    return std::format(L"{:.1f} {}", v, units[u]);
}

std::wstring format_time_spent(std::chrono::milliseconds elapsed) {
    const auto total_ms = elapsed.count();
    const auto days = total_ms / 86'400'000;
    const auto hours = (total_ms / 3'600'000) % 24;
    const auto minutes = (total_ms / 60'000) % 60;
    const double seconds = (total_ms % 60'000) / 1000.0;
    return std::format(L"time spent: {}d {:02}:{:02}:{:05.2f} or {}ms",
                       days, hours, minutes, seconds, total_ms);
}

bool check_local_drive(const std::filesystem::path& p, std::wstring& err) {
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(p, ec);
    const std::wstring root = abs.root_path().native();
    if (root.starts_with(L"\\\\")) {
        err = std::format(L"{}: UNC/network paths are not supported", p.native());
        return false;
    }
    switch (::GetDriveTypeW(root.c_str())) {
    case DRIVE_REMOTE:
        err = std::format(L"{}: network drives are not supported", p.native());
        return false;
    case DRIVE_UNKNOWN:
    case DRIVE_NO_ROOT_DIR:
        err = std::format(L"{}: not a valid local drive", p.native());
        return false;
    default:
        return true;
    }
}

namespace {
std::mutex g_log_mutex;

void write_line(FILE* stream, const std::wstring& msg) {
    const std::string utf8 = wide_to_utf8(msg) + "\n";
    std::lock_guard lock(g_log_mutex);
    std::fwrite(utf8.data(), 1, utf8.size(), stream);
    std::fflush(stream);
}
} // namespace

void log_info(const std::wstring& msg) { write_line(stdout, msg); }
void log_error(const std::wstring& msg) { write_line(stderr, L"error: " + msg); }

} // namespace tc
