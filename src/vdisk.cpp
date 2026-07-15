#include "vdisk.h"

#include "util.h"

#include <windows.h>

#include <initguid.h> // materialize VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT here
#include <virtdisk.h>
#include <winioctl.h>

#include <format>

namespace tc {

namespace {

bool fail(std::wstring& error, const std::wstring& what, DWORD err) {
    error = std::format(L"{}: {}", what, win32_error_message(err));
    return false;
}

} // namespace

VhdxDisk::~VhdxDisk() { detach(); }

bool VhdxDisk::create(const std::filesystem::path& file, std::uint64_t virtual_size,
                      std::uint32_t sector_size, std::wstring& error) {
    VIRTUAL_STORAGE_TYPE stype{VIRTUAL_STORAGE_TYPE_DEVICE_VHDX,
                               VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};
    CREATE_VIRTUAL_DISK_PARAMETERS params{};
    params.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    params.Version2.MaximumSize = virtual_size;
    params.Version2.BlockSizeInBytes = 1u << 20;
    params.Version2.SectorSizeInBytes = sector_size;
    params.Version2.PhysicalSectorSizeInBytes = 4096;

    HANDLE h = INVALID_HANDLE_VALUE;
    const DWORD rc = ::CreateVirtualDisk(&stype, file.c_str(), VIRTUAL_DISK_ACCESS_NONE,
                                         nullptr, CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &params,
                                         nullptr, &h);
    if (rc != ERROR_SUCCESS)
        return fail(error, std::format(L"cannot create {}", file.native()), rc);
    vhd_ = h;
    return true;
}

bool VhdxDisk::open(const std::filesystem::path& file, std::wstring& error) {
    VIRTUAL_STORAGE_TYPE stype{VIRTUAL_STORAGE_TYPE_DEVICE_VHDX,
                               VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};
    OPEN_VIRTUAL_DISK_PARAMETERS params{};
    params.Version = OPEN_VIRTUAL_DISK_VERSION_2;
    params.Version2.GetInfoOnly = FALSE;
    params.Version2.ReadOnly = FALSE;

    HANDLE h = INVALID_HANDLE_VALUE;
    const DWORD rc = ::OpenVirtualDisk(&stype, file.c_str(), VIRTUAL_DISK_ACCESS_NONE,
                                       OPEN_VIRTUAL_DISK_FLAG_NONE, &params, &h);
    if (rc != ERROR_SUCCESS)
        return fail(error, std::format(L"cannot open {}", file.native()), rc);
    vhd_ = h;
    return true;
}

bool VhdxDisk::try_attach(bool non_pnp, std::wstring& error) {
    // Not in every SDK's virtdisk.h: attaches the disk outside the PnP
    // stack, so no volume ever surfaces from the image.
    constexpr auto kNonPnp = static_cast<ATTACH_VIRTUAL_DISK_FLAG>(0x00000040);

    ATTACH_VIRTUAL_DISK_PARAMETERS params{};
    params.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
    const ATTACH_VIRTUAL_DISK_FLAG flags =
        non_pnp ? kNonPnp : ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER;
    const DWORD rc = ::AttachVirtualDisk(vhd_, nullptr, flags, 0, &params, nullptr);
    if (rc != ERROR_SUCCESS) return fail(error, L"cannot attach virtual disk", rc);
    const auto undo = [&] {
        ::DetachVirtualDisk(vhd_, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        phys_path_.clear();
    };

    wchar_t buf[MAX_PATH]{};
    ULONG size = sizeof(buf);
    const DWORD prc = ::GetVirtualDiskPhysicalPath(vhd_, &size, buf);
    if (prc != ERROR_SUCCESS) {
        undo();
        return fail(error, L"cannot get attached disk path", prc);
    }
    phys_path_ = buf;

    // The disk device can take a moment to accept opens after attach.
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < (non_pnp ? 20 : 100); ++attempt) {
        h = ::CreateFileW(phys_path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                          nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        ::Sleep(100);
    }
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD oerr = ::GetLastError();
        fail(error, std::format(L"cannot open attached disk {}", phys_path_), oerr);
        undo();
        return false;
    }
    disk_ = h;
    if (non_pnp) return true;

    // PnP fallback. Offline for this session only: kills any automounted
    // volume objects so raw writes are not blocked, and keeps Windows from
    // replaying the NTFS log inside the image while it is being rewritten. A
    // later attach by the user (Explorer, Mount-DiskImage) comes up online
    // as usual. Anything that grabbed the image's volume in the race before
    // the offline can still hold the eventual detach up for minutes.
    SET_DISK_ATTRIBUTES attrs{};
    attrs.Version = sizeof(attrs);
    attrs.Persist = FALSE;
    attrs.Attributes = DISK_ATTRIBUTE_OFFLINE;
    attrs.AttributesMask = DISK_ATTRIBUTE_OFFLINE;
    DWORD br = 0;
    if (!::DeviceIoControl(disk_, IOCTL_DISK_SET_DISK_ATTRIBUTES, &attrs, sizeof(attrs),
                           nullptr, 0, &br, nullptr)) {
        fail(error, L"cannot take the attached disk offline", ::GetLastError());
        ::CloseHandle(disk_);
        disk_ = nullptr;
        undo();
        return false;
    }
    ::DeviceIoControl(disk_, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &br,
                      nullptr);
    return true;
}

// Non-PnP first, and not merely as an optimization. With a PnP attach of an
// image that already contains a file system (i.e. every incremental run),
// Windows mounts the volume in the instant between the attach and the
// offline IOCTL, system services open handles into it, and DetachVirtualDisk
// then waits out their PnP removal timeouts - measured at ~3 minutes on a
// copy whose actual I/O took 20 seconds. A freshly created image is blank at
// attach time (its partition table is only written later, while offline), so
// the stall never shows on first runs; do not let that pass mislead you.
bool VhdxDisk::attach(std::wstring& error) {
    std::wstring non_pnp_err;
    if (try_attach(true, non_pnp_err)) return true;
    log_info(L"note: non-PnP attach unavailable (" + non_pnp_err +
             L"); attaching through PnP - detaching may take a while");
    return try_attach(false, error);
}

void* VhdxDisk::open_raw(std::wstring& error) const {
    HANDLE h = ::CreateFileW(phys_path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE)
        fail(error, std::format(L"cannot open attached disk {}", phys_path_),
             ::GetLastError());
    return h;
}

bool VhdxDisk::flush(std::wstring& error) {
    if (disk_ && !::FlushFileBuffers(disk_))
        return fail(error, L"cannot flush the attached disk", ::GetLastError());
    return true;
}

void VhdxDisk::detach() {
    if (disk_) {
        ::CloseHandle(disk_);
        disk_ = nullptr;
    }
    if (vhd_) {
        ::DetachVirtualDisk(vhd_, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        ::CloseHandle(vhd_);
        vhd_ = nullptr;
    }
    phys_path_.clear();
}

} // namespace tc
