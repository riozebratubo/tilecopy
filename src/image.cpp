#include "image.h"

#include "chunkdb.h"
#include "hash.h"
#include "util.h"
#include "vdisk.h"
#include "vss.h"

#include <windows.h>

#include <objbase.h>
#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <format>
#include <thread>
#include <utility>
#include <vector>

namespace tc {

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kMiB = 1ull << 20;
// Where the volume data starts inside a --partition image: one aligned MiB
// leaves room for the protective MBR, the GPT header and its entry array.
constexpr std::uint64_t kPartitionDataOffset = kMiB;
constexpr std::uint64_t kGptEntryBytes = 128ull * 128; // 128 entries of 128 bytes
// Allocated runs closer than this are read as one device I/O; the gap bytes
// are zeroed afterwards anyway.
constexpr std::uint64_t kReadMergeGap = 128ull * 1024;

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

std::uint64_t round_up(std::uint64_t v, std::uint64_t a) { return (v + a - 1) / a * a; }

struct ZeroRun {
    std::uint64_t off = 0, len = 0; // volume-relative bytes, forced to zero
};

// A volume that occupies part of the source address space and is read
// through its own device (shadow copy or live volume) instead of raw disk
// sectors, so decrypted views, allocation maps and pagefile zeroing apply.
struct VolumeInfo {
    std::wstring guid_name; // \\?\Volume{...}\ or X:\ (empty when unknown)
    std::wstring read_path; // device the data is read from (no trailing slash)
    std::wstring label;     // for log lines
    std::uint64_t src_offset = 0;
    std::uint64_t length = 0;
    bool snapshotted = false;
    std::uint32_t cluster_size = 0; // 0 = no allocation map (copied fully)
    std::uint64_t cluster_count = 0;
    std::vector<std::uint8_t> bitmap; // bit per cluster, clusters past the end count as allocated
    std::vector<ZeroRun> zero_runs;   // sorted, merged (pagefile family)
};

struct Source {
    bool whole_disk = false;
    int disk_number = -1;
    std::wstring device;    // \\.\PhysicalDriveN, or the volume device
    std::wstring display;   // what the user named
    std::wstring vss_name;  // partition mode: "X:\" or "\\?\Volume{...}\"
    std::wstring guid_name; // partition mode volume GUID path (may be empty)
    std::uint64_t size = 0;
    std::uint32_t sector = 512;
    std::array<std::uint8_t, 16> id{};
    std::vector<VolumeInfo> volumes; // sorted by src_offset
};

HANDLE open_device(const std::wstring& dev, DWORD access, std::wstring& error,
                   DWORD flags = 0) {
    HANDLE h = ::CreateFileW(dev.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        error = std::format(L"cannot open {}: {}", dev, win32_error_message(::GetLastError()));
    return h;
}

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

// Volume handles clamp reads at the NTFS file-system size, which sits a few
// sectors short of the partition end (the backup boot sector lives there);
// this lifts the clamp to the full partition. Harmless on non-volume handles.
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

// First disk extent of a volume; single receives whether it is the only one.
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

// GPT disk GUID or MBR signature; left zeroed for uninitialized disks.
void disk_identity(HANDLE hdisk, std::array<std::uint8_t, 16>& id) {
    std::vector<std::uint8_t> buf(64 * 1024);
    DWORD br = 0;
    if (!::DeviceIoControl(hdisk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, buf.data(),
                           static_cast<DWORD>(buf.size()), &br, nullptr))
        return;
    const auto* dl = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(buf.data());
    if (dl->PartitionStyle == PARTITION_STYLE_GPT)
        std::memcpy(id.data(), &dl->Gpt.DiskId, 16);
    else if (dl->PartitionStyle == PARTITION_STYLE_MBR)
        std::memcpy(id.data(), &dl->Mbr.Signature, 4);
}

std::wstring volume_guid_name(const std::wstring& mount_root) { // "X:\" etc.
    wchar_t buf[MAX_PATH]{};
    if (!::GetVolumeNameForVolumeMountPointW(mount_root.c_str(), buf, MAX_PATH)) return {};
    return buf; // volume GUID path with the trailing backslash
}

// Finds the \\?\Volume{...}\ name whose DOS device maps to an NT device name
// like \Device\HarddiskVolume3.
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

bool id_from_guid_name(const std::wstring& name, std::array<std::uint8_t, 16>& id) {
    const size_t b = name.find(L'{');
    const size_t e = name.find(L'}');
    if (b == std::wstring::npos || e == std::wstring::npos || e <= b) return false;
    GUID g{};
    if (FAILED(::CLSIDFromString(name.substr(b, e - b + 1).c_str(), &g))) return false;
    std::memcpy(id.data(), &g, 16);
    return true;
}

void fallback_id(const std::wstring& s, std::array<std::uint8_t, 16>& id) {
    std::uint64_t h = 14695981039346656037ull;
    for (const wchar_t c : s) {
        h ^= static_cast<std::uint64_t>(std::towlower(c));
        h *= 1099511628211ull;
    }
    std::memcpy(id.data(), &h, 8);
}

// First DOS path of a volume ("C:\") for friendlier log lines.
std::wstring volume_display(const std::wstring& guid_name) {
    wchar_t buf[512]{};
    DWORD ret = 0;
    if (::GetVolumePathNamesForVolumeNameW(guid_name.c_str(), buf, 512, &ret) && buf[0])
        return buf;
    return guid_name;
}

bool resolve_source(const Options& opt, Source& src, std::wstring& error) {
    const std::wstring& s = opt.source.native();

    if (opt.mode == Mode::DriveImage) {
        src.whole_disk = true;
        std::wstring device = s;
        if (s.size() == 3 && s[1] == L':') { // "X:\" - the disk holding that volume
            HandleCloser hv{open_device(std::format(LR"(\\.\{}:)", s[0]), GENERIC_READ, error)};
            if (hv.h == INVALID_HANDLE_VALUE) return false;
            int disk = -1;
            std::uint64_t off = 0, len = 0;
            bool single = true;
            if (!volume_extent(hv.h, disk, off, len, single)) {
                error = std::format(L"cannot find the disk of {}: {}", s,
                                    win32_error_message(::GetLastError()));
                return false;
            }
            if (!single) {
                error = std::format(L"{} spans multiple disks; name one disk directly", s);
                return false;
            }
            device = std::format(LR"(\\.\PhysicalDrive{})", disk);
        }
        src.device = device;
        src.disk_number =
            static_cast<int>(std::wcstol(device.c_str() + wcslen(LR"(\\.\PhysicalDrive)"),
                                         nullptr, 10));
        src.display = device;

        HandleCloser hd{open_device(src.device, GENERIC_READ, error)};
        if (hd.h == INVALID_HANDLE_VALUE) return false;
        if (!device_length(hd.h, src.size)) {
            error = std::format(L"cannot get the size of {}: {}", src.device,
                                win32_error_message(::GetLastError()));
            return false;
        }
        src.sector = disk_sector_size(hd.h);
        disk_identity(hd.h, src.id);
        if (src.id == std::array<std::uint8_t, 16>{}) fallback_id(src.device, src.id);
        return true;
    }

    // --partition
    src.display = s;
    if (s.size() == 3 && s[1] == L':') { // "X:\"
        src.device = std::format(LR"(\\.\{}:)", s[0]);
        src.vss_name = s;
        src.guid_name = volume_guid_name(s);
    } else if (s.size() > 4 && (s[2] == L'?' || s[2] == L'.') && towlower(s[4]) == L'v') {
        // \\?\Volume{...} with or without the trailing backslash
        std::wstring name = s;
        if (name.back() == L'\\') name.pop_back();
        src.device = name;
        src.guid_name = name + L"\\";
        src.vss_name = src.guid_name;
    } else { // \\.\HarddiskVolumeN
        src.device = s;
        const std::wstring nt = L"\\Device\\" + s.substr(4);
        src.guid_name = guid_name_for_nt_device(nt);
        src.vss_name = src.guid_name; // empty means no VSS
    }

    HandleCloser hv{open_device(src.device, GENERIC_READ, error)};
    if (hv.h == INVALID_HANDLE_VALUE) return false;
    if (!device_length(hv.h, src.size)) {
        error = std::format(L"cannot get the size of {}: {}", src.device,
                            win32_error_message(::GetLastError()));
        return false;
    }
    int disk = -1;
    std::uint64_t off = 0, len = 0;
    bool single = true;
    if (volume_extent(hv.h, disk, off, len, single)) {
        src.disk_number = single ? disk : -1;
        HandleCloser hd{::CreateFileW(std::format(LR"(\\.\PhysicalDrive{})", disk).c_str(),
                                      GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, 0, nullptr)};
        if (hd.h != INVALID_HANDLE_VALUE) src.sector = disk_sector_size(hd.h);
    }
    if (src.guid_name.empty() || !id_from_guid_name(src.guid_name, src.id))
        fallback_id(src.device, src.id);
    return true;
}

// Volumes on the source disk that have a mounted file system; those are read
// through their own devices (shadow or live) so BitLocker-decrypted views,
// allocation maps and pagefile zeroing apply. Everything else (unformatted,
// locked, spanned) is copied raw straight from the disk.
void collect_disk_volumes(Source& src) {
    wchar_t vol[MAX_PATH];
    HANDLE f = ::FindFirstVolumeW(vol, MAX_PATH);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = vol; // volume GUID path with the trailing backslash
        const std::wstring dev = name.substr(0, name.size() - 1); // no trailing slash
        HandleCloser hv{::CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, 0, nullptr)};
        if (hv.h == INVALID_HANDLE_VALUE) continue;
        int disk = -1;
        std::uint64_t off = 0, len = 0;
        bool single = true;
        if (!volume_extent(hv.h, disk, off, len, single)) continue;
        if (disk != src.disk_number) continue;
        if (!single) {
            log_info(std::format(L"note: {} spans multiple disks; its extents are copied raw",
                                 volume_display(name)));
            continue;
        }
        if (!::GetVolumeInformationW(name.c_str(), nullptr, 0, nullptr, nullptr, nullptr,
                                     nullptr, 0))
            continue; // no mounted file system: raw disk bytes are the truth
        VolumeInfo v;
        v.guid_name = name;
        v.read_path = dev;
        v.label = volume_display(name);
        v.src_offset = off;
        v.length = len;
        src.volumes.push_back(std::move(v));
    } while (::FindNextVolumeW(f, vol, MAX_PATH));
    ::FindVolumeClose(f);
    std::ranges::sort(src.volumes,
                      [](const VolumeInfo& a, const VolumeInfo& b) {
                          return a.src_offset < b.src_offset;
                      });
}

// Cluster geometry comes from the NTFS-specific FSCTL when the volume is
// NTFS, else (FAT32/exFAT) from GetDiskFreeSpace on the original volume
// root. The bitmap FSCTL below is the defrag interface, which all of them
// serve; file systems that don't (e.g. FAT12/16) fall back to a full copy.
bool load_bitmap(HANDLE h, VolumeInfo& v) {
    std::uint32_t cluster_size = 0;
    std::uint64_t clusters = 0;
    NTFS_VOLUME_DATA_BUFFER nd{};
    DWORD br = 0;
    if (::DeviceIoControl(h, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &nd, sizeof(nd), &br,
                          nullptr) &&
        nd.BytesPerCluster != 0 && nd.TotalClusters.QuadPart > 0) {
        cluster_size = nd.BytesPerCluster;
        clusters = static_cast<std::uint64_t>(nd.TotalClusters.QuadPart);
    } else if (!v.guid_name.empty()) {
        std::wstring root = v.guid_name;
        if (root.back() != L'\\') root += L'\\';
        DWORD spc = 0, bps = 0, free_c = 0, total_c = 0;
        if (::GetDiskFreeSpaceW(root.c_str(), &spc, &bps, &free_c, &total_c) && spc && bps &&
            total_c) {
            cluster_size = spc * bps;
            clusters = total_c;
        }
    }
    if (cluster_size == 0 || clusters == 0) return false;
    // A map that claims more clusters than the volume holds would turn the
    // EOF tail handling off for real read errors; clamp it.
    clusters = std::min(clusters, v.length / cluster_size);
    if (clusters == 0) return false;

    std::vector<std::uint8_t> bm((clusters + 7) / 8, 0xFF);
    std::vector<std::uint8_t> out(sizeof(VOLUME_BITMAP_BUFFER) + (1u << 20));
    std::uint64_t lcn = 0;
    while (lcn < clusters) {
        STARTING_LCN_INPUT_BUFFER in{};
        in.StartingLcn.QuadPart = static_cast<LONGLONG>(lcn);
        const BOOL ok = ::DeviceIoControl(h, FSCTL_GET_VOLUME_BITMAP, &in, sizeof(in),
                                          out.data(), static_cast<DWORD>(out.size()), &br,
                                          nullptr);
        if (!ok && ::GetLastError() != ERROR_MORE_DATA) return false;
        const auto* vb = reinterpret_cast<const VOLUME_BITMAP_BUFFER*>(out.data());
        const std::uint64_t start = static_cast<std::uint64_t>(vb->StartingLcn.QuadPart);
        if (br <= FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer) || start >= clusters) return false;
        std::uint64_t bits =
            std::min<std::uint64_t>((br - FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer)) * 8ull,
                                    static_cast<std::uint64_t>(vb->BitmapSize.QuadPart));
        bits = std::min(bits, clusters - start);
        if (bits == 0) return false;
        std::memcpy(bm.data() + start / 8, vb->Buffer, static_cast<size_t>((bits + 7) / 8));
        if (ok || start + bits >= clusters) break;
        if ((bits & ~7ull) == 0) return false;
        lcn = start + (bits & ~7ull);
    }
    v.bitmap = std::move(bm);
    v.cluster_size = cluster_size;
    v.cluster_count = clusters;
    return true;
}

// The pagefile family carries no data worth keeping; record its extents so
// those clusters read as zeros (VSS does not protect them anyway, so their
// content would churn every run).
void load_zero_runs(VolumeInfo& v) {
    if (v.cluster_size == 0) return;
    for (const wchar_t* name : {L"pagefile.sys", L"hiberfil.sys", L"swapfile.sys"}) {
        const std::wstring path = v.read_path + L"\\" + name;
        HandleCloser f{::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                                     nullptr)};
        if (f.h == INVALID_HANDLE_VALUE) continue;
        STARTING_VCN_INPUT_BUFFER in{};
        std::vector<std::uint8_t> out(64 * 1024);
        for (;;) {
            DWORD br = 0;
            const BOOL ok =
                ::DeviceIoControl(f.h, FSCTL_GET_RETRIEVAL_POINTERS, &in, sizeof(in),
                                  out.data(), static_cast<DWORD>(out.size()), &br, nullptr);
            if (!ok && ::GetLastError() != ERROR_MORE_DATA) break;
            const auto* rp = reinterpret_cast<const RETRIEVAL_POINTERS_BUFFER*>(out.data());
            LONGLONG vcn = rp->StartingVcn.QuadPart;
            for (DWORD i = 0; i < rp->ExtentCount; ++i) {
                const LONGLONG next = rp->Extents[i].NextVcn.QuadPart;
                const LONGLONG lcn = rp->Extents[i].Lcn.QuadPart;
                if (lcn >= 0 && next > vcn)
                    v.zero_runs.push_back(
                        {static_cast<std::uint64_t>(lcn) * v.cluster_size,
                         static_cast<std::uint64_t>(next - vcn) * v.cluster_size});
                vcn = next;
            }
            if (ok) break;
            in.StartingVcn.QuadPart = vcn;
        }
    }
    std::ranges::sort(v.zero_runs,
                      [](const ZeroRun& a, const ZeroRun& b) { return a.off < b.off; });
    // merge overlaps and clamp to the volume
    std::vector<ZeroRun> merged;
    for (const auto& z : v.zero_runs) {
        if (z.off >= v.length) continue;
        const std::uint64_t end = std::min(z.off + z.len, v.length);
        if (!merged.empty() && z.off <= merged.back().off + merged.back().len) {
            merged.back().len = std::max(merged.back().off + merged.back().len, end) -
                                merged.back().off;
        } else {
            merged.push_back({z.off, end - z.off});
        }
    }
    v.zero_runs = std::move(merged);
}

std::uint64_t bitmap_allocated_bytes(const VolumeInfo& v) {
    std::uint64_t bits = 0;
    const std::uint64_t full = v.cluster_count / 8;
    for (std::uint64_t i = 0; i < full; ++i) bits += std::popcount(v.bitmap[i]);
    for (std::uint64_t c = full * 8; c < v.cluster_count; ++c)
        bits += (v.bitmap[c >> 3] >> (c & 7)) & 1;
    return bits * v.cluster_size;
}

// Snapshots what it can, then loads allocation maps and pagefile extents
// from whatever device each volume will actually be read from.
void setup_volumes(Source& src, const Options& opt, VssSnapshotSet& vss) {
    if (src.whole_disk) {
        collect_disk_volumes(src);
    } else {
        VolumeInfo v;
        v.guid_name = src.guid_name.empty() ? src.vss_name : src.guid_name;
        v.read_path = src.device;
        v.label = src.display;
        v.src_offset = 0;
        v.length = src.size;
        // Only a mounted file system can be snapshotted or mapped; a locked
        // or unformatted volume is copied raw in full.
        std::wstring root = v.guid_name;
        if (!root.empty() && root.back() != L'\\') root += L'\\';
        if (root.empty() ||
            !::GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr,
                                     nullptr, 0))
            v.guid_name.clear();
        src.volumes.push_back(std::move(v));
    }

    std::vector<std::wstring> names;
    std::vector<size_t> which;
    for (size_t i = 0; i < src.volumes.size(); ++i) {
        if (src.volumes[i].guid_name.empty()) continue;
        std::wstring root = src.volumes[i].guid_name;
        if (root.back() != L'\\') root += L'\\';
        names.push_back(std::move(root));
        which.push_back(i);
    }
    if (!names.empty()) {
        std::vector<std::wstring> devices;
        std::wstring why;
        if (vss.create(names, devices, why)) {
            for (size_t j = 0; j < which.size(); ++j) {
                VolumeInfo& v = src.volumes[which[j]];
                if (!devices[j].empty()) {
                    v.read_path = devices[j];
                    v.snapshotted = true;
                } else {
                    log_info(std::format(L"vss: {} could not be snapshotted; reading it live",
                                         v.label));
                }
            }
        } else {
            log_info(L"vss: snapshot failed (" + why + L"); reading volumes live");
        }
    }

    for (VolumeInfo& v : src.volumes) {
        std::wstring err;
        HandleCloser h{open_device(v.read_path, GENERIC_READ, err)};
        if (h.h != INVALID_HANDLE_VALUE) allow_extended_dasd(h.h);
        if (h.h != INVALID_HANDLE_VALUE && load_bitmap(h.h, v)) load_zero_runs(v);
        if (opt.file_logs) {
            std::wstring line = std::format(L"volume   {} ({})", v.label,
                                            v.snapshotted ? L"snapshot" : L"live");
            if (v.cluster_size) {
                line += std::format(L": {} of {} allocated",
                                    human_bytes(bitmap_allocated_bytes(v)),
                                    human_bytes(v.length));
                std::uint64_t zb = 0;
                for (const auto& z : v.zero_runs) zb += z.len;
                if (zb) line += std::format(L", zeroing {} of pagefiles", human_bytes(zb));
            } else {
                line += L": full copy (no allocation map)";
            }
            log_info(line);
        }
    }
}

// ---------------------------------------------------------------------------
// GPT synthesis for --partition images

#pragma pack(push, 1)
struct GptHeader {
    char signature[8];
    std::uint32_t revision;
    std::uint32_t header_size;
    std::uint32_t header_crc;
    std::uint32_t reserved;
    std::uint64_t my_lba;
    std::uint64_t alternate_lba;
    std::uint64_t first_usable;
    std::uint64_t last_usable;
    std::uint8_t disk_guid[16];
    std::uint64_t entries_lba;
    std::uint32_t entry_count;
    std::uint32_t entry_size;
    std::uint32_t entries_crc;
};
static_assert(sizeof(GptHeader) == 92);

struct GptEntry {
    std::uint8_t type[16];
    std::uint8_t id[16];
    std::uint64_t first_lba;
    std::uint64_t last_lba;
    std::uint64_t attrs;
    wchar_t name[36];
};
static_assert(sizeof(GptEntry) == 128);
#pragma pack(pop)

std::uint32_t crc32(const void* data, std::size_t len) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < len; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Protective MBR + primary and backup GPT with a single basic-data partition,
// so the mounted image surfaces the volume with a drive letter.
bool write_partition_gpt(HANDLE disk, std::uint64_t vsize, std::uint32_t sector,
                         std::uint64_t part_off, std::uint64_t part_len, std::wstring& error) {
    const std::uint64_t total = vsize / sector;
    const std::uint64_t entry_lbas = kGptEntryBytes / sector;

    GUID disk_guid{}, part_guid{};
    ::CoCreateGuid(&disk_guid);
    ::CoCreateGuid(&part_guid);
    static constexpr GUID kBasicData = {
        0xEBD0A0A2, 0xB9E5, 0x4433, {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}};

    std::vector<std::uint8_t> entries(kGptEntryBytes, 0);
    GptEntry e{};
    std::memcpy(e.type, &kBasicData, 16);
    std::memcpy(e.id, &part_guid, 16);
    e.first_lba = part_off / sector;
    e.last_lba = (part_off + part_len) / sector - 1;
    static constexpr wchar_t kName[] = L"tilecopy";
    std::memcpy(e.name, kName, sizeof(kName));
    std::memcpy(entries.data(), &e, sizeof(e));
    const std::uint32_t entries_crc = crc32(entries.data(), entries.size());

    GptHeader h{};
    std::memcpy(h.signature, "EFI PART", 8);
    h.revision = 0x00010000;
    h.header_size = sizeof(GptHeader);
    h.first_usable = 2 + entry_lbas;
    h.last_usable = total - entry_lbas - 2;
    std::memcpy(h.disk_guid, &disk_guid, 16);
    h.entry_count = 128;
    h.entry_size = sizeof(GptEntry);
    h.entries_crc = entries_crc;

    auto fail = [&](const wchar_t* what) {
        error = std::format(L"cannot write the image {}: {}", what,
                            win32_error_message(::GetLastError()));
        return false;
    };
    auto write_header = [&](std::uint64_t my, std::uint64_t alt, std::uint64_t elba,
                            const wchar_t* what) {
        h.my_lba = my;
        h.alternate_lba = alt;
        h.entries_lba = elba;
        h.header_crc = 0;
        h.header_crc = crc32(&h, sizeof(GptHeader));
        std::vector<std::uint8_t> sec(sector, 0);
        std::memcpy(sec.data(), &h, sizeof(h));
        return write_at(disk, my * sector, sec.data(), sector) || fail(what);
    };

    std::vector<std::uint8_t> mbr(sector, 0);
    std::uint8_t* pe = mbr.data() + 446;
    pe[1] = 0x00; pe[2] = 0x02; pe[3] = 0x00; // CHS 0/2/0
    pe[4] = 0xEE;                             // protective GPT
    pe[5] = 0xFF; pe[6] = 0xFF; pe[7] = 0xFF;
    const std::uint32_t start_lba = 1;
    const std::uint32_t mbr_len =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(0xFFFFFFFFull, total - 1));
    std::memcpy(pe + 8, &start_lba, 4);
    std::memcpy(pe + 12, &mbr_len, 4);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    if (!write_at(disk, 0, mbr.data(), sector)) return fail(L"protective MBR");
    if (!write_at(disk, 2 * sector, entries.data(), static_cast<DWORD>(entries.size())))
        return fail(L"partition entries");
    if (!write_at(disk, (total - 1 - entry_lbas) * sector, entries.data(),
                  static_cast<DWORD>(entries.size())))
        return fail(L"backup partition entries");
    if (!write_header(1, total - 1, 2, L"GPT header")) return false;
    if (!write_header(total - 1, 1, total - 1 - entry_lbas, L"backup GPT header"))
        return false;
    return true;
}

// ---------------------------------------------------------------------------
// The copy itself

struct ImageJob {
    const Options* opt = nullptr;
    Source src;
    ChunkDatabase db;
    fs::path db_file;
    std::uint64_t chunk_size = 0;
    std::uint64_t chunk_count = 0;
    Sha256 zero_full{}, zero_tail{};
    std::uint64_t tail_len = 0;
    bool copying = true;
    bool incremental = false;
    std::uint64_t dest_off = 0; // kPartitionDataOffset for --partition images
    const VhdxDisk* vhdx = nullptr;

    std::atomic<std::uint64_t> next{0}, done{0}, failed{0};
    std::atomic<std::uint64_t> bytes_read{0}, bytes_written{0}, bytes_unchanged{0},
        bytes_zero{0};
    // Wall time per phase, in nanoseconds, summed across workers.
    std::atomic<std::uint64_t> ns_setup{0}, ns_read{0}, ns_hash{0}, ns_write{0};
    std::atomic<int> last_pct{0};
    std::atomic<bool> tail_note{false};
};

// Subtracts the sorted zero runs from sorted, non-overlapping byte runs.
void subtract_runs(std::vector<std::pair<std::uint64_t, std::uint64_t>>& runs,
                   const std::vector<ZeroRun>& zeros) {
    if (zeros.empty() || runs.empty()) return;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    out.reserve(runs.size());
    for (const auto& [lo, hi] : runs) {
        std::uint64_t p = lo;
        for (const auto& z : zeros) {
            const std::uint64_t zlo = z.off, zhi = z.off + z.len;
            if (zhi <= p) continue;
            if (zlo >= hi) break;
            if (zlo > p) out.emplace_back(p, std::min(zlo, hi));
            p = std::max(p, zhi);
            if (p >= hi) break;
        }
        if (p < hi) out.emplace_back(p, hi);
    }
    runs = std::move(out);
}

// Fills out with volume bytes [lo, hi): allocated clusters are read from the
// volume's device, everything else (unallocated, pagefile) becomes zeros.
bool compose_volume_piece(const VolumeInfo& v, HANDLE vh, std::uint64_t lo, std::uint64_t hi,
                          std::uint8_t* out, bool& any_read, std::uint64_t& bytes_read,
                          bool& tail_zeroed, std::wstring& error) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> keep;
    if (v.cluster_size == 0) {
        keep.emplace_back(lo, hi);
    } else {
        const std::uint64_t cs = v.cluster_size;
        for (std::uint64_t c = lo / cs; c * cs < hi; ++c) {
            const bool alloc =
                c >= v.cluster_count || ((v.bitmap[c >> 3] >> (c & 7)) & 1) != 0;
            if (!alloc) continue;
            const std::uint64_t rlo = std::max(lo, c * cs);
            const std::uint64_t rhi = std::min(hi, (c + 1) * cs);
            if (!keep.empty() && keep.back().second == rlo)
                keep.back().second = rhi;
            else
                keep.emplace_back(rlo, rhi);
        }
    }
    subtract_runs(keep, v.zero_runs);

    // One device read per group of nearby runs; the gap bytes get zeroed in
    // the pass below, after the read.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
    for (const auto& k : keep) {
        if (!spans.empty() && k.first - spans.back().second <= kReadMergeGap)
            spans.back().second = k.second;
        else
            spans.push_back(k);
    }
    // Bytes past the file-system end (when the cluster map is known) are
    // read separately: some devices refuse them even with extended DASD I/O,
    // in which case they are stored as zeros rather than failing the chunk.
    const std::uint64_t fs_bytes =
        v.cluster_size ? v.cluster_count * v.cluster_size : ~0ull;
    for (const auto& sp : spans) {
        std::uint64_t p = sp.first;
        while (p < sp.second) {
            const std::uint64_t pe =
                p < fs_bytes ? std::min(sp.second, fs_bytes) : sp.second;
            if (!read_at(vh, p, out + (p - lo), static_cast<DWORD>(pe - p))) {
                if (p >= fs_bytes && ::GetLastError() == ERROR_HANDLE_EOF) {
                    std::memset(out + (p - lo), 0, static_cast<size_t>(pe - p));
                    tail_zeroed = true;
                    p = pe;
                    continue;
                }
                error = std::format(L"read {} at offset {}: {}", v.read_path, p,
                                    win32_error_message(::GetLastError()));
                return false;
            }
            bytes_read += pe - p;
            p = pe;
        }
    }
    any_read = any_read || !spans.empty();

    std::uint64_t z = lo;
    for (const auto& k : keep) {
        if (k.first > z) std::memset(out + (z - lo), 0, static_cast<size_t>(k.first - z));
        z = k.second;
    }
    if (hi > z) std::memset(out + (z - lo), 0, static_cast<size_t>(hi - z));
    return true;
}

bool compose_chunk(const ImageJob& job, HANDLE base, const std::vector<HandleCloser>& vols,
                   std::uint64_t c0, std::uint64_t c1, std::uint8_t* buf, bool& any_read,
                   std::uint64_t& bytes_read, bool& tail_zeroed, std::wstring& error) {
    const auto& vv = job.src.volumes;
    std::uint64_t pos = c0;
    while (pos < c1) {
        // Volume containing pos, if any; otherwise the raw region ends at the
        // next volume start.
        const auto it = std::upper_bound(vv.begin(), vv.end(), pos,
                                         [](std::uint64_t p, const VolumeInfo& x) {
                                             return p < x.src_offset;
                                         });
        const VolumeInfo* v = nullptr;
        size_t vi = 0;
        if (it != vv.begin()) {
            const auto pv = std::prev(it);
            if (pos < pv->src_offset + pv->length) {
                v = &*pv;
                vi = static_cast<size_t>(pv - vv.begin());
            }
        }
        if (!v) {
            const std::uint64_t nb = it != vv.end() ? it->src_offset : c1;
            const std::uint64_t pe = std::min(c1, nb);
            if (!read_at(base, pos, buf + (pos - c0), static_cast<DWORD>(pe - pos))) {
                error = std::format(L"read {} at offset {}: {}", job.src.device, pos,
                                    win32_error_message(::GetLastError()));
                return false;
            }
            any_read = true;
            bytes_read += pe - pos;
            pos = pe;
            continue;
        }
        const std::uint64_t pe = std::min(c1, v->src_offset + v->length);
        if (vols[vi].h == INVALID_HANDLE_VALUE) {
            error = std::format(L"{} is not readable", v->read_path);
            return false;
        }
        if (!compose_volume_piece(*v, vols[vi].h, pos - v->src_offset, pe - v->src_offset,
                                  buf + (pos - c0), any_read, bytes_read, tail_zeroed, error))
            return false;
        pos = pe;
    }
    return true;
}

void process_chunk(ImageJob& job, HANDLE base, const std::vector<HandleCloser>& vols,
                   HANDLE dest, std::uint64_t idx, std::uint8_t* buf, Sha256Hasher& hasher) {
    const std::uint64_t c0 = idx * job.chunk_size;
    const std::uint64_t len = std::min(job.chunk_size, job.src.size - c0);
    const auto since = [](std::chrono::steady_clock::time_point t0) {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - t0)
                                              .count());
    };

    std::wstring err;
    bool ok = false;
    bool any_read = false;
    bool tail_zeroed = false;
    std::uint64_t rbytes = 0;
    const auto tr = std::chrono::steady_clock::now();
    for (int attempt = 1; attempt <= job.opt->max_tries && !ok; ++attempt) {
        any_read = false;
        tail_zeroed = false;
        rbytes = 0;
        ok = compose_chunk(job, base, vols, c0, c0 + len, buf, any_read, rbytes, tail_zeroed,
                           err);
        if (!ok && attempt < job.opt->max_tries) ::Sleep(250);
    }
    job.ns_read.fetch_add(since(tr), std::memory_order_relaxed);
    if (!ok) {
        job.failed.fetch_add(1, std::memory_order_relaxed);
        log_error(std::format(L"chunk {} (offset {}): {}", idx, c0, err));
        return;
    }
    job.bytes_read.fetch_add(rbytes, std::memory_order_relaxed);
    if (tail_zeroed && !job.tail_note.exchange(true, std::memory_order_relaxed))
        log_info(L"note: bytes past the NTFS file-system end were unreadable and stored as "
                 L"zeros (backup boot sector area)");

    // A chunk that needed no device read is all zeros by construction; its
    // hash is the precomputed zero hash, no need to hash 1 MiB of zeros.
    const Sha256& zero_hash = len == job.chunk_size ? job.zero_full : job.zero_tail;
    const auto th = std::chrono::steady_clock::now();
    const Sha256 h = any_read ? hasher.hash(buf, static_cast<std::size_t>(len)) : zero_hash;
    job.ns_hash.fetch_add(since(th), std::memory_order_relaxed);
    // Nothing was read, or everything read turned out to be zeros: the chunk
    // can stay (or become) a hole.
    const bool zero = !any_read || h == zero_hash;
    Sha256& slot = job.db.image.chunks[static_cast<size_t>(idx)];

    if (!job.copying) {
        slot = zero ? kUnwrittenChunk : h;
        if (zero) job.bytes_zero.fetch_add(len, std::memory_order_relaxed);
        return;
    }
    if (job.incremental) {
        if (zero && slot == kUnwrittenChunk) { // hole stays a hole
            job.bytes_zero.fetch_add(len, std::memory_order_relaxed);
            return;
        }
        if (h == slot) {
            job.bytes_unchanged.fetch_add(len, std::memory_order_relaxed);
            return;
        }
    } else if (zero) { // fresh VHDX: skipping the write leaves it sparse
        slot = kUnwrittenChunk;
        job.bytes_zero.fetch_add(len, std::memory_order_relaxed);
        return;
    }

    ok = false;
    DWORD werr = 0;
    const auto tw = std::chrono::steady_clock::now();
    for (int attempt = 1; attempt <= job.opt->max_tries && !ok; ++attempt) {
        ok = write_at(dest, job.dest_off + c0, buf, static_cast<DWORD>(len));
        if (!ok) {
            werr = ::GetLastError();
            if (attempt < job.opt->max_tries) ::Sleep(250);
        }
    }
    job.ns_write.fetch_add(since(tw), std::memory_order_relaxed);
    if (!ok) {
        job.failed.fetch_add(1, std::memory_order_relaxed);
        log_error(std::format(L"chunk {} (offset {}): write failed: {}", idx, c0,
                              win32_error_message(werr)));
        return;
    }
    slot = h;
    job.bytes_written.fetch_add(len, std::memory_order_relaxed);
}

void image_worker(ImageJob& job) {
    const auto t_setup = std::chrono::steady_clock::now();
    // Unbuffered reads: imaging never re-reads data, so the file cache only
    // adds copies, evicts other programs' working sets, and makes throughput
    // depend on cache state instead of the device. Every read offset and
    // length here is sector-aligned (cluster, chunk and partition boundaries
    // all are), and the buffer is page-aligned.
    std::wstring err;
    HandleCloser base{open_device(job.src.device, GENERIC_READ, err, FILE_FLAG_NO_BUFFERING)};
    if (base.h == INVALID_HANDLE_VALUE) {
        log_error(err);
        return; // unprocessed chunks are counted as failed afterwards
    }
    allow_extended_dasd(base.h); // base is a volume in --partition mode
    std::vector<HandleCloser> vols;
    vols.reserve(job.src.volumes.size());
    for (const VolumeInfo& v : job.src.volumes) {
        std::wstring verr;
        vols.emplace_back(open_device(v.read_path, GENERIC_READ, verr,
                                      FILE_FLAG_NO_BUFFERING));
        if (vols.back().h != INVALID_HANDLE_VALUE) allow_extended_dasd(vols.back().h);
    }
    HandleCloser dest;
    if (job.copying) {
        dest.h = static_cast<HANDLE>(job.vhdx->open_raw(err));
        if (dest.h == INVALID_HANDLE_VALUE) {
            log_error(err);
            return;
        }
    }

    Sha256Hasher hasher;
    if (!hasher.valid()) {
        log_error(L"SHA-256 provider unavailable");
        return;
    }
    AlignedBuf buf(static_cast<std::size_t>(job.chunk_size));
    if (!buf.p) {
        log_error(L"cannot allocate the chunk buffer");
        return;
    }
    job.ns_setup.fetch_add(
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - t_setup)
                                       .count()),
        std::memory_order_relaxed);

    for (;;) {
        const std::uint64_t i = job.next.fetch_add(1, std::memory_order_relaxed);
        if (i >= job.chunk_count) break;
        process_chunk(job, base.h, vols, dest.h, i, buf.p, hasher);
        const std::uint64_t done = job.done.fetch_add(1, std::memory_order_relaxed) + 1;

        if (job.opt->file_logs) {
            const int step = static_cast<int>(done * 100 / job.chunk_count) / 5 * 5;
            int prev = job.last_pct.load(std::memory_order_relaxed);
            while (step > prev && step < 100) {
                if (job.last_pct.compare_exchange_weak(prev, step)) {
                    log_info(std::format(
                        L"progress: {}% ({} read, {} written)", step,
                        human_bytes(job.bytes_read.load(std::memory_order_relaxed)),
                        human_bytes(job.bytes_written.load(std::memory_order_relaxed))));
                    break;
                }
            }
        }
    }
}

// A destination sitting on the source is legal (the snapshot keeps the copy
// consistent) but every run rewrites the region where the image itself grew.
void warn_dest_on_source(const Source& src, const fs::path& dest) {
    std::error_code ec;
    const fs::path abs = fs::absolute(dest, ec);
    if (ec) return;
    wchar_t root[MAX_PATH]{};
    if (!::GetVolumePathNameW(abs.c_str(), root, MAX_PATH)) return;
    const std::wstring guid = volume_guid_name(root);
    if (guid.empty()) return;
    bool on_source = false;
    if (src.whole_disk) {
        std::wstring err;
        HandleCloser hv{
            open_device(guid.substr(0, guid.size() - 1), 0, err)};
        int disk = -1;
        std::uint64_t off = 0, len = 0;
        bool single = true;
        if (hv.h != INVALID_HANDLE_VALUE && volume_extent(hv.h, disk, off, len, single))
            on_source = disk == src.disk_number;
    } else if (!src.guid_name.empty()) {
        on_source = _wcsicmp(guid.c_str(), src.guid_name.c_str()) == 0;
    }
    if (on_source)
        log_info(L"note: the destination lies on the source being imaged; its own blocks "
                 L"change every run and will always be rewritten");
}

} // namespace

int run_image(const Options& opt) {
    if (!is_elevated()) {
        log_error(L"raw image copies need an elevated console (run as administrator)");
        return 1;
    }
    std::wstring err;
    if (!opt.destination.empty() && !check_local_drive(opt.destination, err)) {
        log_error(err);
        return 1;
    }

    ImageJob job;
    job.opt = &opt;
    job.copying = !opt.make_db_only;
    if (!resolve_source(opt, job.src, err)) {
        log_error(err);
        return 1;
    }
    if (job.src.size == 0) {
        log_error(std::format(L"{} reports no size", job.src.display));
        return 1;
    }

    job.chunk_size = opt.chunk_size;
    job.chunk_count = (job.src.size + job.chunk_size - 1) / job.chunk_size;
    job.tail_len = job.src.size - (job.chunk_count - 1) * job.chunk_size;

    if (opt.db_path) {
        job.db_file = *opt.db_path;
    } else {
        job.db_file = opt.destination;
        job.db_file += L".tcdb";
    }
    const bool had_db = ChunkDatabase::load(job.db_file, opt.chunk_size, true, job.db);
    job.db.chunk_size = opt.chunk_size;
    log_info(std::format(L"database: {} ({})", job.db_file.native(),
                         had_db ? L"loaded" : L"new"));

    ImageRecord& rec = job.db.image;
    const std::uint8_t kind = job.src.whole_disk ? 1 : 2;
    if (rec.valid && (rec.kind != kind || rec.source_id != job.src.id ||
                      rec.source_size != job.src.size ||
                      rec.chunks.size() != job.chunk_count)) {
        log_info(L"the database describes a different source; rebuilding it");
        rec = ImageRecord{};
    }
    rec.kind = kind;
    rec.source_id = job.src.id;
    rec.source_size = job.src.size;
    rec.chunks.resize(static_cast<size_t>(job.chunk_count), kUnwrittenChunk);

    log_info(std::format(L"imaging {} ({}) -> {}", job.src.display, human_bytes(job.src.size),
                         job.copying ? opt.destination.native() : L"database only"));
    if (job.copying) warn_dest_on_source(job.src, opt.destination);

    // The snapshot set must outlive the copy; released (deleting the
    // snapshots) when run_image returns.
    VssSnapshotSet vss;
    setup_volumes(job.src, opt, vss);

    VhdxDisk vhdx;
    const auto t_attach = std::chrono::steady_clock::now();
    if (job.copying) {
        const std::wstring dest_ext = extended_path(opt.destination);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        const bool dest_exists =
            ::GetFileAttributesExW(dest_ext.c_str(), GetFileExInfoStandard, &fad) != 0;
        if (dest_exists && rec.valid && !rec.db_only &&
            rec.dest_size == ((static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) |
                              fad.nFileSizeLow) &&
            rec.dest_write_time == filetime_to_i64(fad.ftLastWriteTime))
            job.incremental = true;

        if (job.incremental) {
            if (!vhdx.open(opt.destination, err) || !vhdx.attach(err)) {
                log_error(err);
                return 1;
            }
            log_info(std::format(L"incremental: comparing {} recorded chunk hashes against "
                                 L"the source",
                                 job.chunk_count));
        } else {
            if (dest_exists) {
                if (rec.valid && !rec.db_only) {
                    if (rec.dest_size == 0)
                        log_info(L"the previous run did not finish cleanly; rewriting every "
                                 L"chunk");
                    else
                        log_info(L"the destination changed since the last run; rewriting "
                                 L"every chunk (mount images read-only to keep increments "
                                 L"fast)");
                }
                if (!::DeleteFileW(dest_ext.c_str())) {
                    log_error(std::format(L"cannot replace {}: {}", opt.destination.native(),
                                          win32_error_message(::GetLastError())));
                    return 1;
                }
            } else {
                log_info(L"full copy into a new image");
            }
            std::error_code ec;
            if (opt.destination.has_parent_path())
                fs::create_directories(opt.destination.parent_path(), ec);
            const std::uint64_t vsize =
                job.src.whole_disk
                    ? job.src.size
                    : kPartitionDataOffset + round_up(job.src.size, kMiB) + kMiB;
            if (!vhdx.create(opt.destination, vsize, job.src.sector, err) ||
                !vhdx.attach(err)) {
                log_error(err);
                return 1;
            }
            if (!job.src.whole_disk &&
                !write_partition_gpt(static_cast<HANDLE>(vhdx.handle()), vsize,
                                     job.src.sector, kPartitionDataOffset, job.src.size,
                                     err)) {
                log_error(err);
                return 1;
            }
        }
        job.dest_off = job.src.whole_disk ? 0 : kPartitionDataOffset;
        job.vhdx = &vhdx;
        log_info(std::format(
            L"image ready in {:.1f}s",
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_attach).count() / 1e3));
    }
    rec.valid = true;

    {
        Sha256Hasher hasher;
        if (!hasher.valid()) {
            log_error(L"SHA-256 provider unavailable");
            return 1;
        }
        const std::vector<std::uint8_t> zeros(static_cast<size_t>(job.chunk_size), 0);
        job.zero_full = hasher.hash(zeros.data(), static_cast<std::size_t>(job.chunk_size));
        job.zero_tail = hasher.hash(zeros.data(), static_cast<std::size_t>(job.tail_len));
    }

    const auto started = std::chrono::steady_clock::now();
    {
        const size_t workers =
            (opt.multithread && opt.max_threads > 1)
                ? std::min<size_t>(static_cast<size_t>(opt.max_threads),
                                   static_cast<size_t>(job.chunk_count))
                : 1;
        std::vector<std::jthread> pool;
        pool.reserve(workers);
        for (size_t w = 0; w < workers; ++w) pool.emplace_back([&] { image_worker(job); });
    }
    // Workers that could not even open their devices leave chunks untouched.
    job.failed.fetch_add(job.chunk_count - job.done.load());

    bool flushed = true;
    double flush_s = 0.0, detach_s = 0.0;
    if (job.copying) {
        std::wstring ferr;
        auto t = std::chrono::steady_clock::now();
        flushed = vhdx.flush(ferr);
        flush_s = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t).count() / 1e3;
        if (!flushed) log_error(ferr);
        t = std::chrono::steady_clock::now();
        vhdx.detach();
        detach_s = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t).count() / 1e3;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    const std::uint64_t failed = job.failed.load();
    rec.db_only = !job.copying;
    rec.dest_size = 0;
    rec.dest_write_time = 0;
    if (job.copying && failed == 0 && flushed) {
        // Recorded after the detach so the next run can tell whether anything
        // else touched the image in between.
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (::GetFileAttributesExW(extended_path(opt.destination).c_str(),
                                   GetFileExInfoStandard, &fad)) {
            rec.dest_size =
                (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            rec.dest_write_time = filetime_to_i64(fad.ftLastWriteTime);
        }
    }

    std::wstring dberr;
    const bool db_saved = job.db.save(job.db_file, dberr);
    if (!db_saved) log_error(dberr);

    if (opt.make_db_only) {
        log_info(std::format(L"done: {} hashed into database ({} read, {} zero/unallocated), "
                             L"{} chunk(s) failed",
                             human_bytes(job.src.size), human_bytes(job.bytes_read.load()),
                             human_bytes(job.bytes_zero.load()), failed));
    } else {
        log_info(std::format(
            L"done: {} scanned ({} read, {} zero/unallocated), {} written, {} unchanged, "
            L"{} chunk(s) failed",
            human_bytes(job.src.size), human_bytes(job.bytes_read.load()),
            human_bytes(job.bytes_zero.load()), human_bytes(job.bytes_written.load()),
            human_bytes(job.bytes_unchanged.load()), failed));
    }
    // Summed across workers, so with --mt the phases can exceed wall time.
    log_info(std::format(L"time breakdown: worker setup {:.1f}s, source read {:.1f}s, "
                         L"hash {:.1f}s, destination write {:.1f}s, image flush {:.1f}s, "
                         L"detach {:.1f}s",
                         job.ns_setup.load() / 1e9, job.ns_read.load() / 1e9,
                         job.ns_hash.load() / 1e9, job.ns_write.load() / 1e9, flush_s,
                         detach_s));
    log_info(format_time_spent(elapsed));

    if (failed > 0 || !db_saved || !flushed) return 2;
    return 0;
}

} // namespace tc
