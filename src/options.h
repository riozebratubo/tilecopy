#pragma once

#include "chunkdb.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace tc {

enum class Mode { File, Folder, Drive };

struct Options {
    Mode mode = Mode::File;
    std::filesystem::path source;
    std::filesystem::path destination; // empty when --make-db is used without one
    std::optional<std::filesystem::path> db_path;
    bool make_db_only = false;
    std::uint64_t chunk_size = kDefaultChunkSize;

    // folder / drive only
    bool mirror = false;
    bool move_detection = true;
    bool move_detection_check_date = false;
    std::vector<std::filesystem::path> exclude_files;
    std::vector<std::filesystem::path> exclude_folders;
    bool multithread = false;
    int max_threads = 8; // 1..32

    int max_tries = 1;

    bool file_logs = true;    // --no-file-logs (also cleared by --folder-logs)
    bool folder_logs = false; // folder / drive only
};

// Returns std::nullopt after printing an error (or usage for --help).
std::optional<Options> parse_command_line(int argc, wchar_t** argv);
void print_usage();

} // namespace tc
