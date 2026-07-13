#include "reparse.h"

#include "util.h"

#include <winioctl.h>

#include <format>
#include <vector>

#ifndef SYMLINK_FLAG_RELATIVE
#define SYMLINK_FLAG_RELATIVE 0x00000001
#endif

namespace tc {

namespace {

// User-mode SDK headers do not define this (it lives in ntifs.h).
struct TcReparseDataBuffer {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    union {
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            ULONG Flags;
            WCHAR PathBuffer[1];
        } SymbolicLink;
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            WCHAR PathBuffer[1];
        } MountPoint;
        struct {
            UCHAR DataBuffer[1];
        } Generic;
    };
};

constexpr DWORD kMaxReparseSize = 16 * 1024; // MAXIMUM_REPARSE_DATA_BUFFER_SIZE

bool set_error(std::wstring& error, const wchar_t* what) {
    error = std::format(L"{}: {}", what, win32_error_message(::GetLastError()));
    return false;
}

std::wstring symlink_target(const TcReparseDataBuffer* rdb) {
    const auto& sl = rdb->SymbolicLink;
    const wchar_t* base = sl.PathBuffer;
    std::wstring sub(base + sl.SubstituteNameOffset / sizeof(WCHAR),
                     sl.SubstituteNameLength / sizeof(WCHAR));
    if (!(sl.Flags & SYMLINK_FLAG_RELATIVE) && sub.starts_with(LR"(\??\)"))
        sub.erase(0, 4);
    return sub;
}

} // namespace

bool delete_reparse_point(const std::filesystem::path& p, bool is_directory) {
    const std::wstring ext = extended_path(p);
    ::SetFileAttributesW(ext.c_str(), FILE_ATTRIBUTE_NORMAL);
    return is_directory ? ::RemoveDirectoryW(ext.c_str()) != 0
                        : ::DeleteFileW(ext.c_str()) != 0;
}

bool clone_reparse_point(const std::filesystem::path& src, const std::filesystem::path& dst,
                         bool is_directory, std::wstring& error) {
    const std::wstring sext = extended_path(src);
    const std::wstring dext = extended_path(dst);

    HANDLE hs = ::CreateFileW(sext.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (hs == INVALID_HANDLE_VALUE) return set_error(error, L"cannot open link");

    std::vector<std::uint8_t> buf(kMaxReparseSize);
    DWORD returned = 0;
    const BOOL got = ::DeviceIoControl(hs, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf.data(),
                                       static_cast<DWORD>(buf.size()), &returned, nullptr);
    ::CloseHandle(hs);
    if (!got) return set_error(error, L"cannot read reparse data");

    const auto* rdb = reinterpret_cast<const TcReparseDataBuffer*>(buf.data());

    // Replace whatever currently sits at the destination.
    const DWORD dattrs = ::GetFileAttributesW(dext.c_str());
    if (dattrs != INVALID_FILE_ATTRIBUTES) {
        if (dattrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            delete_reparse_point(dst, (dattrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
        } else if (dattrs & FILE_ATTRIBUTE_DIRECTORY) {
            std::error_code ec;
            std::filesystem::remove_all(dst, ec);
        } else {
            ::SetFileAttributesW(dext.c_str(), FILE_ATTRIBUTE_NORMAL);
            ::DeleteFileW(dext.c_str());
        }
    }

    if (rdb->ReparseTag == IO_REPARSE_TAG_SYMLINK) {
        const std::wstring target = symlink_target(rdb);
        DWORD flags = (is_directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0u) |
                      SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
        if (!::CreateSymbolicLinkW(dst.c_str(), target.c_str(), flags)) {
            // Pre-1703 Windows 10 rejects the unprivileged-create flag outright.
            flags &= ~SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
            if (!::CreateSymbolicLinkW(dst.c_str(), target.c_str(), flags))
                return set_error(error, L"cannot create symbolic link (needs admin or Developer Mode)");
        }
        return true;
    }

    // Junctions and any other tag: create a placeholder and stamp the raw
    // reparse buffer onto it.
    if (is_directory) {
        if (!::CreateDirectoryW(dext.c_str(), nullptr) &&
            ::GetLastError() != ERROR_ALREADY_EXISTS)
            return set_error(error, L"cannot create junction directory");
    } else {
        HANDLE hf = ::CreateFileW(dext.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf == INVALID_HANDLE_VALUE) return set_error(error, L"cannot create link placeholder");
        ::CloseHandle(hf);
    }

    HANDLE hd = ::CreateFileW(dext.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (hd == INVALID_HANDLE_VALUE) return set_error(error, L"cannot open link placeholder");

    DWORD unused = 0;
    const BOOL set = ::DeviceIoControl(hd, FSCTL_SET_REPARSE_POINT, buf.data(), returned,
                                       nullptr, 0, &unused, nullptr);
    ::CloseHandle(hd);
    if (!set) {
        // Do not leave a plain placeholder behind pretending to be a link.
        if (is_directory) ::RemoveDirectoryW(dext.c_str());
        else ::DeleteFileW(dext.c_str());
        return set_error(error, L"cannot set reparse data");
    }
    return true;
}

} // namespace tc
