#pragma once

#include "chunkdb.h"

#include <filesystem>
#include <string>
#include <vector>

namespace tc {

struct UsnEntry {
    std::wstring rel;     // path relative to the scanned root, on-disk case
    bool is_dir = false;
    bool is_link = false; // reparse point (file or directory)
};

struct UsnChanges {
    // Entries that exist right now inside the root and were reported changed
    // (created, written, renamed to, metadata/security/reparse change).
    std::vector<UsnEntry> changed;
    // Lowercased final name components of delete/rename-old records. The
    // journal does not give their full path once they are gone, so callers
    // match them against known paths and let the file system confirm.
    std::vector<std::wstring> removed_names;
};

// Wraps the USN change journal of the volume holding a given root
// (--ntfs-map-origin). Reading the journal requires administrator rights.
class UsnJournal {
public:
    UsnJournal() = default;
    ~UsnJournal();
    UsnJournal(const UsnJournal&) = delete;
    UsnJournal& operator=(const UsnJournal&) = delete;

    // Opens the volume containing `root` and queries its journal. state()
    // afterwards holds the checkpoint to persist for the next run. Returns
    // false with `why` set when the journal cannot be used (no NTFS/ReFS
    // journal, no administrator rights, ...).
    bool open(const std::filesystem::path& root, std::wstring& why);

    const UsnState& state() const { return state_; }

    // Reads every journal record in [since.next_usn, state().next_usn) and
    // resolves the survivors against `root`. Returns false with `why` set
    // when `since` does not line up with this journal (different volume or
    // journal, wrapped past the position); the caller must fall back to a
    // full scan.
    bool read_changes(const UsnState& since, const std::filesystem::path& root,
                      UsnChanges& out, std::wstring& why);

private:
    void* hvol_ = nullptr;  // volume handle for the journal FSCTLs
    void* hroot_ = nullptr; // hint handle on the volume for OpenFileById
    UsnState state_;
    std::int64_t first_usn_ = 0;
};

} // namespace tc
