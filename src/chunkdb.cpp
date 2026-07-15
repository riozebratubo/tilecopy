#include "chunkdb.h"

#include "util.h"

#include <windows.h>

#include <format>
#include <fstream>

namespace tc {

namespace {

constexpr std::uint32_t kMagic = 0x42444354;   // "TCDB" little-endian
constexpr std::uint32_t kVersion = 1;      // no USN journal state
constexpr std::uint32_t kVersionUsn = 2;   // + UsnState block after the header
constexpr std::uint32_t kVersionImage = 3; // raw image record, no file records
constexpr std::uint32_t kMaxPathBytes = 1u << 20;

template <typename T>
bool read_pod(std::istream& in, T& v) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof v));
}

template <typename T>
void write_pod(std::ostream& out, const T& v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof v);
}

} // namespace

bool ChunkDatabase::load(const std::filesystem::path& db_file, std::uint64_t expected_chunk_size,
                         bool expect_image, ChunkDatabase& out) {
    std::ifstream in(db_file, std::ios::binary);
    if (!in) return false;

    std::uint32_t magic = 0, version = 0;
    std::uint64_t chunk_size = 0, file_count = 0;
    if (!read_pod(in, magic) || !read_pod(in, version) || !read_pod(in, chunk_size))
        return false;
    if (magic != kMagic ||
        (version != kVersion && version != kVersionUsn && version != kVersionImage) ||
        chunk_size != expected_chunk_size)
        return false;
    if ((version == kVersionImage) != expect_image) return false;

    ChunkDatabase db;
    db.chunk_size = chunk_size;
    if (version == kVersionImage) {
        std::uint8_t db_only = 0;
        std::uint64_t chunk_count = 0;
        if (!read_pod(in, db.image.kind) || !read_pod(in, db_only) ||
            !read_pod(in, db.image.source_id) || !read_pod(in, db.image.source_size) ||
            !read_pod(in, db.image.dest_size) || !read_pod(in, db.image.dest_write_time) ||
            !read_pod(in, chunk_count))
            return false;
        if (chunk_count != (db.image.source_size + chunk_size - 1) / chunk_size) return false;
        db.image.chunks.resize(chunk_count);
        if (chunk_count &&
            !in.read(reinterpret_cast<char*>(db.image.chunks.data()),
                     static_cast<std::streamsize>(chunk_count * sizeof(Sha256))))
            return false;
        db.image.db_only = db_only != 0;
        db.image.valid = true;
        out = std::move(db);
        return true;
    }
    if (version == kVersionUsn) {
        std::uint8_t db_only = 0;
        if (!read_pod(in, db_only) || !read_pod(in, db.usn.volume_serial) ||
            !read_pod(in, db.usn.journal_id) || !read_pod(in, db.usn.next_usn) ||
            !read_pod(in, db.usn.excludes_hash))
            return false;
        db.usn.valid = true;
        db.usn.db_only = db_only != 0;
    }
    if (!read_pod(in, file_count)) return false;
    for (std::uint64_t i = 0; i < file_count; ++i) {
        std::uint32_t path_bytes = 0;
        if (!read_pod(in, path_bytes) || path_bytes > kMaxPathBytes) return false;
        std::string path_utf8(path_bytes, '\0');
        if (path_bytes && !in.read(path_utf8.data(), path_bytes)) return false;

        FileRecord rec;
        std::uint64_t chunk_count = 0;
        if (!read_pod(in, rec.file_size) || !read_pod(in, rec.source_write_time) ||
            !read_pod(in, chunk_count))
            return false;
        // A record can never have more chunks than its size implies.
        if (chunk_count > rec.file_size / chunk_size + 1) return false;
        rec.chunks.resize(chunk_count);
        if (chunk_count &&
            !in.read(reinterpret_cast<char*>(rec.chunks.data()),
                     static_cast<std::streamsize>(chunk_count * sizeof(Sha256))))
            return false;

        db.files.emplace(utf8_to_wide(path_utf8), std::move(rec));
    }

    out = std::move(db);
    return true;
}

bool ChunkDatabase::save(const std::filesystem::path& db_file, std::wstring& error) const {
    std::filesystem::path tmp = db_file;
    tmp += L".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = std::format(L"cannot create database file {}", tmp.native());
            return false;
        }
        write_pod(out, kMagic);
        // A database without journal state keeps the version-1 layout so
        // older builds (and runs without --ntfs-map-origin) read it as-is.
        write_pod(out, image.valid ? kVersionImage : usn.valid ? kVersionUsn : kVersion);
        write_pod(out, chunk_size);
        if (image.valid) {
            write_pod(out, image.kind);
            write_pod(out, static_cast<std::uint8_t>(image.db_only ? 1 : 0));
            write_pod(out, image.source_id);
            write_pod(out, image.source_size);
            write_pod(out, image.dest_size);
            write_pod(out, image.dest_write_time);
            write_pod(out, static_cast<std::uint64_t>(image.chunks.size()));
            if (!image.chunks.empty())
                out.write(reinterpret_cast<const char*>(image.chunks.data()),
                          static_cast<std::streamsize>(image.chunks.size() * sizeof(Sha256)));
        } else {
            if (usn.valid) {
                write_pod(out, static_cast<std::uint8_t>(usn.db_only ? 1 : 0));
                write_pod(out, usn.volume_serial);
                write_pod(out, usn.journal_id);
                write_pod(out, usn.next_usn);
                write_pod(out, usn.excludes_hash);
            }
            write_pod(out, static_cast<std::uint64_t>(files.size()));
            for (const auto& [path, rec] : files) {
                const std::string path_utf8 = wide_to_utf8(path);
                write_pod(out, static_cast<std::uint32_t>(path_utf8.size()));
                out.write(path_utf8.data(), static_cast<std::streamsize>(path_utf8.size()));
                write_pod(out, rec.file_size);
                write_pod(out, rec.source_write_time);
                write_pod(out, static_cast<std::uint64_t>(rec.chunks.size()));
                if (!rec.chunks.empty())
                    out.write(reinterpret_cast<const char*>(rec.chunks.data()),
                              static_cast<std::streamsize>(rec.chunks.size() * sizeof(Sha256)));
            }
        }
        out.flush();
        if (!out) {
            error = std::format(L"failed writing database file {}", tmp.native());
            return false;
        }
    }

    if (!::MoveFileExW(extended_path(tmp).c_str(), extended_path(db_file).c_str(),
                       MOVEFILE_REPLACE_EXISTING)) {
        error = std::format(L"cannot replace database file {}: {}",
                            db_file.native(), win32_error_message(::GetLastError()));
        return false;
    }
    return true;
}

} // namespace tc
