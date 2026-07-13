#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace tc {

std::wstring utf8_to_wide(std::string_view s);
std::string wide_to_utf8(std::wstring_view w);

// Message text for a Win32 error code, trimmed, with the numeric code appended.
std::wstring win32_error_message(DWORD err);

// Absolute path with the \\?\ prefix so long paths work everywhere.
std::wstring extended_path(const std::filesystem::path& p);

std::wstring human_bytes(unsigned long long bytes);

// Thread-safe console output (UTF-8).
void log_info(const std::wstring& msg);
void log_error(const std::wstring& msg);

} // namespace tc
