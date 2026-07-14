#include "usnmap.h"

#include "util.h"

#include <windows.h>

#include <winioctl.h>

#include <array>
#include <cstring>
#include <cwctype>
#include <format>
#include <set>

namespace tc {

namespace fs = std::filesystem;

namespace {

std::wstring to_lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// Reasons that mean an entry stopped existing under its recorded name.
constexpr DWORD kGoneMask = USN_REASON_FILE_DELETE | USN_REASON_RENAME_OLD_NAME;

} // namespace

UsnJournal::~UsnJournal() {
    if (hvol_) ::CloseHandle(hvol_);
    if (hroot_) ::CloseHandle(hroot_);
}

bool UsnJournal::open(const fs::path& root, std::wstring& why) {
    std::error_code ec;
    const std::wstring abs = fs::absolute(root, ec).native();

    wchar_t mount[MAX_PATH];
    if (!::GetVolumePathNameW(abs.c_str(), mount, MAX_PATH)) {
        why = std::format(L"cannot resolve the volume of {}: {}", abs,
                          win32_error_message(::GetLastError()));
        return false;
    }
    wchar_t guid[MAX_PATH]; // "\\?\Volume{...}\"
    if (!::GetVolumeNameForVolumeMountPointW(mount, guid, MAX_PATH)) {
        why = std::format(L"cannot resolve the volume name of {}: {}", mount,
                          win32_error_message(::GetLastError()));
        return false;
    }
    DWORD serial = 0;
    if (!::GetVolumeInformationW(guid, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) {
        why = std::format(L"cannot query volume {}: {}", mount,
                          win32_error_message(::GetLastError()));
        return false;
    }

    std::wstring volume = guid;
    if (!volume.empty() && volume.back() == L'\\') volume.pop_back();
    HANDLE hv = ::CreateFileW(volume.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                              nullptr);
    if (hv == INVALID_HANDLE_VALUE) {
        why = std::format(L"cannot open volume {} (administrator rights are needed): {}", mount,
                          win32_error_message(::GetLastError()));
        return false;
    }
    hvol_ = hv;

    HANDLE hr = ::CreateFileW(mount, FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (hr == INVALID_HANDLE_VALUE) {
        why = std::format(L"cannot open volume root {}: {}", mount,
                          win32_error_message(::GetLastError()));
        return false;
    }
    hroot_ = hr;

    USN_JOURNAL_DATA_V1 jd{};
    DWORD bytes = 0;
    if (!::DeviceIoControl(hvol_, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &jd, sizeof jd, &bytes,
                           nullptr)) {
        why = std::format(L"volume {} has no usable USN change journal: {}", mount,
                          win32_error_message(::GetLastError()));
        return false;
    }

    state_.valid = true;
    state_.volume_serial = serial;
    state_.journal_id = jd.UsnJournalID;
    state_.next_usn = jd.NextUsn;
    first_usn_ = jd.FirstUsn;
    return true;
}

bool UsnJournal::read_changes(const UsnState& since, const fs::path& root, UsnChanges& out,
                              std::wstring& why) {
    if (!state_.valid) {
        why = L"the journal is not open";
        return false;
    }
    if (!since.valid) {
        why = L"no journal position in the database yet";
        return false;
    }
    if (since.volume_serial != state_.volume_serial || since.journal_id != state_.journal_id) {
        why = L"the database's journal position belongs to a different volume or journal";
        return false;
    }
    if (since.next_usn < first_usn_ || since.next_usn > state_.next_usn) {
        why = L"the journal wrapped past the database's position";
        return false;
    }

    const std::wstring root_ext = extended_path(root);
    const std::wstring root_norm = to_lower(root_ext);

    std::set<std::wstring> removed;
    std::set<std::array<std::uint64_t, 2>> changed_ids; // V2 fills the low half only

    READ_USN_JOURNAL_DATA_V1 rd{};
    rd.ReasonMask = 0xFFFFFFFF;
    rd.UsnJournalID = state_.journal_id;
    rd.MinMajorVersion = 2;
    rd.MaxMajorVersion = 3;

    std::vector<std::uint8_t> buf(1 << 16);
    std::int64_t cursor = since.next_usn;
    while (cursor < state_.next_usn) {
        rd.StartUsn = cursor;
        DWORD bytes = 0;
        if (!::DeviceIoControl(hvol_, FSCTL_READ_USN_JOURNAL, &rd, sizeof rd, buf.data(),
                               static_cast<DWORD>(buf.size()), &bytes, nullptr)) {
            why = std::format(L"cannot read the USN journal: {}",
                              win32_error_message(::GetLastError()));
            return false;
        }
        if (bytes < sizeof(USN)) break;
        std::int64_t next = 0;
        std::memcpy(&next, buf.data(), sizeof next);

        std::size_t off = sizeof(USN);
        while (off + sizeof(USN_RECORD_COMMON_HEADER) <= bytes) {
            const auto* hdr =
                reinterpret_cast<const USN_RECORD_COMMON_HEADER*>(buf.data() + off);
            if (hdr->RecordLength < sizeof(USN_RECORD_COMMON_HEADER) ||
                off + hdr->RecordLength > bytes)
                break;

            DWORD reason = 0;
            WORD name_off = 0, name_len = 0;
            std::array<std::uint64_t, 2> id{};
            bool known = true;
            if (hdr->MajorVersion == 2) {
                const auto* r = reinterpret_cast<const USN_RECORD_V2*>(hdr);
                reason = r->Reason;
                name_off = r->FileNameOffset;
                name_len = r->FileNameLength;
                id[0] = r->FileReferenceNumber;
            } else if (hdr->MajorVersion == 3) {
                const auto* r = reinterpret_cast<const USN_RECORD_V3*>(hdr);
                reason = r->Reason;
                name_off = r->FileNameOffset;
                name_len = r->FileNameLength;
                std::memcpy(id.data(), r->FileReferenceNumber.Identifier, 16);
            } else {
                known = false; // V4 range-tracking records are never requested
            }

            if (known && (reason & kGoneMask) && name_len &&
                static_cast<DWORD>(name_off) + name_len <= hdr->RecordLength) {
                std::wstring name(
                    reinterpret_cast<const wchar_t*>(buf.data() + off + name_off),
                    name_len / sizeof(wchar_t));
                removed.insert(to_lower(std::move(name)));
            }
            if (known && (reason & ~(kGoneMask | USN_REASON_CLOSE)))
                changed_ids.insert(id);

            off += hdr->RecordLength;
        }

        if (next <= cursor) break;
        cursor = next;
    }

    // Resolve the ids to current paths. Entries that vanished again since
    // simply fail to open; their delete records cover them.
    std::set<std::wstring> dedupe;
    for (const auto& id : changed_ids) {
        FILE_ID_DESCRIPTOR fid{};
        fid.dwSize = sizeof fid;
        fid.Type = ExtendedFileIdType;
        std::memcpy(fid.ExtendedFileId.Identifier, id.data(), 16);
        HANDLE h = ::OpenFileById(hroot_, &fid, FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
        if (h == INVALID_HANDLE_VALUE) continue;

        std::wstring full(512, L'\0');
        DWORD n = ::GetFinalPathNameByHandleW(h, full.data(), static_cast<DWORD>(full.size()),
                                              FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (n >= full.size()) {
            full.resize(n);
            n = ::GetFinalPathNameByHandleW(h, full.data(), static_cast<DWORD>(full.size()),
                                            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        }
        BY_HANDLE_FILE_INFORMATION info{};
        const bool have_info = ::GetFileInformationByHandle(h, &info) != 0;
        ::CloseHandle(h);
        if (n == 0 || n >= full.size() || !have_info) continue;
        full.resize(n);

        const std::wstring full_norm = to_lower(full);
        std::size_t rel_pos = 0;
        if (!full_norm.starts_with(root_norm)) continue;
        if (root_norm.back() == L'\\') {
            if (full_norm.size() <= root_norm.size()) continue;
            rel_pos = root_norm.size();
        } else if (full_norm.size() > root_norm.size() &&
                   full_norm[root_norm.size()] == L'\\') {
            rel_pos = root_norm.size() + 1;
        } else {
            continue; // the root itself or an unrelated sibling
        }

        if (!dedupe.insert(full_norm).second) continue;
        UsnEntry e;
        e.rel = full.substr(rel_pos);
        e.is_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.is_link = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        out.changed.push_back(std::move(e));
    }

    out.removed_names.assign(removed.begin(), removed.end());
    return true;
}

} // namespace tc
