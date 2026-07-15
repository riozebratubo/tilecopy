#include "options.h"

#include "util.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <format>
#include <string_view>

namespace tc {

#ifndef TILECOPY_VERSION
#define TILECOPY_VERSION L"unknown"
#endif

void print_usage() {
    log_info(
        L"tilecopy " TILECOPY_VERSION
        LR"( - delta-copy files, folders or whole drives between local drives

Usage:
  tilecopy --file   <source-file> <dest-file>   [options]
  tilecopy --folder <source-dir>  <dest-dir>    [options]
  tilecopy --drive  <X:>          <Y:|dest-dir> [options]
  tilecopy --drive  <X:|N|\\.\PhysicalDriveN>   <image.vhdx> [options]
  tilecopy --partition <X:|\\?\Volume{GUID}\|\\.\HarddiskVolumeN>
                                  <image.vhdx> [options]
  tilecopy --file/--folder/--drive/--partition <source> --make-db [options]

Common options:
  --db <path>            Chunk-database file to use (default: next to the
                         destination: <dest-file>.tcdb, <dest-dir>\tilecopy.tcdb,
                         Y:\tilecopy.tcdb; derived from the source when
                         --make-db is used without a destination)
  --make-db              Only (re)generate the chunk database, copy nothing;
                         the destination argument may be omitted
  --chunk-size <size>    Delta chunk size, 4K-64M, K/M suffixes allowed
                         (default 1M; a database built with a different chunk
                         size is discarded and rebuilt)
  --max-tries <n>        Attempts per file before giving up (default 1)
  --no-file-logs         Do not print a line per file copied; only the initial
                         and final messages (and errors) are printed

Folder and drive options:
  --mirror               Delete destination entries that do not exist in the
                         source (excluded entries are left untouched)
  --no-move-detection    Do not detect moved/renamed source files. By default
                         a file recorded in the database that disappeared from
                         one source path and reappeared at another with the
                         same size and content (the new file is hashed and
                         compared to the recorded chunk hashes) is renamed at
                         the destination instead of being copied again
  --move-detection-check-date
                         Only consider move candidates whose last-write time
                         also matches the record. Cuts down how many new files
                         must be hashed, but misses moves done by tools that
                         rewrite write times (e.g. Explorer copies)
  --exclude-file <p>     File to exclude (repeatable; absolute or relative to
                         the source root)
  --exclude-folder <p>   Folder subtree to exclude (repeatable)
  --mt                   Enable multithreaded copying (default: off)
  --threads <n>          Max worker threads, 1-32 (default 8; needs --mt)
  --folder-logs          Instead of a line per file, print one line per folder
                         checked with the number of files copied from it
  --ntfs-map-origin      Read the source volume's NTFS USN change journal to
                         find what changed since the last run and visit only
                         that, instead of walking the whole source tree. The
                         journal position is kept in the database. Needs
                         administrator rights; the first run, or any run where
                         the journal cannot be trusted (non-NTFS source,
                         wrapped journal, changed exclude list, a previous
                         failed run), does a full scan automatically

Raw image copies (--drive to a .vhdx, or --partition):
  A .vhdx destination switches --drive to a raw sector copy of the whole
  physical disk (a drive letter names the disk that contains that volume);
  --partition images a single volume, wrapped in a GPT partition table so the
  image mounts with a drive letter. Both need an elevated console. The copy
  is taken from VSS snapshots where possible (removable drives are read
  live), skips unallocated clusters (NTFS, FAT32, exFAT) and zeroes the
  pagefile family. Later runs against the same database read
  only the source and rewrite only the chunks that changed; if the .vhdx was
  modified by anything else in between (e.g. mounted read-write) every chunk
  is rewritten, so mount images read-only. Not valid with raw images:
  --mirror, --no-move-detection, --exclude-*, --folder-logs,
  --ntfs-map-origin; --chunk-size must be a multiple of 4K.

Notes:
  - A --drive destination may be a drive or a folder; the source drive's
    contents are copied into it. A destination inside the source tree is
    rejected unless it is covered by --exclude-folder.
  - Only local drives are supported (no network drives or UNC paths).
  - Empty folders are always copied; symbolic links, junctions and other
    reparse points are copied as links, never followed.
  - Attributes, timestamps and security data are always copied.)");
}

namespace {

bool parse_int(const wchar_t* s, long min, long max, long& out) {
    wchar_t* end = nullptr;
    const long v = std::wcstol(s, &end, 10);
    if (!end || *end != L'\0' || v < min || v > max) return false;
    out = v;
    return true;
}

// Byte count with an optional K or M suffix (KiB/MiB).
bool parse_size(const wchar_t* s, std::uint64_t& out) {
    wchar_t* end = nullptr;
    const unsigned long long v = std::wcstoull(s, &end, 10);
    if (end == s || v == 0 || v > (1ull << 40)) return false;
    std::uint64_t mult = 1;
    if (*end == L'K' || *end == L'k') { mult = 1024; ++end; }
    else if (*end == L'M' || *end == L'm') { mult = 1024 * 1024; ++end; }
    if (*end != L'\0') return false;
    out = v * mult;
    return true;
}

bool fail(const std::wstring& msg) {
    log_error(msg + L" (use --help for usage)");
    return false;
}

} // namespace

std::optional<Options> parse_command_line(int argc, wchar_t** argv) {
    if (argc < 2) {
        print_usage();
        return std::nullopt;
    }

    Options opt;
    bool mode_set = false;
    std::vector<std::wstring> positional;

    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        auto next_value = [&](const wchar_t* name) -> const wchar_t* {
            if (i + 1 >= argc) {
                fail(std::format(L"{} requires a value", name));
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            print_usage();
            return std::nullopt;
        } else if (arg == L"--file" || arg == L"--folder" || arg == L"--drive" ||
                   arg == L"--partition") {
            if (mode_set) {
                fail(L"only one of --file/--folder/--drive/--partition is allowed");
                return std::nullopt;
            }
            mode_set = true;
            opt.mode = arg == L"--file"     ? Mode::File
                       : arg == L"--folder" ? Mode::Folder
                       : arg == L"--drive"  ? Mode::Drive
                                            : Mode::PartitionImage;
        } else if (arg == L"--db") {
            const wchar_t* v = next_value(L"--db");
            if (!v) return std::nullopt;
            opt.db_path = v;
        } else if (arg == L"--make-db") {
            opt.make_db_only = true;
        } else if (arg == L"--chunk-size") {
            const wchar_t* v = next_value(L"--chunk-size");
            if (!v) return std::nullopt;
            std::uint64_t bytes = 0;
            if (!parse_size(v, bytes) || bytes < 4ull * 1024 || bytes > 64ull * 1024 * 1024) {
                fail(L"--chunk-size must be between 4K and 64M (e.g. 65536, 512K, 4M)");
                return std::nullopt;
            }
            opt.chunk_size = bytes;
        } else if (arg == L"--mirror") {
            opt.mirror = true;
        } else if (arg == L"--no-move-detection") {
            opt.move_detection = false;
        } else if (arg == L"--move-detection-check-date") {
            opt.move_detection_check_date = true;
        } else if (arg == L"--exclude-file") {
            const wchar_t* v = next_value(L"--exclude-file");
            if (!v) return std::nullopt;
            opt.exclude_files.emplace_back(v);
        } else if (arg == L"--exclude-folder") {
            const wchar_t* v = next_value(L"--exclude-folder");
            if (!v) return std::nullopt;
            opt.exclude_folders.emplace_back(v);
        } else if (arg == L"--no-file-logs") {
            opt.file_logs = false;
        } else if (arg == L"--folder-logs") {
            opt.folder_logs = true;
        } else if (arg == L"--mt") {
            opt.multithread = true;
        } else if (arg == L"--ntfs-map-origin") {
            opt.ntfs_map_origin = true;
        } else if (arg == L"--threads") {
            const wchar_t* v = next_value(L"--threads");
            if (!v) return std::nullopt;
            long n = 0;
            if (!parse_int(v, 1, 32, n)) { fail(L"--threads must be between 1 and 32"); return std::nullopt; }
            opt.max_threads = static_cast<int>(n);
        } else if (arg == L"--max-tries") {
            const wchar_t* v = next_value(L"--max-tries");
            if (!v) return std::nullopt;
            long n = 0;
            if (!parse_int(v, 1, 1000, n)) { fail(L"--max-tries must be between 1 and 1000"); return std::nullopt; }
            opt.max_tries = static_cast<int>(n);
        } else if (arg.starts_with(L"-")) {
            fail(std::format(L"unknown option: {}", arg));
            return std::nullopt;
        } else {
            positional.emplace_back(arg);
        }
    }

    if (!mode_set) {
        fail(L"one of --file, --folder, --drive or --partition is required");
        return std::nullopt;
    }

    if (positional.empty()) { fail(L"a source path is required"); return std::nullopt; }
    if (positional.size() > 2) { fail(L"too many path arguments"); return std::nullopt; }
    opt.source = positional[0];
    if (positional.size() == 2) opt.destination = positional[1];

    if (opt.destination.empty() && !opt.make_db_only) {
        fail(L"a destination path is required unless --make-db is used");
        return std::nullopt;
    }
    if (opt.make_db_only && opt.mirror) {
        fail(L"--mirror cannot be combined with --make-db");
        return std::nullopt;
    }
    if (!opt.move_detection && opt.move_detection_check_date) {
        fail(L"--move-detection-check-date cannot be combined with --no-move-detection");
        return std::nullopt;
    }

    if (opt.mode == Mode::File) {
        if (opt.mirror) { fail(L"--mirror is only valid with --folder or --drive"); return std::nullopt; }
        if (!opt.move_detection || opt.move_detection_check_date) {
            fail(L"--no-move-detection and --move-detection-check-date are only valid "
                 L"with --folder or --drive");
            return std::nullopt;
        }
        if (!opt.exclude_files.empty() || !opt.exclude_folders.empty()) {
            fail(L"--exclude-file/--exclude-folder are only valid with --folder or --drive");
            return std::nullopt;
        }
        if (opt.multithread) { fail(L"--mt is only valid with --folder or --drive"); return std::nullopt; }
        if (opt.folder_logs) {
            fail(L"--folder-logs is only valid with --folder or --drive");
            return std::nullopt;
        }
        if (opt.ntfs_map_origin) {
            fail(L"--ntfs-map-origin is only valid with --folder or --drive");
            return std::nullopt;
        }
    }

    // Folder logging replaces the per-file lines.
    if (opt.folder_logs) opt.file_logs = false;

    auto is_drive = [](const std::filesystem::path& p) {
        const std::wstring& s = p.native();
        return (s.size() == 2 || (s.size() == 3 && s[2] == L'\\')) &&
               ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z')) && s[1] == L':';
    };
    auto lower = [](std::wstring s) {
        for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
        return s;
    };
    auto is_vhdx = [&](const std::filesystem::path& p) {
        return lower(p.extension().native()) == L".vhdx";
    };

    if (opt.mode == Mode::Drive) {
        const std::wstring low = lower(opt.source.native());
        const bool digits = !low.empty() && std::ranges::all_of(low, [](wchar_t c) {
                                return c >= L'0' && c <= L'9';
                            });
        constexpr std::wstring_view kPhys = LR"(\\.\physicaldrive)";
        const bool phys = low.starts_with(kPhys) || low.starts_with(LR"(\\?\physicaldrive)");
        if (digits || phys || (!opt.destination.empty() && is_vhdx(opt.destination))) {
            // Raw image of a whole physical disk into a .vhdx.
            if (digits || phys) {
                long n = 0;
                const std::wstring num = digits ? low : low.substr(kPhys.size());
                if (!parse_int(num.c_str(), 0, 999, n)) {
                    fail(L"--drive disk number must be between 0 and 999");
                    return std::nullopt;
                }
                opt.source = std::format(LR"(\\.\PhysicalDrive{})", n);
            } else if (is_drive(opt.source)) {
                opt.source = std::wstring{opt.source.native()[0], L':', L'\\'};
            } else {
                fail(L"--drive image source must be a drive letter, a disk number or "
                     LR"(\\.\PhysicalDriveN)");
                return std::nullopt;
            }
            if (!opt.destination.empty() && !is_vhdx(opt.destination)) {
                fail(L"a raw disk source needs a .vhdx destination file");
                return std::nullopt;
            }
            opt.mode = Mode::DriveImage;
        } else {
            if (!is_drive(opt.source)) {
                fail(L"--drive source must be a drive like X: or X:\\");
                return std::nullopt;
            }
            // Normalize to X:\ so the path is a proper root, not drive-relative.
            opt.source = std::wstring{opt.source.native()[0], L':', L'\\'};
            // The destination may be a drive or a folder; only a drive spec needs
            // the same root normalization.
            if (!opt.destination.empty() && is_drive(opt.destination))
                opt.destination = std::wstring{opt.destination.native()[0], L':', L'\\'};
        }
    }

    if (opt.mode == Mode::PartitionImage) {
        const std::wstring low = lower(opt.source.native());
        if (is_drive(opt.source)) {
            opt.source = std::wstring{opt.source.native()[0], L':', L'\\'};
        } else if (!low.starts_with(LR"(\\?\volume{)") &&
                   !low.starts_with(LR"(\\.\harddiskvolume)")) {
            fail(L"--partition source must be a drive letter, "
                 LR"(\\?\Volume{GUID}\ or \\.\HarddiskVolumeN)");
            return std::nullopt;
        }
        if (!opt.destination.empty() && !is_vhdx(opt.destination)) {
            fail(L"--partition needs a .vhdx destination file");
            return std::nullopt;
        }
    }

    if (opt.mode == Mode::DriveImage || opt.mode == Mode::PartitionImage) {
        const wchar_t* with =
            opt.mode == Mode::DriveImage ? L"--drive image copies" : L"--partition";
        if (opt.mirror) {
            fail(std::format(L"--mirror is not valid with {}", with));
            return std::nullopt;
        }
        if (!opt.move_detection || opt.move_detection_check_date) {
            fail(std::format(L"move detection options are not valid with {}", with));
            return std::nullopt;
        }
        if (!opt.exclude_files.empty() || !opt.exclude_folders.empty()) {
            fail(std::format(L"--exclude-file/--exclude-folder are not valid with {}", with));
            return std::nullopt;
        }
        if (opt.folder_logs) {
            fail(std::format(L"--folder-logs is not valid with {}", with));
            return std::nullopt;
        }
        if (opt.ntfs_map_origin) {
            fail(std::format(L"--ntfs-map-origin is not valid with {}", with));
            return std::nullopt;
        }
        if (opt.chunk_size % 4096 != 0) {
            fail(L"--chunk-size must be a multiple of 4K for raw image copies");
            return std::nullopt;
        }
        if (opt.make_db_only && opt.destination.empty() && !opt.db_path) {
            fail(L"--make-db without a destination needs --db for raw image copies");
            return std::nullopt;
        }
    }

    return opt;
}

} // namespace tc
