#pragma once

#include <windows.h>

#include <chrono>
#include <cstdint>
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

std::int64_t filetime_to_i64(const FILETIME& ft);

std::wstring human_bytes(unsigned long long bytes);

// "time spent: <days>d <hh>:<mm>:<ss.ss> or <total_ms>ms"
std::wstring format_time_spent(std::chrono::milliseconds elapsed);

// Rejects UNC paths and network drives; fills err when returning false.
bool check_local_drive(const std::filesystem::path& p, std::wstring& err);

// Thread-safe console output (UTF-8).
void log_info(const std::wstring& msg);
void log_error(const std::wstring& msg);

} // namespace tc
