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

// One database per copy job. Keys are paths relative to the source root
// (backslash-separated); a single-file job uses the empty key.
struct ChunkDatabase {
    std::uint64_t chunk_size = kDefaultChunkSize;
    UsnState usn; // persisted only when valid (--ntfs-map-origin)
    std::map<std::wstring, FileRecord> files;

    // Returns false if the file is missing, unreadable, corrupt, or was
    // written with an incompatible version or a different chunk size.
    static bool load(const std::filesystem::path& db_file, std::uint64_t expected_chunk_size,
                     ChunkDatabase& out);

    // Writes atomically (temp file + rename).
    bool save(const std::filesystem::path& db_file, std::wstring& error) const;
};

} // namespace tc
