#pragma once

#include <filesystem>
#include <string>

namespace tc {

// Best-effort enabling of SeBackupPrivilege, SeRestorePrivilege,
// SeSecurityPrivilege and SeTakeOwnershipPrivilege. Call once at startup.
void enable_backup_privileges();

// True if SeSecurityPrivilege was enabled (needed to copy SACLs).
bool have_security_privilege();

// Copies attributes, owner/group/DACL (and SACL when privileged) and all three
// timestamps from src to dst. Operates on the entry itself, never following
// reparse points. Returns false with `error` set only when nothing metadata-
// related could be applied at all; partial degradation (e.g. no SACL) is
// silent by design.
bool copy_metadata(const std::filesystem::path& src, const std::filesystem::path& dst,
                   bool is_directory, std::wstring& error);

} // namespace tc
