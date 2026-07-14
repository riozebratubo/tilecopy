#include "engine.h"

#include "chunkdb.h"
#include "delta.h"
#include "fsmeta.h"
#include "reparse.h"
#include "util.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <format>
#include <set>
#include <thread>
#include <vector>

namespace tc {

namespace fs = std::filesystem;

namespace {

struct CopyStats {
    std::atomic<std::uint64_t> copied{0}, skipped{0}, moved{0}, failed{0}, links{0},
        deleted{0}, hashed{0}, bytes_written{0}, bytes_skipped{0};
};

struct FileTask {
    fs::path src;
    fs::path dst;
    std::wstring rel;
    FileRecord* record = nullptr;
    bool had_record = false;
    std::uint64_t src_size = 0;       // from the directory walk
    std::int64_t src_write_time = 0;  // FILETIME from the directory walk
};

std::wstring norm_path(const fs::path& p) {
    std::wstring s = extended_path(p);
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

struct Excluder {
    std::vector<std::wstring> files;   // normalized absolute paths
    std::vector<std::wstring> folders; // normalized absolute paths

    void build(const Options& opt) {
        auto resolve = [&](const fs::path& p) {
            return norm_path(p.is_absolute() ? p : opt.source / p);
        };
        for (const auto& p : opt.exclude_files) files.push_back(resolve(p));
        for (const auto& p : opt.exclude_folders) folders.push_back(resolve(p));
    }

    bool excluded(const std::wstring& norm) const {
        for (const auto& f : files)
            if (norm == f) return true;
        for (const auto& d : folders)
            if (norm == d || (norm.size() > d.size() && norm.starts_with(d) &&
                              norm[d.size()] == L'\\'))
                return true;
        return false;
    }
};

struct Job {
    const Options* opt = nullptr;
    fs::path src_root, dst_root;
    ChunkDatabase db;
    fs::path db_file;
    std::wstring db_norm, db_tmp_norm;
    Excluder excl;
    std::vector<FileTask> tasks;
    std::vector<std::pair<fs::path, fs::path>> dir_meta; // post-order (children first)
    std::set<std::wstring> seen_keys;
    CopyStats stats;
    bool copying = true; // false with --make-db
};

// NTFS metadata files, journals and Windows-managed root entries that must
// never be copied off (or onto) a live system drive.
bool is_drive_system_entry(const wchar_t* name) {
    static constexpr const wchar_t* kNames[] = {
        L"$Recycle.Bin", L"$RECYCLE.BIN", L"System Volume Information",
        L"$MFT", L"$MFTMirr", L"$LogFile", L"$Volume", L"$AttrDef", L"$Bitmap",
        L"$Boot", L"$BadClus", L"$Secure", L"$UpCase", L"$Extend", L"$Txf", L"$TxfLog",
        L"pagefile.sys", L"swapfile.sys", L"hiberfil.sys", L"DumpStack.log.tmp",
    };
    return std::ranges::any_of(kNames, [&](const wchar_t* n) { return _wcsicmp(n, name) == 0; });
}

bool check_local_drive(const fs::path& p, std::wstring& err) {
    std::error_code ec;
    const fs::path abs = fs::absolute(p, ec);
    const std::wstring root = abs.root_path().native();
    if (root.starts_with(L"\\\\")) {
        err = std::format(L"{}: UNC/network paths are not supported", p.native());
        return false;
    }
    switch (::GetDriveTypeW(root.c_str())) {
    case DRIVE_REMOTE:
        err = std::format(L"{}: network drives are not supported", p.native());
        return false;
    case DRIVE_UNKNOWN:
    case DRIVE_NO_ROOT_DIR:
        err = std::format(L"{}: not a valid local drive", p.native());
        return false;
    default:
        return true;
    }
}

// The database lives next to the destination (it describes what was last
// copied there); with --make-db and no destination it derives from the source.
fs::path resolve_db_path(const Options& opt, const fs::path& effective_dest) {
    if (opt.db_path) return *opt.db_path;
    const fs::path& base = effective_dest.empty() ? opt.source : effective_dest;
    switch (opt.mode) {
    case Mode::File: {
        fs::path p = base;
        p += L".tcdb";
        return p;
    }
    case Mode::Folder:
    case Mode::Drive:
        return base / L"tilecopy.tcdb";
    }
    return {};
}

bool delete_plain_file(const fs::path& p) {
    const std::wstring ext = extended_path(p);
    ::SetFileAttributesW(ext.c_str(), FILE_ATTRIBUTE_NORMAL);
    return ::DeleteFileW(ext.c_str()) != 0;
}

// Recursively removes whatever lives at p. Reparse points are removed as
// links; their targets are never entered.
bool remove_tree(const fs::path& p, DWORD attrs, std::uint64_t& removed) {
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (!delete_reparse_point(p, (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)) return false;
        ++removed;
        return true;
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!delete_plain_file(p)) return false;
        ++removed;
        return true;
    }

    std::wstring pattern = extended_path(p);
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd;
    HANDLE hf = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                   FindExSearchNameMatch, nullptr, 0);
    bool ok = true;
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            ok &= remove_tree(p / fd.cFileName, fd.dwFileAttributes, removed);
        } while (::FindNextFileW(hf, &fd));
        ::FindClose(hf);
    }
    const std::wstring ext = extended_path(p);
    ::SetFileAttributesW(ext.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!::RemoveDirectoryW(ext.c_str())) return false;
    ++removed;
    return ok;
}

// Makes sure dst can receive a directory: removes any file or link in the way.
bool ensure_directory(const fs::path& dst, std::wstring& err) {
    const std::wstring ext = extended_path(dst);
    const DWORD attrs = ::GetFileAttributesW(ext.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT))
            return true;
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
            delete_reparse_point(dst, (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
        else
            delete_plain_file(dst);
    }
    if (!::CreateDirectoryW(ext.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
        err = std::format(L"cannot create directory {}: {}", dst.native(),
                          win32_error_message(::GetLastError()));
        return false;
    }
    return true;
}

// Clears a destination slot that must become a plain file: removes a
// directory or link occupying the path. A plain file is left for delta reuse.
void prepare_file_slot(const fs::path& dst) {
    const DWORD attrs = ::GetFileAttributesW(extended_path(dst).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        delete_reparse_point(dst, (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } else if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        std::uint64_t removed = 0;
        remove_tree(dst, attrs, removed);
    }
}

void handle_link(Job& job, const fs::path& src, const fs::path& dst, bool is_dir,
                 const std::wstring& rel) {
    if (!job.copying) return; // links carry no chunk data; nothing for the DB
    std::wstring err;
    bool ok = false;
    for (int attempt = 1; attempt <= job.opt->max_tries && !ok; ++attempt) {
        ok = clone_reparse_point(src, dst, is_dir, err);
        if (!ok && attempt < job.opt->max_tries) ::Sleep(250);
    }
    if (ok) {
        std::wstring merr;
        copy_metadata(src, dst, is_dir, merr);
        job.stats.links.fetch_add(1, std::memory_order_relaxed);
        log_info(L"link     " + rel);
    } else {
        job.stats.failed.fetch_add(1, std::memory_order_relaxed);
        log_error(std::format(L"link {} failed: {}", rel, err));
    }
}

void walk_dir(Job& job, const fs::path& sdir, const fs::path& ddir, const std::wstring& rel,
              int depth) {
    std::wstring pattern = extended_path(sdir);
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW fd;
    HANDLE hf = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (hf == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_NO_MORE_FILES) {
            job.stats.failed.fetch_add(1, std::memory_order_relaxed);
            log_error(std::format(L"cannot enumerate {}: {}", sdir.native(),
                                  win32_error_message(err)));
        }
        return;
    }

    do {
        const wchar_t* name = fd.cFileName;
        if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) continue;
        if (depth == 0 && job.opt->mode == Mode::Drive && is_drive_system_entry(name)) continue;

        const fs::path spath = sdir / name;
        const std::wstring snorm = norm_path(spath);
        if (snorm == job.db_norm || snorm == job.db_tmp_norm) continue;
        if (job.excl.excluded(snorm)) continue;

        const fs::path dpath = ddir / name;
        if (job.copying) {
            // Never let a copied file land on top of the database itself.
            const std::wstring dnorm = norm_path(dpath);
            if (dnorm == job.db_norm || dnorm == job.db_tmp_norm) continue;
        }
        const std::wstring child_rel = rel.empty() ? std::wstring(name)
                                                   : rel + L"\\" + name;
        const bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            handle_link(job, spath, dpath, is_dir, child_rel);
        } else if (is_dir) {
            std::wstring err;
            if (job.copying && !ensure_directory(dpath, err)) {
                job.stats.failed.fetch_add(1, std::memory_order_relaxed);
                log_error(err);
                continue; // no point descending into it
            }
            walk_dir(job, spath, dpath, child_rel, depth + 1);
            job.dir_meta.emplace_back(spath, dpath);
        } else {
            auto [it, inserted] = job.db.files.try_emplace(child_rel);
            job.seen_keys.insert(child_rel);
            const std::uint64_t fsize =
                (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            job.tasks.push_back({spath, dpath, child_rel, &it->second, !inserted, fsize,
                                 filetime_to_i64(fd.ftLastWriteTime)});
        }
    } while (::FindNextFileW(hf, &fd));
    ::FindClose(hf);
}

void run_task(Job& job, FileTask& t) {
    if (job.copying) prepare_file_slot(t.dst);

    DeltaResult r;
    for (int attempt = 1; attempt <= job.opt->max_tries; ++attempt) {
        r = delta_copy_file(t.src, t.dst, *t.record, t.had_record, !job.copying,
                            job.opt->chunk_size);
        if (r.ok) break;
        if (attempt < job.opt->max_tries) ::Sleep(250);
    }

    const std::wstring& label = t.rel.empty() ? t.src.native() : t.rel;
    if (!r.ok) {
        job.stats.failed.fetch_add(1, std::memory_order_relaxed);
        log_error(std::format(L"copy {} failed: {}", label, r.error));
        return;
    }

    if (!job.copying) {
        job.stats.hashed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (r.skipped) {
        job.stats.skipped.fetch_add(1, std::memory_order_relaxed);
        job.stats.bytes_skipped.fetch_add(r.bytes_skipped, std::memory_order_relaxed);
        return;
    }

    std::wstring merr;
    if (!copy_metadata(t.src, t.dst, false, merr))
        log_error(std::format(L"metadata for {}: {}", label, merr));

    job.stats.copied.fetch_add(1, std::memory_order_relaxed);
    job.stats.bytes_written.fetch_add(r.bytes_written, std::memory_order_relaxed);
    job.stats.bytes_skipped.fetch_add(r.bytes_skipped, std::memory_order_relaxed);
    if (r.delta_used)
        log_info(std::format(L"copied   {} (delta: {} written, {} unchanged)", label,
                             human_bytes(r.bytes_written), human_bytes(r.bytes_skipped)));
    else
        log_info(std::format(L"copied   {} ({})", label, human_bytes(r.bytes_written)));
}

void process_tasks(Job& job) {
    if (job.tasks.empty()) return;
    const Options& opt = *job.opt;

    if (!opt.multithread || opt.max_threads <= 1 || job.tasks.size() == 1) {
        for (auto& t : job.tasks) run_task(job, t);
        return;
    }

    std::atomic<size_t> next{0};
    const size_t workers = std::min<size_t>(opt.max_threads, job.tasks.size());
    std::vector<std::jthread> pool;
    pool.reserve(workers);
    for (size_t w = 0; w < workers; ++w) {
        pool.emplace_back([&] {
            for (;;) {
                const size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= job.tasks.size()) break;
                run_task(job, job.tasks[i]);
            }
        });
    }
}

// Reads src sequentially and fills `out` with the SHA-256 of each chunk.
// `out` is left empty on failure so partial results can never be matched.
bool hash_file_chunks(const fs::path& src, std::uint64_t chunk_size, std::vector<Sha256>& out) {
    HANDLE h = ::CreateFileW(extended_path(src).c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    thread_local Sha256Hasher hasher;
    bool ok = hasher.valid();
    std::vector<std::uint8_t> buf(chunk_size);
    while (ok) {
        DWORD read = 0;
        if (!::ReadFile(h, buf.data(), static_cast<DWORD>(chunk_size), &read, nullptr))
            ok = false;
        else if (read == 0)
            break;
        else
            out.push_back(hasher.hash(buf.data(), read));
    }
    ::CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

// Detects source files that moved since the last run: a database record whose
// path truly vanished from the source paired with a new source file of the
// same size whose content matches — the new file is hashed and its chunk
// hashes must equal the record's. Write times are ignored by default because
// tools like Explorer rewrite them when copying; --move-detection-check-date
// requires them to match too, which shrinks the set of files to hash. On a
// match the destination copy is renamed instead of re-copied and the record
// is adopted under the new path, so later runs delta against it as usual.
void detect_moves(Job& job) {
    const Options& opt = *job.opt;
    const bool check_date = opt.move_detection_check_date;
    using Key = std::pair<std::uint64_t, std::int64_t>; // size, write time (0 unless checked)

    struct Orphan {
        std::wstring rel;
        FileRecord* rec = nullptr;
    };
    std::map<Key, std::vector<Orphan>> orphans;
    for (auto& [rel, rec] : job.db.files) {
        if (rec.file_size == 0 || job.seen_keys.contains(rel)) continue;
        // A record is only a move source when the file is truly gone from the
        // source; newly excluded entries and subtrees that failed to enumerate
        // also miss seen_keys and must not have their destinations renamed away.
        if (::GetFileAttributesW(extended_path(job.src_root / rel).c_str()) !=
            INVALID_FILE_ATTRIBUTES)
            continue;
        orphans[{rec.file_size, check_date ? rec.source_write_time : 0}].push_back({rel, &rec});
    }
    if (orphans.empty()) return;

    std::vector<FileTask*> cands;
    for (auto& t : job.tasks) {
        if (t.had_record || t.src_size == 0) continue;
        const auto it = orphans.find({t.src_size, check_date ? t.src_write_time : 0});
        if (it != orphans.end() && !it->second.empty()) cands.push_back(&t);
    }
    if (cands.empty()) return;

    // Hash every candidate once up front (in parallel with --mt). A candidate
    // that ends up not matching is read again by the copy path; that is the
    // price of content-verified pairing and only hits new files that happen
    // to share a size with a vanished one.
    std::vector<std::vector<Sha256>> cand_chunks(cands.size());
    {
        const size_t workers = (opt.multithread && opt.max_threads > 1)
                                   ? std::min<size_t>(opt.max_threads, cands.size())
                                   : 1;
        if (workers <= 1) {
            for (size_t i = 0; i < cands.size(); ++i)
                hash_file_chunks(cands[i]->src, opt.chunk_size, cand_chunks[i]);
        } else {
            std::atomic<size_t> next{0};
            std::vector<std::jthread> pool;
            pool.reserve(workers);
            for (size_t w = 0; w < workers; ++w) {
                pool.emplace_back([&] {
                    for (;;) {
                        const size_t i = next.fetch_add(1, std::memory_order_relaxed);
                        if (i >= cands.size()) break;
                        hash_file_chunks(cands[i]->src, opt.chunk_size, cand_chunks[i]);
                    }
                });
            }
        }
    }

    for (size_t i = 0; i < cands.size(); ++i) {
        FileTask& t = *cands[i];
        if (cand_chunks[i].empty()) continue; // unreadable; the copy path reports it
        auto& bucket = orphans[{t.src_size, check_date ? t.src_write_time : 0}];

        for (auto it = bucket.begin(); it != bucket.end();) {
            if (it->rec->chunks != cand_chunks[i]) {
                ++it;
                continue;
            }

            // Only rename what the record still describes: a plain file of
            // the recorded size. Anything else is of no use to any candidate.
            const fs::path old_dst = job.dst_root / it->rel;
            const std::wstring old_ext = extended_path(old_dst);
            WIN32_FILE_ATTRIBUTE_DATA fad;
            const bool dst_good =
                ::GetFileAttributesExW(old_ext.c_str(), GetFileExInfoStandard, &fad) &&
                !(fad.dwFileAttributes &
                  (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                ((static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow) ==
                    it->rec->file_size;
            if (!dst_good) {
                it = bucket.erase(it);
                continue;
            }

            const std::wstring new_ext = extended_path(t.dst);
            if (norm_path(old_dst) != norm_path(t.dst)) {
                // Clear whatever occupies the target slot; it has no record,
                // so a full copy was inevitable there anyway.
                prepare_file_slot(t.dst);
                if (::GetFileAttributesW(new_ext.c_str()) != INVALID_FILE_ATTRIBUTES)
                    delete_plain_file(t.dst);
            }
            if (!::MoveFileExW(old_ext.c_str(), new_ext.c_str(), 0)) {
                log_info(std::format(L"move {} -> {} failed ({}); copying instead", it->rel,
                                     t.rel, win32_error_message(::GetLastError())));
                break; // target slot problem; let the normal copy handle it
            }

            const std::wstring old_rel = it->rel;
            *t.record = std::move(*it->rec);
            // Content equality was verified against the record, so the new
            // write time can be adopted too; run_task then skips the file
            // without reading it again.
            t.record->source_write_time = t.src_write_time;
            t.had_record = true;
            job.db.files.erase(old_rel);
            bucket.erase(it);
            job.stats.moved.fetch_add(1, std::memory_order_relaxed);
            log_info(std::format(L"moved    {} -> {}", old_rel, t.rel));
            break;
        }
    }
}

void mirror_dir(Job& job, const fs::path& ddir, const fs::path& sdir, int depth) {
    std::wstring pattern = extended_path(ddir);
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW fd;
    HANDLE hf = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (hf == INVALID_HANDLE_VALUE) return;

    do {
        const wchar_t* name = fd.cFileName;
        if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) continue;
        if (depth == 0 && job.opt->mode == Mode::Drive && is_drive_system_entry(name)) continue;

        const fs::path dpath = ddir / name;
        const std::wstring dnorm = norm_path(dpath);
        if (dnorm == job.db_norm || dnorm == job.db_tmp_norm) continue;

        const fs::path spath = sdir / name;
        // Excluded entries are defined in source terms; map this destination
        // entry back to its source counterpart and leave it untouched.
        if (job.excl.excluded(norm_path(spath))) continue;

        const DWORD sattrs = ::GetFileAttributesW(extended_path(spath).c_str());
        if (sattrs == INVALID_FILE_ATTRIBUTES) {
            std::uint64_t removed = 0;
            if (remove_tree(dpath, fd.dwFileAttributes, removed)) {
                log_info(L"deleted  " + dpath.native());
            } else {
                job.stats.failed.fetch_add(1, std::memory_order_relaxed);
                log_error(std::format(L"cannot delete {}: {}", dpath.native(),
                                      win32_error_message(::GetLastError())));
            }
            job.stats.deleted.fetch_add(removed, std::memory_order_relaxed);
            continue;
        }

        const bool src_dir = (sattrs & FILE_ATTRIBUTE_DIRECTORY) &&
                             !(sattrs & FILE_ATTRIBUTE_REPARSE_POINT);
        const bool dst_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                             !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        if (src_dir && dst_dir) mirror_dir(job, dpath, spath, depth + 1);
    } while (::FindNextFileW(hf, &fd));
    ::FindClose(hf);
}

// "time spent: <days>d <hh>:<mm>:<ss.ss> or <total_ms>ms"
std::wstring format_time_spent(std::chrono::milliseconds elapsed) {
    const auto total_ms = elapsed.count();
    const auto days = total_ms / 86'400'000;
    const auto hours = (total_ms / 3'600'000) % 24;
    const auto minutes = (total_ms / 60'000) % 60;
    const double seconds = (total_ms % 60'000) / 1000.0;
    return std::format(L"time spent: {}d {:02}:{:02}:{:05.2f} or {}ms",
                       days, hours, minutes, seconds, total_ms);
}

int run_single_file(Job& job) {
    const Options& opt = *job.opt;
    const DWORD sattrs = ::GetFileAttributesW(extended_path(opt.source).c_str());
    if (sattrs == INVALID_FILE_ATTRIBUTES) {
        log_error(std::format(L"source file not found: {}", opt.source.native()));
        return 1;
    }
    if ((sattrs & FILE_ATTRIBUTE_DIRECTORY) && !(sattrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        log_error(L"source is a directory; use --folder");
        return 1;
    }

    // run() already resolved a directory destination to dst/<source name>.
    fs::path dst = job.dst_root;
    if (job.copying && dst.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
    }

    if (sattrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        handle_link(job, opt.source, dst, (sattrs & FILE_ATTRIBUTE_DIRECTORY) != 0,
                    opt.source.native());
    } else {
        auto [it, inserted] = job.db.files.try_emplace(L"");
        job.seen_keys.insert(L"");
        job.tasks.push_back({opt.source, dst, L"", &it->second, !inserted});
        run_task(job, job.tasks.front());
    }
    return 0;
}

} // namespace

int run(const Options& opt) {
    std::wstring err;
    if (!check_local_drive(opt.source, err)) { log_error(err); return 1; }
    if (!opt.destination.empty() && !check_local_drive(opt.destination, err)) {
        log_error(err);
        return 1;
    }

    Job job;
    job.opt = &opt;
    job.copying = !opt.make_db_only;
    job.src_root = opt.source;
    job.dst_root = opt.destination;
    // A file destination that is an existing directory means "copy into it
    // under the source name"; resolve that now so the default DB path (which
    // sits next to the destination) is derived from the real target.
    if (opt.mode == Mode::File && !job.dst_root.empty()) {
        const DWORD dattrs = ::GetFileAttributesW(extended_path(job.dst_root).c_str());
        if (dattrs != INVALID_FILE_ATTRIBUTES && (dattrs & FILE_ATTRIBUTE_DIRECTORY) &&
            !(dattrs & FILE_ATTRIBUTE_REPARSE_POINT))
            job.dst_root /= opt.source.filename();
    }
    job.excl.build(opt);
    // A destination inside the source tree would be walked while being
    // written to, copying the copy into itself; only an excluded destination
    // (which the walk skips) is safe.
    if (job.copying && opt.mode != Mode::File) {
        const std::wstring src_norm = norm_path(job.src_root);
        const std::wstring dst_norm = norm_path(job.dst_root);
        const bool inside = dst_norm == src_norm ||
                            (dst_norm.starts_with(src_norm) &&
                             (src_norm.back() == L'\\' || dst_norm[src_norm.size()] == L'\\'));
        if (inside && !job.excl.excluded(dst_norm)) {
            log_error(std::format(L"destination {} is inside the source {}; exclude it with "
                                  L"--exclude-folder to copy anyway",
                                  opt.destination.native(), opt.source.native()));
            return 1;
        }
    }
    job.db_file = resolve_db_path(opt, job.dst_root);
    job.db_norm = norm_path(job.db_file);
    job.db_tmp_norm = job.db_norm + L".tmp";

    const bool had_db = ChunkDatabase::load(job.db_file, opt.chunk_size, job.db);
    job.db.chunk_size = opt.chunk_size;
    log_info(std::format(L"database: {} ({})", job.db_file.native(),
                         had_db ? L"loaded" : L"new"));

    const auto started = std::chrono::steady_clock::now();

    if (opt.mode == Mode::File) {
        const int rc = run_single_file(job);
        if (rc != 0) return rc;
    } else {
        const DWORD sattrs = ::GetFileAttributesW(extended_path(opt.source).c_str());
        if (sattrs == INVALID_FILE_ATTRIBUTES || !(sattrs & FILE_ATTRIBUTE_DIRECTORY)) {
            log_error(std::format(L"source folder not found: {}", opt.source.native()));
            return 1;
        }
        if (job.copying) {
            std::error_code ec;
            fs::create_directories(job.dst_root, ec);
            if (ec) {
                log_error(std::format(L"cannot create destination {}", job.dst_root.native()));
                return 1;
            }
        }

        walk_dir(job, job.src_root, job.dst_root, L"", 0);
        if (job.copying && opt.move_detection) detect_moves(job);
        process_tasks(job);

        if (job.copying) {
            // Children were pushed before their parents, so timestamps set on
            // a directory are not disturbed afterwards.
            std::wstring merr;
            for (const auto& [s, d] : job.dir_meta)
                if (!copy_metadata(s, d, true, merr))
                    log_error(std::format(L"metadata for {}: {}", d.native(), merr));
            if (opt.mode == Mode::Folder)
                copy_metadata(job.src_root, job.dst_root, true, merr);
        }

        if (job.copying && opt.mirror) mirror_dir(job, job.dst_root, job.src_root, 0);

        // Forget files that no longer exist in the source.
        std::erase_if(job.db.files,
                      [&](const auto& kv) { return !job.seen_keys.contains(kv.first); });
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              started);

    std::wstring dberr;
    bool db_saved = job.db.save(job.db_file, dberr);
    if (!db_saved) log_error(dberr);

    const auto& s = job.stats;
    if (opt.make_db_only) {
        log_info(std::format(L"done: {} file(s) hashed into database, {} failed",
                             s.hashed.load(), s.failed.load()));
    } else {
        log_info(std::format(
            L"done: {} copied ({} written, {} unchanged), {} moved, {} up-to-date, "
            L"{} link(s), {} deleted, {} failed",
            s.copied.load(), human_bytes(s.bytes_written.load()),
            human_bytes(s.bytes_skipped.load()), s.moved.load(), s.skipped.load(),
            s.links.load(), s.deleted.load(), s.failed.load()));
    }
    log_info(format_time_spent(elapsed));

    if (s.failed.load() > 0 || !db_saved) return 2;
    return 0;
}

} // namespace tc
