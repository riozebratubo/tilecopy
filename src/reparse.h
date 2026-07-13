#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace tc {

inline bool is_reparse_point(DWORD attrs) {
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// Recreates the reparse point src at dst (symlink, junction, or any other tag),
// replacing whatever exists at dst. Never touches the link target.
bool clone_reparse_point(const std::filesystem::path& src, const std::filesystem::path& dst,
                         bool is_directory, std::wstring& error);

// Deletes a single reparse point entry (the link itself, not its target).
bool delete_reparse_point(const std::filesystem::path& p, bool is_directory);

} // namespace tc
