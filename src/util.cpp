#include "util.h"

#include <cstdio>
#include <cwctype>
#include <format>
#include <mutex>

namespace tc {

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string wide_to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
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
