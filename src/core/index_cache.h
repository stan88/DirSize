#pragma once

// The on-disk form of a folder index (Phase 3 cache, docs/DESIGN-phase3.md
// section 6). Pure byte-buffer functions; file I/O is in cache_store.h.
//
// Only primary data is stored: folder IDs, parents and names; file IDs,
// sizes, folders and link counts; extra links. Child lists and subtree totals
// are rebuilt on load (RollUp), so a damaged or crafted file cannot make them
// disagree with the data. A loaded file is untrusted input: every count,
// index and range is checked before use, and anything unexpected rejects the
// whole file (the caller then does a full scan).
//
// Layout (little-endian, x64): CacheHeader, then dir records, file records,
// extra-link records, open-file IDs, then the name pool (UTF-16 units).
// The header ends with a CRC-32 of the payload and a CRC-32 of the header.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/index.h"

namespace dirsize {

constexpr std::uint32_t kCacheMagic = 0x435A5344u;  // "DSZC"
constexpr std::uint32_t kCacheFormatVersion = 1;
// Bump whenever a counting rule changes (what is counted, how links count):
// a cache made under other rules is not reused.
constexpr std::uint32_t kCountingRulesVersion = 1;

struct CacheMeta {
    std::uint8_t volumeGuid[16] = {};
    std::uint64_t volumeSerial = 0;  // NTFS_VOLUME_DATA_BUFFER::VolumeSerialNumber
    std::uint64_t journalId = 0;
    std::int64_t appliedUsn = 0;     // the index reflects every journal record before this USN
    std::vector<std::uint64_t> openFileIds;  // files seen open without CLOSE (re-queried on resume)
};

enum class CacheLoadStatus {
    Ok,
    Truncated,          // shorter than its header or than the counts require
    BadMagic,
    VersionMismatch,    // format or counting-rules version differs
    HeaderCorrupt,      // header CRC mismatch
    PayloadCorrupt,     // payload CRC mismatch
    SizeMismatch,       // counts do not add up to the file size
    Invalid,            // CRC fine but contents inconsistent
};

struct CacheLoadResult {
    CacheLoadStatus status = CacheLoadStatus::Invalid;
    const wchar_t* reason = L"";
};

// `index` must be compact (no dead entries) and sorted by ID, e.g. from
// LiveIndex::Compacted().
std::vector<std::byte> SerializeIndex(const VolumeIndex& index, const CacheMeta& meta);

// On success fills `index` (sortedById, totals rebuilt, usn from the meta)
// and `meta`. On failure both are left empty.
CacheLoadResult DeserializeIndex(const std::byte* data, size_t size, VolumeIndex& index, CacheMeta& meta);

// For tests: where the sections of a serialized cache start, and a way to
// recompute both checksums after changing bytes, so that the structural
// checks can be tested on their own.
struct CacheSections {
    size_t header = 0, dirs = 0, files = 0, links = 0, openIds = 0, names = 0;
    std::uint64_t dirCount = 0, fileCount = 0, linkCount = 0, openCount = 0, nameUnits = 0;
    static constexpr size_t kHeaderBytes = 280, kDirBytes = 24, kFileBytes = 32, kLinkBytes = 8;
    // Field offsets inside the header and the records.
    static constexpr size_t kFormatVersionAt = 4, kRulesVersionAt = 8, kDirCountAt = 56, kRootAt = 96, kHeaderReservedAt = 100;
    static constexpr size_t kDirParentAt = 8, kDirNameOffsetAt = 12, kDirNameLengthAt = 16, kDirReservedAt = 18;
    static constexpr size_t kFileLogicalAt = 8, kFileDirAt = 24, kFileLinksAt = 28, kFileReservedAt = 30;
};
bool LocateCacheSections(const std::vector<std::byte>& bytes, CacheSections& sections);
void RecomputeCacheChecksums(std::vector<std::byte>& bytes);

// Upper bound accepted for a cache file.
constexpr std::uint64_t kMaxCacheBytes = 4ull << 30;

}  // namespace dirsize
