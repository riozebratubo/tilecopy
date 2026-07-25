#pragma once

#include "chunkdb.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace tc {

// DriveImage/PartitionImage are the raw-sector modes: --drive with a disk
// number/\\.\PhysicalDriveN source or a .vhdx destination, and --partition
// (always an image). The source is kept as parsed; image.cpp resolves it.
enum class Mode { File, Folder, Drive, DriveImage, PartitionImage };

struct Options {
    Mode mode = Mode::File;
    std::filesystem::path source;
    std::filesystem::path destination; // empty when --make-db is used without one
    std::optional<std::filesystem::path> db_path;
    bool make_db_only = false;
    std::uint64_t chunk_size = kDefaultChunkSize;
    // Hash every source file instead of trusting size + last-write time to
    // decide it is unchanged (raw image copies always read the source).
    bool always_read_source = false;

    // folder / drive only
    bool mirror = false;
    bool move_detection = true;
    bool move_detection_check_date = false;
    std::vector<std::filesystem::path> exclude_files;
    std::vector<std::filesystem::path> exclude_folders;
    bool multithread = false;
    int max_threads = 8; // 1..32
    bool ntfs_map_origin = false; // scan via the source volume's USN journal

    // Retries are cheap next to what one give-up costs: a raw image gives up
    // on a whole chunk, and a chunk that could not be copied is redone on the
    // next run.
    int max_tries = 3;

    bool file_logs = true;    // --no-file-logs (also cleared by --folder-logs)
    bool folder_logs = false; // folder / drive only
};

// Returns std::nullopt after printing an error (or usage for --help).
std::optional<Options> parse_command_line(int argc, wchar_t** argv);
void print_usage();

} // namespace tc
