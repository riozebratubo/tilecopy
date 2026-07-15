#pragma once

#include "hash.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace tc {

inline constexpr std::uint64_t kDefaultChunkSize = 1ull << 20; // 1 MiB

struct FileRecord {
    std::uint64_t file_size = 0;
    std::int64_t source_write_time = 0; // FILETIME of the source at last run
    std::vector<Sha256> chunks;
};

// Position in the source volume's USN change journal plus the identity of
// the volume and journal it belongs to (--ntfs-map-origin). Saved with the
// database so the next run can visit only what changed since.
struct UsnState {
    bool valid = false;   // present in the database / worth persisting
    bool db_only = false; // the position was taken by a --make-db run
    std::uint32_t volume_serial = 0;
    std::uint64_t journal_id = 0;
    std::int64_t next_usn = 0;
    std::uint64_t excludes_hash = 0; // fingerprint of the exclude lists at that run
};

// Sentinel for a raw-image chunk that is all zeros in the source and was
// never written to the VHDX (left as a hole). SHA-256 can never produce 32
// zero bytes, so the value cannot collide with a real chunk hash.
inline constexpr Sha256 kUnwrittenChunk{};

// Raw device image (--drive/--partition with a .vhdx destination): one hash
// per chunk of the source address space (whole-disk or volume bytes). The
// destination file's size and write time are recorded after each successful
// run; a mismatch on the next run means the VHDX was touched by something
// else and every chunk is rewritten.
struct ImageRecord {
    bool valid = false;
    std::uint8_t kind = 0; // 1 = whole disk, 2 = single partition
    bool db_only = false;  // hashes taken by --make-db; no destination written
    std::array<std::uint8_t, 16> source_id{}; // disk GUID/MBR signature or volume GUID
    std::uint64_t source_size = 0;    // bytes hashed (disk or volume size)
    std::uint64_t dest_size = 0;      // VHDX file size after the last run (0 = none)
    std::int64_t dest_write_time = 0; // VHDX FILETIME after the last run
    std::vector<Sha256> chunks;       // kUnwrittenChunk marks holes
};

// One database per copy job. Keys are paths relative to the source root
// (backslash-separated); a single-file job uses the empty key. An image
// database carries only the image record, never file records.
struct ChunkDatabase {
    std::uint64_t chunk_size = kDefaultChunkSize;
    UsnState usn; // persisted only when valid (--ntfs-map-origin)
    std::map<std::wstring, FileRecord> files;
    ImageRecord image; // persisted only when valid (--drive/--partition images)

    // Returns false if the file is missing, unreadable, corrupt, was written
    // with an incompatible version or a different chunk size, or is not of
    // the expected flavor (file records vs image record).
    static bool load(const std::filesystem::path& db_file, std::uint64_t expected_chunk_size,
                     bool expect_image, ChunkDatabase& out);

    // Writes atomically (temp file + rename).
    bool save(const std::filesystem::path& db_file, std::wstring& error) const;
};

} // namespace tc
