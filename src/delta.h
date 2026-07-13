#pragma once

#include "chunkdb.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace tc {

struct DeltaResult {
    bool ok = false;
    bool skipped = false;    // source unchanged and destination intact; nothing done
    bool delta_used = false; // partial rewrite instead of full copy
    std::uint64_t bytes_written = 0;
    std::uint64_t bytes_skipped = 0; // bytes proven unchanged and not rewritten
    std::wstring error;
};

// Copies src to dst rewriting only the chunks whose hashes differ from
// `record` (falls back to a full copy when the database and destination do
// not line up). Always leaves `record` describing the current source content.
// With db_only the destination is ignored and only the record is refreshed.
// chunk_size must match the database the record came from.
DeltaResult delta_copy_file(const std::filesystem::path& src, const std::filesystem::path& dst,
                            FileRecord& record, bool had_record, bool db_only,
                            std::uint64_t chunk_size);

} // namespace tc
