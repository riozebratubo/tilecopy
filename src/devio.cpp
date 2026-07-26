#include "devio.h"

#include "util.h"

#include <winioctl.h>

#include <cwchar>
#include <format>

namespace tc {

bool read_at(HANDLE h, std::uint64_t off, void* p, DWORD len) {
    OVERLAPPED o{};
    o.Offset = static_cast<DWORD>(off);
    o.OffsetHigh = static_cast<DWORD>(off >> 32);
    DWORD got = 0;
    return ::ReadFile(h, p, len, &got, &o) && got == len;
}

bool write_at(HANDLE h, std::uint64_t off, const void* p, DWORD len) {
    OVERLAPPED o{};
    o.Offset = static_cast<DWORD>(off);
    o.OffsetHigh = static_cast<DWORD>(off >> 32);
    DWORD written = 0;
    return ::WriteFile(h, p, len, &written, &o) && written == len;
}

bool is_elevated() {
    HANDLE tok = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION te{};
    DWORD rl = 0;
    const bool ok =
        ::GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &rl) && te.TokenIsElevated;
    ::CloseHandle(tok);
    return ok;
}

HANDLE open_device(const std::wstring& dev, DWORD access, std::wstring& error, DWORD flags) {
    HANDLE h = ::CreateFileW(dev.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        error = std::format(L"cannot open {}: {}", dev, win32_error_message(::GetLastError()));
    return h;
}

void allow_extended_dasd(HANDLE h) {
    DWORD br = 0;
    ::DeviceIoControl(h, FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0, &br, nullptr);
}

bool device_length(HANDLE h, std::uint64_t& out) {
    GET_LENGTH_INFORMATION gl{};
    DWORD br = 0;
    if (!::DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &gl, sizeof(gl), &br,
                           nullptr))
        return false;
    out = static_cast<std::uint64_t>(gl.Length.QuadPart);
    return true;
}

std::uint32_t disk_sector_size(HANDLE h) {
    DISK_GEOMETRY_EX g{};
    DWORD br = 0;
    if (::DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &g, sizeof(g), &br,
                          nullptr) &&
        g.Geometry.BytesPerSector)
        return g.Geometry.BytesPerSector;
    return 512;
}

bool volume_extent(HANDLE hvol, int& disk, std::uint64_t& off, std::uint64_t& len,
                   bool& single) {
    alignas(8) std::uint8_t buf[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)];
    DWORD br = 0;
    if (!::DeviceIoControl(hvol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf,
                           sizeof(buf), &br, nullptr))
        return false;
    const auto* de = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buf);
    if (de->NumberOfDiskExtents == 0) return false;
    single = de->NumberOfDiskExtents == 1;
    disk = static_cast<int>(de->Extents[0].DiskNumber);
    off = static_cast<std::uint64_t>(de->Extents[0].StartingOffset.QuadPart);
    len = static_cast<std::uint64_t>(de->Extents[0].ExtentLength.QuadPart);
    return true;
}

std::wstring volume_guid_name(const std::wstring& mount_root) { // "X:\" etc.
    wchar_t buf[MAX_PATH]{};
    if (!::GetVolumeNameForVolumeMountPointW(mount_root.c_str(), buf, MAX_PATH)) return {};
    return buf; // volume GUID path with the trailing backslash
}

std::wstring guid_name_for_nt_device(const std::wstring& nt_name) {
    wchar_t vol[MAX_PATH];
    HANDLE f = ::FindFirstVolumeW(vol, MAX_PATH);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    do {
        const std::wstring name = vol; // volume GUID path with the trailing backslash
        if (name.size() < 6) continue;
        const std::wstring qname = name.substr(4, name.size() - 5); // Volume{...}
        wchar_t target[512];
        if (::QueryDosDeviceW(qname.c_str(), target, 512) &&
            _wcsicmp(target, nt_name.c_str()) == 0) {
            found = name;
            break;
        }
    } while (::FindNextVolumeW(f, vol, MAX_PATH));
    ::FindVolumeClose(f);
    return found;
}

std::wstring volume_display(const std::wstring& guid_name) {
    wchar_t buf[512]{};
    DWORD ret = 0;
    if (::GetVolumePathNamesForVolumeNameW(guid_name.c_str(), buf, 512, &ret) && buf[0])
        return buf;
    return guid_name;
}

} // namespace tc
