#include "restore.h"

#include "chunkdb.h"
#include "devio.h"
#include "gpt.h"
#include "hash.h"
#include "util.h"
#include "vdisk.h"

#include <windows.h>

#include <winioctl.h>

#include <algorithm>
#include <atomic>
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

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// ---------------------------------------------------------------------------
// What the image holds

struct ImageInfo {
    std::uint64_t size = 0;     // virtual size of the attached image
    std::uint32_t sector = 512; // logical sector size the image was created with
    bool gpt = false;           // the image starts with a GPT
    bool wrapped = false;       // ... whose single entry is a tilecopy volume image
    std::uint64_t data_off = 0; // where the payload starts inside the image
    std::uint64_t data_len = 0; // payload bytes
};

// A whole-disk image is the disk's own bytes, so anything may sit at sector 0;
// a --partition image is recognized by the wrapper tilecopy writes around the
// volume - one basic-data entry named "tilecopy" at the fixed data offset.
// Nothing else distinguishes the two, which is why a matching database wins
// over this probe when there is one.
bool inspect_image(HANDLE img, ImageInfo& info, std::wstring& error) {
    if (!device_length(img, info.size)) {
        error = std::format(L"cannot get the size of the attached image: {}",
                            win32_error_message(::GetLastError()));
        return false;
    }
    info.sector = disk_sector_size(img);
    info.data_off = 0;
    info.data_len = info.size;
    if (info.sector == 0 || info.size < 4 * static_cast<std::uint64_t>(info.sector)) {
        error = L"the image is too small to hold a device copy";
        return false;
    }

    AlignedBuf buf(64 * 1024); // one aligned window for the header and the entries
    if (!buf.p) {
        error = L"cannot allocate the image probe buffer";
        return false;
    }
    if (!read_at(img, info.sector, buf.p, info.sector)) { // GPT header lives at LBA 1
        error = std::format(L"cannot read the image at offset {}: {}", info.sector,
                            win32_error_message(::GetLastError()));
        return false;
    }
    const auto* h = reinterpret_cast<const GptHeader*>(buf.p);
    if (std::memcmp(h->signature, "EFI PART", 8) != 0) return true; // no GPT
    info.gpt = true;
    if (h->entry_size != sizeof(GptEntry) || h->entry_count == 0) return true;

    const std::uint64_t entries_off = h->entries_lba * info.sector;
    if (entries_off == 0 || entries_off + info.sector > info.size) return true;
    if (!read_at(img, entries_off, buf.p, info.sector)) {
        error = std::format(L"cannot read the image at offset {}: {}", entries_off,
                            win32_error_message(::GetLastError()));
        return false;
    }
    const auto* e = reinterpret_cast<const GptEntry*>(buf.p);
    wchar_t name[37]{};
    std::memcpy(name, e->name, sizeof(e->name));
    if (std::wcscmp(name, kImagePartitionName) != 0) return true;
    if (std::memcmp(e->type, &kBasicDataPartition, 16) != 0) return true;
    if (e->last_lba < e->first_lba) return true;
    const std::uint64_t off = e->first_lba * info.sector;
    const std::uint64_t len = (e->last_lba + 1 - e->first_lba) * info.sector;
    if (off != kPartitionDataOffset || len == 0 || off + len > info.size) return true;
    info.wrapped = true;
    info.data_off = off;
    info.data_len = len;
    return true;
}

// ---------------------------------------------------------------------------
// What is being written to

struct Target {
    bool whole_disk = false;
    int disk_number = -1;
    std::wstring device;    // \\.\PhysicalDriveN or the volume device, no trailing slash
    std::wstring display;   // what the user named
    std::wstring guid_name; // volume targets: \\?\Volume{...}\ when known
    std::wstring model;     // disk product name, for the confirmation
    std::uint64_t size = 0;
    std::uint32_t sector = 512;
    bool removable = false;
};

void query_device_model(HANDLE h, std::wstring& model, bool& removable) {
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    std::vector<std::uint8_t> out(4096, 0);
    DWORD br = 0;
    if (!::DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), out.data(),
                           static_cast<DWORD>(out.size()), &br, nullptr) ||
        br < sizeof(STORAGE_DEVICE_DESCRIPTOR))
        return;
    const auto* d = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(out.data());
    removable = d->RemovableMedia != FALSE;
    const auto field = [&](DWORD off) -> std::string {
        if (off == 0 || off >= br) return {};
        const char* p = reinterpret_cast<const char*>(out.data()) + off;
        return std::string(p, ::strnlen(p, br - off));
    };
    std::string s = field(d->VendorIdOffset);
    const std::string product = field(d->ProductIdOffset);
    if (!product.empty()) {
        if (!s.empty()) s += ' ';
        s += product;
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    s.erase(0, s.find_first_not_of(' '));
    model = utf8_to_wide(s); // the descriptor strings are ASCII
}

bool resolve_target(const fs::path& spec, bool want_disk, Target& t, std::wstring& error) {
    const std::wstring s = spec.native();
    const std::wstring low = lower(s);
    const bool letter = s.size() == 3 && s[1] == L':' && s[2] == L'\\';

    if (want_disk) {
        if (low.starts_with(LR"(\\.\physicaldrive)")) {
            t.device = s;
        } else if (letter) { // the disk that holds that volume
            HandleCloser hv{open_device(std::format(LR"(\\.\{}:)", s[0]), 0, error)};
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
            t.device = std::format(LR"(\\.\PhysicalDrive{})", disk);
        } else {
            error = std::format(L"{} is a volume, but the image is a copy of a whole disk; "
                                LR"(name a disk (N, \\.\PhysicalDriveN, or a drive letter )"
                                L"whose disk should be overwritten)",
                                s);
            return false;
        }
        t.whole_disk = true;
        t.display = t.device;
        t.disk_number = static_cast<int>(
            std::wcstol(t.device.c_str() + wcslen(LR"(\\.\PhysicalDrive)"), nullptr, 10));
        HandleCloser hd{open_device(t.device, GENERIC_READ, error)};
        if (hd.h == INVALID_HANDLE_VALUE) return false;
        if (!device_length(hd.h, t.size)) {
            error = std::format(L"cannot get the size of {}: {}", t.device,
                                win32_error_message(::GetLastError()));
            return false;
        }
        t.sector = disk_sector_size(hd.h);
        query_device_model(hd.h, t.model, t.removable);
        return true;
    }

    if (letter) {
        t.device = std::format(LR"(\\.\{}:)", s[0]);
        t.guid_name = volume_guid_name(s);
    } else if (low.starts_with(LR"(\\?\volume{)")) {
        t.device = s;
        if (t.device.back() == L'\\') t.device.pop_back();
        t.guid_name = t.device + L"\\";
    } else if (low.starts_with(LR"(\\.\harddiskvolume)")) {
        t.device = s;
        t.guid_name = guid_name_for_nt_device(L"\\Device\\" + s.substr(4));
    } else {
        // The braces are doubled for std::format, not for the reader.
        error = std::format(L"{} is a whole disk, but the image holds a single volume; name a "
                            LR"(volume (a drive letter, \\?\Volume{{GUID}}\ or )"
                            LR"(\\.\HarddiskVolumeN))",
                            s);
        return false;
    }
    t.display = s;
    HandleCloser hv{open_device(t.device, GENERIC_READ, error)};
    if (hv.h == INVALID_HANDLE_VALUE) return false;
    if (!device_length(hv.h, t.size)) {
        error = std::format(L"cannot get the size of {}: {}", t.device,
                            win32_error_message(::GetLastError()));
        return false;
    }
    int disk = -1;
    std::uint64_t off = 0, len = 0;
    bool single = true;
    if (volume_extent(hv.h, disk, off, len, single)) {
        t.disk_number = single ? disk : -1;
        HandleCloser hd{::CreateFileW(std::format(LR"(\\.\PhysicalDrive{})", disk).c_str(),
                                      GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, 0, nullptr)};
        if (hd.h != INVALID_HANDLE_VALUE) {
            t.sector = disk_sector_size(hd.h);
            query_device_model(hd.h, t.model, t.removable);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// What the target holds right now (for the confirmation)

struct VolumeBrief {
    std::wstring device; // \\?\Volume{...} - always openable, unlike a mount path
    std::wstring display, label, fs_name;
    std::uint64_t size = 0;
};

std::vector<VolumeBrief> volumes_on_disk(int disk) {
    std::vector<VolumeBrief> out;
    wchar_t vol[MAX_PATH];
    HANDLE f = ::FindFirstVolumeW(vol, MAX_PATH);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring name = vol; // volume GUID path with the trailing backslash
        HandleCloser hv{::CreateFileW(name.substr(0, name.size() - 1).c_str(), 0,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, 0, nullptr)};
        if (hv.h == INVALID_HANDLE_VALUE) continue;
        int d = -1;
        std::uint64_t off = 0, len = 0;
        bool single = true;
        if (!volume_extent(hv.h, d, off, len, single) || d != disk) continue;
        VolumeBrief b;
        b.device = name.substr(0, name.size() - 1);
        b.display = volume_display(name);
        b.size = len;
        wchar_t label[MAX_PATH]{}, fsn[64]{};
        if (::GetVolumeInformationW(name.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr,
                                    fsn, 64)) {
            b.label = label;
            b.fs_name = fsn;
        }
        out.push_back(std::move(b));
    } while (::FindNextVolumeW(f, vol, MAX_PATH));
    ::FindVolumeClose(f);
    return out;
}

std::wstring layout_summary(HANDLE hdisk) {
    std::vector<std::uint8_t> buf(64 * 1024, 0);
    DWORD br = 0;
    if (!::DeviceIoControl(hdisk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, buf.data(),
                           static_cast<DWORD>(buf.size()), &br, nullptr))
        return L"no readable partition table";
    const auto* dl = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(buf.data());
    int used = 0;
    for (DWORD i = 0; i < dl->PartitionCount; ++i) {
        const PARTITION_INFORMATION_EX& p = dl->PartitionEntry[i];
        if (p.PartitionLength.QuadPart == 0) continue;
        if (dl->PartitionStyle == PARTITION_STYLE_MBR &&
            p.Mbr.PartitionType == PARTITION_ENTRY_UNUSED)
            continue;
        ++used;
    }
    const wchar_t* style = dl->PartitionStyle == PARTITION_STYLE_GPT   ? L"GPT"
                           : dl->PartitionStyle == PARTITION_STYLE_MBR ? L"MBR"
                                                                       : L"uninitialized";
    return std::format(L"{}, {} partition(s)", style, used);
}

// Volume GUID name and disk number of whatever volume holds a path.
void locate_path(const fs::path& p, std::wstring& guid, int& disk) {
    guid.clear();
    disk = -1;
    std::error_code ec;
    const fs::path abs = fs::absolute(p, ec);
    if (ec) return;
    wchar_t root[MAX_PATH]{};
    if (!::GetVolumePathNameW(abs.c_str(), root, MAX_PATH)) return;
    guid = volume_guid_name(root);
    if (guid.empty()) return;
    std::wstring err;
    HandleCloser hv{open_device(guid.substr(0, guid.size() - 1), 0, err)};
    if (hv.h == INVALID_HANDLE_VALUE) return;
    int d = -1;
    std::uint64_t off = 0, len = 0;
    bool single = true;
    if (volume_extent(hv.h, d, off, len, single)) disk = d;
}

// Whether a path lies on what is about to be overwritten.
bool path_on_target(const fs::path& p, const Target& t) {
    std::wstring guid;
    int disk = -1;
    locate_path(p, guid, disk);
    if (t.whole_disk) return disk >= 0 && disk == t.disk_number;
    return !guid.empty() && !t.guid_name.empty() && _wcsicmp(guid.c_str(), t.guid_name.c_str()) == 0;
}

// ---------------------------------------------------------------------------
// Confirmation

bool read_console_line(std::wstring& out) {
    HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    // A redirected stdin is not a console: there is nobody to ask, and reading
    // a script's input as an answer is the last thing a restore should do.
    if (in == nullptr || in == INVALID_HANDLE_VALUE || !::GetConsoleMode(in, &mode))
        return false;
    wchar_t buf[128]{};
    DWORD got = 0;
    if (!::ReadConsoleW(in, buf, 127, &got, nullptr)) return false;
    out.assign(buf, got);
    while (!out.empty() && std::iswspace(out.back())) out.pop_back();
    out.erase(0, out.find_first_not_of(L" \t"));
    return true;
}

// ---------------------------------------------------------------------------
// The restore itself

struct RestoreJob {
    const Options* opt = nullptr;
    const VhdxDisk* image = nullptr;
    HANDLE target = INVALID_HANDLE_VALUE; // one handle, shared: one device, ordered writes
    std::uint64_t img_off = 0;            // payload offset inside the image
    std::uint64_t data_len = 0;
    std::uint64_t chunk_size = 0, chunk_count = 0, tail_len = 0;
    std::uint32_t target_sector = 512;
    bool compare = true; // read the target first and write only what differs
    bool verify = false; // check each chunk read against the recorded hash
    const std::vector<Sha256>* hashes = nullptr;
    Sha256 zero_full{}, zero_tail{};

    std::atomic<std::uint64_t> next{0}, done{0}, failed{0}, mismatched{0}, unverified{0};
    std::atomic<std::uint64_t> bytes_read{0}, bytes_written{0}, bytes_unchanged{0};
    // Wall time per phase, in nanoseconds, summed across workers.
    std::atomic<std::uint64_t> ns_setup{0}, ns_read{0}, ns_hash{0}, ns_compare{0}, ns_write{0};
    std::atomic<int> last_pct{0};
};

void restore_chunk(RestoreJob& job, HANDLE img, std::uint64_t idx, std::uint8_t* buf,
                   std::uint8_t* cmp, Sha256Hasher& hasher) {
    const std::uint64_t off = idx * job.chunk_size;
    const std::uint64_t len = std::min(job.chunk_size, job.data_len - off);
    // The image is a whole number of its own sectors, which need not divide by
    // the target's; the last chunk is then padded with zeros up to the target's
    // sector size (the chunk buffer is a multiple of 4K, so it always fits).
    const std::uint64_t wlen = round_up(len, job.target_sector);
    const auto since = [](std::chrono::steady_clock::time_point t0) {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - t0)
                                              .count());
    };

    bool ok = false;
    DWORD rerr = 0;
    const auto tr = std::chrono::steady_clock::now();
    for (int attempt = 1; attempt <= job.opt->max_tries && !ok; ++attempt) {
        ok = read_at(img, job.img_off + off, buf, static_cast<DWORD>(len));
        if (!ok) {
            rerr = ::GetLastError();
            if (attempt < job.opt->max_tries) ::Sleep(250);
        }
    }
    job.ns_read.fetch_add(since(tr), std::memory_order_relaxed);
    if (!ok) {
        // Nothing is written for this chunk, so the target keeps whatever it
        // held: it no longer matches the image and the run reports a failure.
        job.failed.fetch_add(1, std::memory_order_relaxed);
        log_error(std::format(L"chunk {} (offset {}): cannot read the image: {}", idx, off,
                              win32_error_message(rerr)));
        return;
    }
    if (wlen > len) std::memset(buf + len, 0, static_cast<size_t>(wlen - len));
    job.bytes_read.fetch_add(len, std::memory_order_relaxed);

    if (job.verify) {
        const Sha256& slot = (*job.hashes)[static_cast<size_t>(idx)];
        if (slot == kFailedChunk) {
            job.unverified.fetch_add(1, std::memory_order_relaxed);
        } else {
            const auto th = std::chrono::steady_clock::now();
            const Sha256 h = hasher.hash(buf, static_cast<std::size_t>(len));
            job.ns_hash.fetch_add(since(th), std::memory_order_relaxed);
            // A hole in the image is recorded as unwritten and reads as zeros.
            const Sha256& want =
                slot == kUnwrittenChunk
                    ? (len == job.chunk_size ? job.zero_full : job.zero_tail)
                    : slot;
            if (h != want) {
                job.mismatched.fetch_add(1, std::memory_order_relaxed);
                log_error(std::format(L"chunk {} (offset {}) does not match the hash recorded "
                                      L"for it; the image content is not what was backed up",
                                      idx, off));
            }
        }
    }

    if (job.compare) {
        const auto tc0 = std::chrono::steady_clock::now();
        // A target that cannot be read tells us nothing, so it is written.
        const bool same = read_at(job.target, off, cmp, static_cast<DWORD>(wlen)) &&
                          std::memcmp(buf, cmp, static_cast<size_t>(len)) == 0;
        job.ns_compare.fetch_add(since(tc0), std::memory_order_relaxed);
        if (same) {
            job.bytes_unchanged.fetch_add(len, std::memory_order_relaxed);
            return;
        }
    }

    ok = false;
    DWORD werr = 0;
    const auto tw = std::chrono::steady_clock::now();
    for (int attempt = 1; attempt <= job.opt->max_tries && !ok; ++attempt) {
        ok = write_at(job.target, off, buf, static_cast<DWORD>(wlen));
        if (!ok) {
            werr = ::GetLastError();
            if (attempt < job.opt->max_tries) ::Sleep(250);
        }
    }
    job.ns_write.fetch_add(since(tw), std::memory_order_relaxed);
    if (!ok) {
        job.failed.fetch_add(1, std::memory_order_relaxed);
        log_error(std::format(L"chunk {} (offset {}): write failed: {}", idx, off,
                              win32_error_message(werr)));
        return;
    }
    job.bytes_written.fetch_add(len, std::memory_order_relaxed);
}

void restore_worker(RestoreJob& job) {
    const auto t_setup = std::chrono::steady_clock::now();
    std::wstring err;
    // Unbuffered: a restore reads each image block once and the target only to
    // compare it, so the file cache would just add copies. Every offset and
    // length below is a multiple of both sector sizes by construction, and the
    // buffers are page-aligned.
    HandleCloser img{static_cast<HANDLE>(job.image->open_raw(err, FILE_FLAG_NO_BUFFERING))};
    if (img.h == INVALID_HANDLE_VALUE) {
        log_error(err);
        return; // unprocessed chunks are counted as failed afterwards
    }
    Sha256Hasher hasher;
    if (job.verify && !hasher.valid()) {
        log_error(L"SHA-256 provider unavailable");
        return;
    }
    AlignedBuf buf(static_cast<std::size_t>(job.chunk_size));
    AlignedBuf cmp(job.compare ? static_cast<std::size_t>(job.chunk_size) : 4096);
    if (!buf.p || !cmp.p) {
        log_error(L"cannot allocate the chunk buffers");
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
        restore_chunk(job, img.h, i, buf.p, cmp.p, hasher);
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

// ---------------------------------------------------------------------------
// Taking the target away from Windows and giving it back

bool set_disk_offline(HANDLE hdisk, bool offline, std::wstring& error) {
    SET_DISK_ATTRIBUTES attrs{};
    attrs.Version = sizeof(attrs);
    attrs.Persist = FALSE; // for this session only: a reboot brings it back
    attrs.Attributes = offline ? DISK_ATTRIBUTE_OFFLINE : 0;
    attrs.AttributesMask = DISK_ATTRIBUTE_OFFLINE;
    DWORD br = 0;
    if (!::DeviceIoControl(hdisk, IOCTL_DISK_SET_DISK_ATTRIBUTES, &attrs, sizeof(attrs),
                           nullptr, 0, &br, nullptr)) {
        error = win32_error_message(::GetLastError());
        return false;
    }
    return true;
}

void update_disk_properties(HANDLE hdisk) {
    DWORD br = 0;
    ::DeviceIoControl(hdisk, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &br,
                      nullptr);
}

// Locks and dismounts a volume; the lock lasts exactly as long as the handle,
// which is why the caller keeps them all open for the whole restore.
bool lock_and_dismount(HANDLE hv, bool& locked) {
    DWORD br = 0;
    locked = ::DeviceIoControl(hv, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &br, nullptr) != 0;
    return ::DeviceIoControl(hv, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &br, nullptr) !=
           0;
}

} // namespace

int run_restore(const Options& opt) {
    if (!is_elevated()) {
        log_error(L"--restore needs an elevated console (run as administrator)");
        return 1;
    }
    std::wstring err;
    if (!check_local_drive(opt.source, err)) {
        log_error(err);
        return 1;
    }
    const std::wstring src_ext = extended_path(opt.source);
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(src_ext.c_str(), GetFileExInfoStandard, &fad)) {
        log_error(std::format(L"cannot open {}: {}", opt.source.native(),
                              win32_error_message(::GetLastError())));
        return 1;
    }
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        log_error(std::format(L"{} is a directory, not a .vhdx image", opt.source.native()));
        return 1;
    }

    // The database is only read here - never written - and only to check what
    // comes out of the image against what went in.
    fs::path db_file;
    if (opt.db_path) {
        db_file = *opt.db_path;
    } else {
        db_file = opt.source;
        db_file += L".tcdb";
    }
    ChunkDatabase db;
    const bool db_loaded = ChunkDatabase::load(db_file, opt.chunk_size, true, db);
    const bool db_exists =
        ::GetFileAttributesW(extended_path(db_file).c_str()) != INVALID_FILE_ATTRIBUTES;
    log_info(std::format(
        L"database: {} ({})", db_file.native(),
        db_loaded ? L"loaded"
        : db_exists
            ? L"unusable here (different --chunk-size, or not an image database); "
              L"chunks are not verified"
            : L"not found; chunks are not verified"));

    VhdxDisk image;
    if (!image.open(opt.source, err, /*read_only=*/true) || !image.attach(err)) {
        log_error(err);
        return 1;
    }
    ImageInfo info;
    if (!inspect_image(static_cast<HANDLE>(image.handle()), info, err)) {
        log_error(err);
        return 1;
    }

    // The probe reads the image; a database that describes this image is more
    // authoritative than the probe, and one that does not is ignored outright.
    const ImageRecord& rec = db.image;
    bool whole_disk = !info.wrapped;
    bool verify = false;
    std::uint64_t disk_payload = info.size; // what a whole-disk image restores
    if (db_loaded && rec.valid) {
        // A whole-disk image is created at the size of the disk it came from,
        // but the virtual size is only guaranteed to be a whole number of
        // sectors: the recorded source size is the exact one, and anything the
        // container added past it is padding that must not be written.
        if (rec.kind == 1 && rec.source_size <= info.size &&
            info.size - rec.source_size < kMiB) {
            whole_disk = true;
            verify = true;
            disk_payload = rec.source_size;
            if (info.wrapped)
                log_info(L"note: the image carries a tilecopy volume wrapper, but the database "
                         L"records a whole-disk copy; restoring it as a whole disk");
        } else if (rec.kind == 2 && info.wrapped && rec.source_size == info.data_len) {
            whole_disk = false;
            verify = true;
        } else {
            log_info(std::format(L"note: the database describes a different image ({} of {}); "
                                 L"ignoring it, chunks are not verified",
                                 rec.kind == 2 ? L"a volume" : L"a disk",
                                 human_bytes(rec.source_size)));
        }
    }
    if (whole_disk) {
        info.data_off = 0;
        info.data_len = disk_payload;
    }
    if (info.data_len == 0) {
        log_error(L"the image holds no data to restore");
        return 1;
    }
    log_info(std::format(L"image:    {} ({} {}, {}-byte sectors{})", opt.source.native(),
                         human_bytes(info.data_len),
                         whole_disk ? L"whole disk" : L"volume image", info.sector,
                         whole_disk ? L"" : L", inside a GPT wrapper"));

    Target target;
    if (!resolve_target(opt.destination, whole_disk, target, err)) {
        log_error(err);
        return 1;
    }
    if (target.size < info.data_len) {
        log_error(std::format(L"{} holds {} but the image needs {}; restore it to a device at "
                              L"least as large as the one it was made from",
                              target.display, human_bytes(target.size),
                              human_bytes(info.data_len)));
        return 1;
    }

    const std::uint64_t chunk_size = opt.chunk_size;
    const std::uint64_t chunk_count = (info.data_len + chunk_size - 1) / chunk_size;
    if (verify && rec.chunks.size() != static_cast<size_t>(chunk_count)) {
        log_info(L"note: the database was written with a different chunk layout; chunks are "
                 L"not verified");
        verify = false;
    }

    // Refusals: what the restore must never eat.
    wchar_t windir[MAX_PATH]{};
    if (::GetSystemWindowsDirectoryW(windir, MAX_PATH) && path_on_target(windir, target)) {
        log_error(std::format(L"{} holds the running Windows installation; refusing to "
                              L"overwrite it",
                              target.display));
        return 1;
    }
    if (path_on_target(opt.source, target)) {
        log_error(std::format(L"the image {} lies on {}; restoring would destroy the image "
                              L"halfway through",
                              opt.source.native(), target.display));
        return 1;
    }
    // Attaching the image gave it a disk number of its own, which is a number
    // the user could have named as the target.
    {
        const std::wstring& img_dev = image.physical_path();
        const std::wstring prefix = LR"(\\.\PhysicalDrive)";
        int img_disk = -1;
        if (lower(img_dev).starts_with(lower(prefix)))
            img_disk = static_cast<int>(
                std::wcstol(img_dev.c_str() + prefix.size(), nullptr, 10));
        if (img_disk >= 0 && img_disk == target.disk_number) {
            log_error(std::format(L"{} is the image itself, attached as {}; name the device the "
                                  L"image should be written to",
                                  target.display, img_dev));
            return 1;
        }
    }

    // Warnings the user should see before confirming.
    std::vector<std::wstring> notes;
    if (db_loaded && path_on_target(db_file, target))
        notes.push_back(std::format(L"the chunk database {} lies on the target and will be "
                                    L"destroyed (it is not needed to restore)",
                                    db_file.native()));
    if (target.sector != info.sector)
        notes.push_back(std::format(L"the image was made from a device with {}-byte sectors but "
                                    L"the target has {}-byte sectors; partition tables and "
                                    L"file systems inside the image assume the original size",
                                    info.sector, target.sector));
    if (target.size > info.data_len) {
        notes.push_back(std::format(L"the target is {} larger than the image; that tail is left "
                                    L"untouched",
                                    human_bytes(target.size - info.data_len)));
        if (whole_disk && info.gpt)
            notes.push_back(L"the image is GPT: its backup header lands where the original "
                            L"disk ended, not at the end of the larger target, so Windows "
                            L"will ask to repair the partition table afterwards");
    }
    for (const std::wstring& n : notes) log_info(L"note: " + n);

    // Confirmation. The token is something only someone looking at the right
    // device would type.
    const std::wstring token =
        target.whole_disk ? std::to_wstring(target.disk_number)
        : (target.display.size() >= 2 && target.display[1] == L':')
            ? std::wstring{static_cast<wchar_t>(std::towupper(target.display[0]))}
            : L"YES";
    {
        std::wstring desc = target.display;
        if (!target.model.empty()) desc += L" (" + target.model + L")";
        log_info(std::format(L"target:   {}: {}{}", desc, human_bytes(target.size),
                             target.removable ? L", removable" : L""));
        if (target.whole_disk) {
            std::wstring lerr;
            HandleCloser hd{open_device(target.device, GENERIC_READ, lerr)};
            if (hd.h != INVALID_HANDLE_VALUE)
                log_info(L"          currently: " + layout_summary(hd.h));
            for (const VolumeBrief& v : volumes_on_disk(target.disk_number))
                log_info(std::format(L"            {} {}{} ({})", v.display,
                                     v.label.empty() ? std::wstring{} : L"\"" + v.label + L"\" ",
                                     v.fs_name.empty() ? std::wstring{L"unformatted"} : v.fs_name,
                                     human_bytes(v.size)));
        } else {
            wchar_t label[MAX_PATH]{}, fsn[64]{};
            std::wstring root = target.guid_name.empty() ? target.display : target.guid_name;
            if (!root.empty() && root.back() != L'\\') root += L'\\';
            if (::GetVolumeInformationW(root.c_str(), label, MAX_PATH, nullptr, nullptr,
                                        nullptr, fsn, 64)) {
                const std::wstring fs_name = fsn[0] ? fsn : L"unknown";
                const std::wstring name = label;
                log_info(std::format(L"          currently: {}{}", fs_name,
                                     name.empty() ? std::wstring{}
                                                  : L" \"" + name + L"\""));
            }
        }
    }
    if (!opt.assume_yes) {
        log_info(std::format(L"everything on {} will be replaced by the image. Type {} to "
                             L"confirm, anything else aborts:",
                             target.display, token));
        std::wstring answer;
        if (!read_console_line(answer)) {
            log_error(L"cannot ask for confirmation (stdin is not a console); pass --yes to "
                      L"restore without being asked");
            return 1;
        }
        if (_wcsicmp(answer.c_str(), token.c_str()) != 0) {
            log_info(L"aborted; nothing was written");
            return 1;
        }
    }

    // Take the target away from Windows: locked and dismounted volumes cannot
    // write behind the restore, and an offline disk is not remounted while it
    // is being rewritten. The handles stay open until the restore is done.
    const auto t_prep = std::chrono::steady_clock::now();
    std::vector<HandleCloser> held;
    HandleCloser disk_ctl; // whole-disk targets: the handle the offline flip rides on
    HandleCloser target_h;
    if (target.whole_disk) {
        disk_ctl.h = open_device(target.device, GENERIC_READ | GENERIC_WRITE, err);
        if (disk_ctl.h == INVALID_HANDLE_VALUE) {
            log_error(err);
            return 1;
        }
        // Opened before anything is dismounted or taken offline, so no failure
        // path can leave the disk offline behind us. Handles survive the
        // transition; only the writes need the exclusivity, and those come last.
        target_h.h = open_device(target.device, GENERIC_READ | GENERIC_WRITE, err,
                                 FILE_FLAG_NO_BUFFERING);
        if (target_h.h == INVALID_HANDLE_VALUE) {
            log_error(err);
            return 1;
        }
        for (const VolumeBrief& v : volumes_on_disk(target.disk_number)) {
            std::wstring verr;
            HandleCloser hv{open_device(v.device, GENERIC_READ | GENERIC_WRITE, verr)};
            if (hv.h == INVALID_HANDLE_VALUE) {
                log_info(std::format(L"note: {} could not be opened to dismount it ({}); the "
                                     L"restore may be refused for its region",
                                     v.display, verr));
                continue;
            }
            bool locked = false;
            if (!lock_and_dismount(hv.h, locked))
                log_info(std::format(L"note: {} could not be dismounted; writes over it may "
                                     L"fail",
                                     v.display));
            else if (!locked)
                log_info(std::format(L"note: {} was dismounted but not locked (something has "
                                     L"it open)",
                                     v.display));
            held.push_back(std::move(hv));
        }
        std::wstring oerr;
        if (!set_disk_offline(disk_ctl.h, true, oerr))
            log_info(L"note: the target disk could not be taken offline (" + oerr +
                     L"); the dismounted volumes are what keeps it exclusive");
        else
            update_disk_properties(disk_ctl.h);
    } else {
        target_h.h = open_device(target.device, GENERIC_READ | GENERIC_WRITE, err,
                                 FILE_FLAG_NO_BUFFERING);
        if (target_h.h == INVALID_HANDLE_VALUE) {
            log_error(err);
            return 1;
        }
        // Writing the volume's own boot sector and metadata under a live file
        // system corrupts it, so an unlockable volume is a hard stop here.
        allow_extended_dasd(target_h.h);
        bool locked = false;
        if (!lock_and_dismount(target_h.h, locked) || !locked) {
            log_error(std::format(L"cannot lock and dismount {}: {}. Close anything using it "
                                  L"(Explorer windows, indexing, antivirus) and try again",
                                  target.display, win32_error_message(::GetLastError())));
            return 1;
        }
    }
    log_info(std::format(L"target ready in {:.1f}s",
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t_prep)
                                 .count() /
                             1e3));

    RestoreJob job;
    job.opt = &opt;
    job.image = &image;
    job.target = target_h.h;
    job.img_off = info.data_off;
    job.data_len = info.data_len;
    job.chunk_size = chunk_size;
    job.chunk_count = chunk_count;
    job.tail_len = info.data_len - (chunk_count - 1) * chunk_size;
    job.target_sector = target.sector;
    job.compare = !opt.restore_write_all;
    job.verify = verify;
    job.hashes = verify ? &rec.chunks : nullptr;
    if (verify) {
        Sha256Hasher hasher;
        if (!hasher.valid()) {
            log_error(L"SHA-256 provider unavailable");
            return 1;
        }
        const std::vector<std::uint8_t> zeros(static_cast<size_t>(chunk_size), 0);
        job.zero_full = hasher.hash(zeros.data(), static_cast<std::size_t>(chunk_size));
        job.zero_tail = hasher.hash(zeros.data(), static_cast<std::size_t>(job.tail_len));
    }

    log_info(std::format(L"restoring {} onto {} in {} chunk(s) of {}{}{}",
                         human_bytes(info.data_len), target.display, chunk_count,
                         human_bytes(chunk_size),
                         job.compare ? L", writing only what differs"
                                     : L", writing every chunk",
                         verify ? L", verified against the database" : L""));

    const auto started = std::chrono::steady_clock::now();
    {
        const size_t workers =
            (opt.multithread && opt.max_threads > 1)
                ? std::min<size_t>(static_cast<size_t>(opt.max_threads),
                                   static_cast<size_t>(chunk_count))
                : 1;
        std::vector<std::jthread> pool;
        pool.reserve(workers);
        for (size_t w = 0; w < workers; ++w) pool.emplace_back([&] { restore_worker(job); });
    }
    // Workers that could not even open their handles leave the tail of the
    // chunk list unclaimed (indexes are handed out in order).
    const std::uint64_t claimed = std::min(job.next.load(), chunk_count);
    job.failed.fetch_add(chunk_count - claimed);

    bool flushed = ::FlushFileBuffers(target_h.h) != 0;
    if (!flushed)
        log_error(std::format(L"cannot flush {}: {}", target.display,
                              win32_error_message(::GetLastError())));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    // Give the target back: release the locks first, then let Windows re-read
    // what is now on the device.
    target_h = HandleCloser{};
    held.clear();
    if (target.whole_disk) {
        std::wstring oerr;
        if (!set_disk_offline(disk_ctl.h, false, oerr))
            log_info(L"note: the target disk could not be brought back online (" + oerr +
                     L"); bring it online in Disk Management or reboot");
        update_disk_properties(disk_ctl.h);
    } else if (target.disk_number >= 0) {
        std::wstring derr;
        HandleCloser hd{open_device(std::format(LR"(\\.\PhysicalDrive{})", target.disk_number),
                                    GENERIC_READ | GENERIC_WRITE, derr)};
        if (hd.h != INVALID_HANDLE_VALUE) update_disk_properties(hd.h);
    }

    const std::uint64_t failed = job.failed.load();
    const std::uint64_t mismatched = job.mismatched.load();
    log_info(std::format(L"done: {} restored ({} read from the image, {} written, {} already "
                         L"identical), {} chunk(s) failed",
                         human_bytes(info.data_len), human_bytes(job.bytes_read.load()),
                         human_bytes(job.bytes_written.load()),
                         human_bytes(job.bytes_unchanged.load()), failed));
    if (verify)
        log_info(std::format(L"verified {} chunk(s) against the database: {} mismatch(es), {} "
                             L"chunk(s) the backup run never copied successfully",
                             chunk_count, mismatched, job.unverified.load()));
    if (failed > 0)
        log_info(L"the target does not match the image: the failed chunk(s) still hold what "
                 L"was there before. Run the restore again");
    if (mismatched > 0)
        log_info(L"the mismatched chunk(s) were restored as the image holds them, but the "
                 L"image no longer matches what was backed up - treat it as damaged");
    log_info(std::format(L"time breakdown: worker setup {:.1f}s, image read {:.1f}s, hash "
                         L"{:.1f}s, target compare {:.1f}s, target write {:.1f}s",
                         job.ns_setup.load() / 1e9, job.ns_read.load() / 1e9,
                         job.ns_hash.load() / 1e9, job.ns_compare.load() / 1e9,
                         job.ns_write.load() / 1e9));
    log_info(format_time_spent(elapsed));

    if (failed > 0 || mismatched > 0 || !flushed) return 2;
    return 0;
}

} // namespace tc
