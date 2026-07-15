#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace tc {

// A dynamic VHDX opened through the Virtual Disk API and attached as a raw
// disk. Attaching is done outside the PnP stack (non-PnP) where Windows
// supports it, so the image's file system can never be mounted while chunks
// are written and detaching is instant; older systems fall back to a PnP
// attach with no drive letters and the disk taken offline for the session.
// Detaching happens on destruction; non-attached use is harmless.
class VhdxDisk {
public:
    VhdxDisk() = default;
    ~VhdxDisk();
    VhdxDisk(const VhdxDisk&) = delete;
    VhdxDisk& operator=(const VhdxDisk&) = delete;

    // Creates a new dynamic VHDX. virtual_size must be a multiple of
    // sector_size (512 or 4096); the block size is 1 MiB so unwritten chunks
    // stay unallocated at a fine granularity.
    bool create(const std::filesystem::path& file, std::uint64_t virtual_size,
                std::uint32_t sector_size, std::wstring& error);

    // Opens an existing VHDX read/write.
    bool open(const std::filesystem::path& file, std::wstring& error);

    // Attaches (non-PnP first, PnP fallback), waits for the disk device and
    // opens a raw read/write handle on it.
    bool attach(std::wstring& error);

    // Raw disk HANDLE valid after attach() (owned by this object).
    void* handle() const { return disk_; }
    const std::wstring& physical_path() const { return phys_path_; }

    // Opens an additional raw handle on the attached disk for per-thread I/O.
    // Returns INVALID_HANDLE_VALUE on failure; the caller closes it.
    void* open_raw(std::wstring& error) const;

    bool flush(std::wstring& error);
    void detach();

private:
    bool try_attach(bool non_pnp, std::wstring& error);

    void* vhd_ = nullptr;  // virtdisk HANDLE
    void* disk_ = nullptr; // raw disk device HANDLE
    std::wstring phys_path_;
};

} // namespace tc
