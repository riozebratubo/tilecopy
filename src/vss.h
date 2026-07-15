#pragma once

#include <string>
#include <vector>

namespace tc {

// One VSS snapshot set covering any number of volumes, taken at a single
// consistent point in time. Writer participation (application-consistent) is
// attempted first; if the writer dance fails the set is retried without
// writers (file-share backup context, still filesystem-consistent). The
// snapshots are non-persistent: released when the object is destroyed.
class VssSnapshotSet {
public:
    VssSnapshotSet() = default;
    ~VssSnapshotSet();
    VssSnapshotSet(const VssSnapshotSet&) = delete;
    VssSnapshotSet& operator=(const VssSnapshotSet&) = delete;

    // volumes are named "X:\" or "\\?\Volume{GUID}\". devices[i] receives the
    // shadow device path for volumes[i], e.g.
    // "\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy3" (usable with
    // CreateFileW directly or with a "\path" suffix), or stays empty when
    // that volume could not be included.
    // Returns false (with error set) when no snapshot at all was taken.
    bool create(const std::vector<std::wstring>& volumes,
                std::vector<std::wstring>& devices, std::wstring& error);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace tc
