#include "fsmeta.h"

#include "util.h"

#include <windows.h>
#include <aclapi.h>

#include <format>

namespace tc {

namespace {

bool g_security_priv = false;

bool enable_privilege(HANDLE token, const wchar_t* name) {
    LUID luid;
    if (!::LookupPrivilegeValueW(nullptr, name, &luid)) return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    ::AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
    return ::GetLastError() == ERROR_SUCCESS;
}

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE h = INVALID_HANDLE_VALUE) : h_(h) {}
    ~UniqueHandle() { if (valid()) ::CloseHandle(h_); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    bool valid() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
    HANDLE get() const { return h_; }

private:
    HANDLE h_;
};

HANDLE open_meta(const std::wstring& path, DWORD access) {
    return ::CreateFileW(path.c_str(), access,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

// Copies owner/group/DACL (and SACL when privileged) via handles so reparse
// points get their own security, not their target's. Returns ERROR_SUCCESS
// or the error of the step that failed.
DWORD copy_security(const std::wstring& src, const std::wstring& dst) {
    SECURITY_INFORMATION si =
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    DWORD read_access = READ_CONTROL;
    if (g_security_priv) {
        si |= SACL_SECURITY_INFORMATION;
        read_access |= ACCESS_SYSTEM_SECURITY;
    }

    UniqueHandle hs(open_meta(src, read_access));
    if (!hs.valid()) return ::GetLastError();

    PSID owner = nullptr, group = nullptr;
    PACL dacl = nullptr, sacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD rc =
        ::GetSecurityInfo(hs.get(), SE_FILE_OBJECT, si, &owner, &group, &dacl, &sacl, &sd);
    if (rc != ERROR_SUCCESS) return rc;

    // Try the full set first, then degrade: without SACL, then DACL only.
    struct Attempt { SECURITY_INFORMATION si; DWORD access; };
    const Attempt attempts[] = {
        {si, WRITE_DAC | WRITE_OWNER | (g_security_priv ? ACCESS_SYSTEM_SECURITY : 0u)},
        {OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
         WRITE_DAC | WRITE_OWNER},
        {DACL_SECURITY_INFORMATION, WRITE_DAC},
    };
    for (const Attempt& a : attempts) {
        UniqueHandle hd(open_meta(dst, a.access));
        if (!hd.valid()) {
            rc = ::GetLastError();
            continue;
        }
        rc = ::SetSecurityInfo(hd.get(), SE_FILE_OBJECT, a.si,
                               (a.si & OWNER_SECURITY_INFORMATION) ? owner : nullptr,
                               (a.si & GROUP_SECURITY_INFORMATION) ? group : nullptr,
                               (a.si & DACL_SECURITY_INFORMATION) ? dacl : nullptr,
                               (a.si & SACL_SECURITY_INFORMATION) ? sacl : nullptr);
        if (rc == ERROR_SUCCESS) break;
    }

    ::LocalFree(sd);
    return rc;
}

// Returns ERROR_SUCCESS or the error of the step that failed.
DWORD copy_times(const std::wstring& src, const std::wstring& dst) {
    UniqueHandle hs(open_meta(src, FILE_READ_ATTRIBUTES));
    if (!hs.valid()) return ::GetLastError();
    FILETIME creation, access, write;
    if (!::GetFileTime(hs.get(), &creation, &access, &write)) return ::GetLastError();

    UniqueHandle hd(open_meta(dst, FILE_WRITE_ATTRIBUTES));
    if (!hd.valid()) return ::GetLastError();
    if (!::SetFileTime(hd.get(), &creation, &access, &write)) return ::GetLastError();
    return ERROR_SUCCESS;
}

} // namespace

void enable_backup_privileges() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return;
    enable_privilege(token, SE_BACKUP_NAME);
    enable_privilege(token, SE_RESTORE_NAME);
    enable_privilege(token, SE_TAKE_OWNERSHIP_NAME);
    g_security_priv = enable_privilege(token, SE_SECURITY_NAME);
    ::CloseHandle(token);
}

bool have_security_privilege() { return g_security_priv; }

bool copy_metadata(const std::filesystem::path& src, const std::filesystem::path& dst,
                   bool /*is_directory*/, std::wstring& error) {
    const std::wstring s = extended_path(src);
    const std::wstring d = extended_path(dst);

    // Each step keeps its own error; the message below must name a call that
    // actually failed, not whatever the last (possibly successful) call left
    // in the thread's error slot.
    DWORD last_err = ERROR_SUCCESS;

    const DWORD sec_rc = copy_security(s, d);
    if (sec_rc != ERROR_SUCCESS) last_err = sec_rc;

    bool attr_ok = false;
    const DWORD attrs = ::GetFileAttributesW(s.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        // Reparse/directory bits cannot be assigned; SetFileAttributes ignores
        // what does not apply, so pass the raw value through.
        attr_ok = ::SetFileAttributesW(d.c_str(), attrs) != 0;
        if (!attr_ok) last_err = ::GetLastError();
    } else {
        last_err = ::GetLastError();
    }

    // Timestamps last so the writes above do not disturb them.
    const DWORD time_rc = copy_times(s, d);
    if (time_rc != ERROR_SUCCESS) last_err = time_rc;

    if (sec_rc != ERROR_SUCCESS && !attr_ok && time_rc != ERROR_SUCCESS) {
        error = std::format(L"could not copy any metadata: {}",
                            win32_error_message(last_err));
        return false;
    }
    return true;
}

} // namespace tc
