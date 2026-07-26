#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace tc {

// Raw disk and volume access shared by the image modes (--drive/--partition
// to a .vhdx) and the restore mode (--restore back to a device): sector-aligned
// I/O, device geometry, and the volume-to-disk lookups both need.

struct HandleCloser {
    HANDLE h = INVALID_HANDLE_VALUE;
    HandleCloser() = default;
    explicit HandleCloser(HANDLE hh) : h(hh) {}
    HandleCloser(const HandleCloser&) = delete;
    HandleCloser& operator=(const HandleCloser&) = delete;
    HandleCloser(HandleCloser&& o) noexcept : h(std::exchange(o.h, INVALID_HANDLE_VALUE)) {}
    HandleCloser& operator=(HandleCloser&& o) noexcept {
        if (this != &o) {
            if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
            h = std::exchange(o.h, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    ~HandleCloser() {
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    }
};

// Page-aligned buffer: FILE_FLAG_NO_BUFFERING needs sector-aligned memory.
struct AlignedBuf {
    std::uint8_t* p = nullptr;
    explicit AlignedBuf(std::size_t n) {
        p = static_cast<std::uint8_t*>(
            ::VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }
    AlignedBuf(const AlignedBuf&) = delete;
    AlignedBuf& operator=(const AlignedBuf&) = delete;
    ~AlignedBuf() {
        if (p) ::VirtualFree(p, 0, MEM_RELEASE);
    }
};

inline std::uint64_t round_up(std::uint64_t v, std::uint64_t a) { return (v + a - 1) / a * a; }

// Positional I/O; both demand the exact length, so a short transfer counts as
// a failure (device handles never return partial reads for valid ranges).
bool read_at(HANDLE h, std::uint64_t off, void* p, DWORD len);
bool write_at(HANDLE h, std::uint64_t off, const void* p, DWORD len);

bool is_elevated();

// Opens a device path (\\.\PhysicalDriveN, \\.\X:, \\?\Volume{...}); on
// failure fills error and returns INVALID_HANDLE_VALUE.
HANDLE open_device(const std::wstring& dev, DWORD access, std::wstring& error, DWORD flags = 0);

// Volume handles clamp I/O at the file-system size, which sits a few sectors
// short of the partition end (the backup boot sector lives there); this lifts
// the clamp to the full partition. Harmless on non-volume handles.
void allow_extended_dasd(HANDLE h);

bool device_length(HANDLE h, std::uint64_t& out);
std::uint32_t disk_sector_size(HANDLE h);

// First disk extent of a volume; single receives whether it is the only one.
bool volume_extent(HANDLE hvol, int& disk, std::uint64_t& off, std::uint64_t& len, bool& single);

// \\?\Volume{...}\ name of the volume mounted at "X:\" (empty if unknown).
std::wstring volume_guid_name(const std::wstring& mount_root);

// Finds the \\?\Volume{...}\ name whose DOS device maps to an NT device name
// like \Device\HarddiskVolume3.
std::wstring guid_name_for_nt_device(const std::wstring& nt_name);

// First DOS path of a volume ("C:\") for friendlier log lines; falls back to
// the GUID name itself.
std::wstring volume_display(const std::wstring& guid_name);

} // namespace tc
