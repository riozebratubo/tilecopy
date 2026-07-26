#pragma once

#include <windows.h>

#include <cstdint>

namespace tc {

// The GPT a --partition image carries: image.cpp synthesizes it around the
// volume bytes so the image mounts with a drive letter, and restore.cpp reads
// it back to find where the volume bytes are and to tell a partition image
// apart from a whole-disk image.

// Where the volume data starts inside a --partition image: one aligned MiB
// leaves room for the protective MBR, the GPT header and its entry array.
inline constexpr std::uint64_t kPartitionDataOffset = 1ull << 20;
inline constexpr std::uint64_t kGptEntryBytes = 128ull * 128; // 128 entries of 128 bytes

inline constexpr GUID kBasicDataPartition = {
    0xEBD0A0A2, 0xB9E5, 0x4433, {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}};

// Name given to the single entry of a --partition image. Together with the
// fixed data offset this is what identifies an image as a wrapped volume
// rather than a copy of a whole disk that merely also carries a GPT.
inline constexpr wchar_t kImagePartitionName[] = L"tilecopy";

#pragma pack(push, 1)
struct GptHeader {
    char signature[8]; // "EFI PART"
    std::uint32_t revision;
    std::uint32_t header_size;
    std::uint32_t header_crc;
    std::uint32_t reserved;
    std::uint64_t my_lba;
    std::uint64_t alternate_lba;
    std::uint64_t first_usable;
    std::uint64_t last_usable;
    std::uint8_t disk_guid[16];
    std::uint64_t entries_lba;
    std::uint32_t entry_count;
    std::uint32_t entry_size;
    std::uint32_t entries_crc;
};
static_assert(sizeof(GptHeader) == 92);

struct GptEntry {
    std::uint8_t type[16];
    std::uint8_t id[16];
    std::uint64_t first_lba;
    std::uint64_t last_lba;
    std::uint64_t attrs;
    wchar_t name[36];
};
static_assert(sizeof(GptEntry) == 128);
#pragma pack(pop)

} // namespace tc
