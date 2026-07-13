#include "options.h"

#include "util.h"

#include <cwchar>
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
  tilecopy --file   <source-file> <dest-file>  [options]
  tilecopy --folder <source-dir>  <dest-dir>   [options]
  tilecopy --drive  <X:>          <Y:>         [options]
  tilecopy --file/--folder/--drive <source> --make-db [options]

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

Folder and drive options:
  --mirror               Delete destination entries that do not exist in the
                         source (excluded entries are left untouched)
  --exclude-file <p>     File to exclude (repeatable; absolute or relative to
                         the source root)
  --exclude-folder <p>   Folder subtree to exclude (repeatable)
  --mt                   Enable multithreaded copying (default: off)
  --threads <n>          Max worker threads, 1-32 (default 8; needs --mt)

Notes:
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
        } else if (arg == L"--file" || arg == L"--folder" || arg == L"--drive") {
            if (mode_set) { fail(L"only one of --file/--folder/--drive is allowed"); return std::nullopt; }
            mode_set = true;
            opt.mode = arg == L"--file" ? Mode::File : arg == L"--folder" ? Mode::Folder : Mode::Drive;
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
        } else if (arg == L"--exclude-file") {
            const wchar_t* v = next_value(L"--exclude-file");
            if (!v) return std::nullopt;
            opt.exclude_files.emplace_back(v);
        } else if (arg == L"--exclude-folder") {
            const wchar_t* v = next_value(L"--exclude-folder");
            if (!v) return std::nullopt;
            opt.exclude_folders.emplace_back(v);
        } else if (arg == L"--mt") {
            opt.multithread = true;
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

    if (!mode_set) { fail(L"one of --file, --folder or --drive is required"); return std::nullopt; }

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

    if (opt.mode == Mode::File) {
        if (opt.mirror) { fail(L"--mirror is only valid with --folder or --drive"); return std::nullopt; }
        if (!opt.exclude_files.empty() || !opt.exclude_folders.empty()) {
            fail(L"--exclude-file/--exclude-folder are only valid with --folder or --drive");
            return std::nullopt;
        }
        if (opt.multithread) { fail(L"--mt is only valid with --folder or --drive"); return std::nullopt; }
    }

    if (opt.mode == Mode::Drive) {
        auto is_drive = [](const std::filesystem::path& p) {
            const std::wstring& s = p.native();
            return (s.size() == 2 || (s.size() == 3 && s[2] == L'\\')) &&
                   ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z')) && s[1] == L':';
        };
        if (!is_drive(opt.source)) { fail(L"--drive source must be a drive like X: or X:\\"); return std::nullopt; }
        if (!opt.destination.empty() && !is_drive(opt.destination)) {
            fail(L"--drive destination must be a drive like Y: or Y:\\");
            return std::nullopt;
        }
        // Normalize to X:\ so the paths are proper roots, not drive-relative.
        opt.source = std::wstring{opt.source.native()[0], L':', L'\\'};
        if (!opt.destination.empty())
            opt.destination = std::wstring{opt.destination.native()[0], L':', L'\\'};
    }

    return opt;
}

} // namespace tc
