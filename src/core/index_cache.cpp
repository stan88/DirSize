#include "core/index_cache.h"

#include <cstring>
#include <type_traits>

#include "core/crc32.h"

namespace dirsize {
namespace {

struct CacheHeader {
    std::uint32_t magic;
    std::uint32_t formatVersion;
    std::uint32_t rulesVersion;
    std::uint32_t headerBytes;
    std::uint8_t volumeGuid[16];
    std::uint64_t volumeSerial;
    std::uint64_t journalId;
    std::int64_t appliedUsn;
    std::uint64_t dirCount;
    std::uint64_t fileCount;
    std::uint64_t linkCount;
    std::uint64_t openCount;
    std::uint64_t nameUnits;
    std::uint32_t root;
    std::uint32_t reserved;
    ExcludedTotals excluded;  // informational, from the last full scan
    ScanIssues issues;        // informational, from the last full scan
    std::uint64_t rootListingMatched;
    std::uint32_t payloadCrc;
    std::uint32_t headerCrc;  // of every header byte before this field
};
static_assert(std::is_trivially_copyable_v<CacheHeader>);
static_assert(sizeof(ExcludedTotals) == 15 * 8 && sizeof(ScanIssues) == 5 * 8, "no padding in stored structs");
static_assert(sizeof(CacheHeader) == 280);

struct DirRecord {
    std::uint64_t id;
    std::uint32_t parent;  // record index; kNoDir for the root
    std::uint32_t nameOffset;
    std::uint16_t nameLength;
    std::uint16_t reserved0;
    std::uint32_t reserved1;
};
static_assert(sizeof(DirRecord) == 24);

struct FileRecord {
    std::uint64_t id;
    std::uint64_t logicalBytes;
    std::uint64_t allocatedBytes;
    std::uint32_t dir;
    std::uint16_t countedLinks;
    std::uint16_t reserved;
};
static_assert(sizeof(FileRecord) == 32);

struct LinkRecord {
    std::uint32_t file;
    std::uint32_t dir;
};
static_assert(sizeof(LinkRecord) == 8);

template <typename T>
void Put(std::vector<std::byte>& out, const T& value) {
    const size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

template <typename T>
T Get(const std::byte* data, size_t at) {
    T value;
    std::memcpy(&value, data + at, sizeof(T));
    return value;
}

bool AddChecked(std::uint64_t& sum, std::uint64_t value) {
    if (sum > UINT64_MAX - value) return false;
    sum += value;
    return true;
}

CacheLoadResult Fail(CacheLoadStatus status, const wchar_t* reason) { return {status, reason}; }

}  // namespace

std::vector<std::byte> SerializeIndex(const VolumeIndex& index, const CacheMeta& meta) {
    CacheHeader h;
    std::memset(&h, 0, sizeof(h));
    h.magic = kCacheMagic;
    h.formatVersion = kCacheFormatVersion;
    h.rulesVersion = kCountingRulesVersion;
    h.headerBytes = sizeof(CacheHeader);
    std::memcpy(h.volumeGuid, meta.volumeGuid, sizeof(h.volumeGuid));
    h.volumeSerial = meta.volumeSerial;
    h.journalId = meta.journalId;
    h.appliedUsn = meta.appliedUsn;
    h.dirCount = index.dirs.size();
    h.fileCount = index.files.size();
    h.linkCount = index.extraLinks.size();
    h.openCount = meta.openFileIds.size();
    h.nameUnits = index.names.size();
    h.root = index.root;
    h.excluded = index.excluded;
    h.issues = index.issues;
    h.rootListingMatched = index.rootListingMatched;

    std::vector<std::byte> out;
    out.reserve(sizeof(h) + index.dirs.size() * sizeof(DirRecord) + index.files.size() * sizeof(FileRecord) +
                index.extraLinks.size() * sizeof(LinkRecord) + meta.openFileIds.size() * 8 + index.names.size() * 2);
    out.resize(sizeof(h));  // header written last, once the payload CRC is known
    for (const DirEntry& d : index.dirs) {
        DirRecord r{};
        r.id = d.id;
        r.parent = d.parent;
        r.nameOffset = d.nameOffset;
        r.nameLength = d.nameLength;
        Put(out, r);
    }
    for (const FileEntry& f : index.files) {
        FileRecord r{};
        r.id = f.id;
        r.logicalBytes = f.logicalBytes;
        r.allocatedBytes = f.allocatedBytes;
        r.dir = f.dir;
        r.countedLinks = f.countedLinks;
        Put(out, r);
    }
    for (const ExtraLink& x : index.extraLinks) Put(out, LinkRecord{x.file, x.dir});
    for (std::uint64_t id : meta.openFileIds) Put(out, id);
    static_assert(sizeof(wchar_t) == 2);
    for (wchar_t c : index.names) Put(out, static_cast<std::uint16_t>(c));

    h.payloadCrc = ComputeCrc32(out.data() + sizeof(h), out.size() - sizeof(h));
    h.headerCrc = ComputeCrc32(&h, offsetof(CacheHeader, headerCrc));
    std::memcpy(out.data(), &h, sizeof(h));
    return out;
}

CacheLoadResult DeserializeIndex(const std::byte* data, size_t size, VolumeIndex& index, CacheMeta& meta) {
    index = VolumeIndex{};
    meta = CacheMeta{};
    if (data == nullptr || size < sizeof(CacheHeader)) return Fail(CacheLoadStatus::Truncated, L"shorter than the header");
    const auto h = Get<CacheHeader>(data, 0);
    if (h.magic != kCacheMagic) return Fail(CacheLoadStatus::BadMagic, L"not a DirSize cache file");
    if (h.formatVersion != kCacheFormatVersion || h.headerBytes != sizeof(CacheHeader)) {
        return Fail(CacheLoadStatus::VersionMismatch, L"other cache format version");
    }
    if (h.rulesVersion != kCountingRulesVersion) {
        return Fail(CacheLoadStatus::VersionMismatch, L"made under other counting rules");
    }
    if (ComputeCrc32(data, offsetof(CacheHeader, headerCrc)) != h.headerCrc) {
        return Fail(CacheLoadStatus::HeaderCorrupt, L"header checksum mismatch");
    }

    // Counts must fit the bytes present before anything is allocated; each
    // count is bounded first, so the products below cannot overflow.
    const std::uint64_t room = size - sizeof(CacheHeader);
    if (h.dirCount > room / sizeof(DirRecord) || h.fileCount > room / sizeof(FileRecord) ||
        h.linkCount > room / sizeof(LinkRecord) || h.openCount > room / 8 || h.nameUnits > room / 2) {
        return Fail(CacheLoadStatus::SizeMismatch, L"counts exceed the file size");
    }
    std::uint64_t expected = sizeof(CacheHeader);
    if (!AddChecked(expected, h.dirCount * sizeof(DirRecord)) || !AddChecked(expected, h.fileCount * sizeof(FileRecord)) ||
        !AddChecked(expected, h.linkCount * sizeof(LinkRecord)) || !AddChecked(expected, h.openCount * 8) ||
        !AddChecked(expected, h.nameUnits * 2) || expected != size) {
        return Fail(CacheLoadStatus::SizeMismatch, L"counts do not match the file size");
    }
    if (ComputeCrc32(data + sizeof(CacheHeader), size - sizeof(CacheHeader)) != h.payloadCrc) {
        return Fail(CacheLoadStatus::PayloadCorrupt, L"payload checksum mismatch");
    }

    // ---- structure (the CRC only proves the bytes are as written) -------------
    if (h.dirCount == 0 || h.dirCount >= kNoDir || h.fileCount >= 0xFFFFFFFFull || h.root >= h.dirCount ||
        h.nameUnits > 0xFFFFFFFFull || h.reserved != 0) {
        return Fail(CacheLoadStatus::Invalid, L"counts or root out of range");
    }
    size_t at = sizeof(CacheHeader);
    const size_t dirsAt = at;
    at += h.dirCount * sizeof(DirRecord);
    const size_t filesAt = at;
    at += h.fileCount * sizeof(FileRecord);
    const size_t linksAt = at;
    at += h.linkCount * sizeof(LinkRecord);
    const size_t openAt = at;
    at += h.openCount * 8;
    const size_t namesAt = at;

    VolumeIndex x;
    x.dirs.resize(h.dirCount);
    std::uint64_t previous = 0;
    for (std::uint64_t k = 0; k < h.dirCount; ++k) {
        const auto r = Get<DirRecord>(data, dirsAt + k * sizeof(DirRecord));
        if (r.reserved0 != 0 || r.reserved1 != 0) return Fail(CacheLoadStatus::Invalid, L"folder reserved field set");
        if (r.id <= previous) return Fail(CacheLoadStatus::Invalid, L"folder IDs not strictly increasing");
        previous = r.id;
        if (k == h.root ? r.parent != kNoDir : (r.parent >= h.dirCount || r.parent == k)) {
            return Fail(CacheLoadStatus::Invalid, L"folder parent out of range");
        }
        if (static_cast<std::uint64_t>(r.nameOffset) + r.nameLength > h.nameUnits) {
            return Fail(CacheLoadStatus::Invalid, L"folder name outside the name pool");
        }
        DirEntry& d = x.dirs[k];
        d.id = r.id;
        d.parent = r.parent;
        d.nameOffset = r.nameOffset;
        d.nameLength = r.nameLength;
    }
    x.files.resize(h.fileCount);
    previous = 0;
    for (std::uint64_t k = 0; k < h.fileCount; ++k) {
        const auto r = Get<FileRecord>(data, filesAt + k * sizeof(FileRecord));
        if (r.reserved != 0) return Fail(CacheLoadStatus::Invalid, L"file reserved field set");
        if (r.id <= previous) return Fail(CacheLoadStatus::Invalid, L"file IDs not strictly increasing");
        previous = r.id;
        if (r.dir >= h.dirCount || r.countedLinks == 0) {
            return Fail(CacheLoadStatus::Invalid, L"file folder or link count out of range");
        }
        FileEntry& f = x.files[k];
        f.id = r.id;
        f.logicalBytes = r.logicalBytes;
        f.allocatedBytes = r.allocatedBytes;
        f.dir = r.dir;
        f.countedLinks = r.countedLinks;
    }
    // A folder and a file never share an ID (both lists are sorted).
    for (size_t i = 0, j = 0; i < x.dirs.size() && j < x.files.size();) {
        if (x.dirs[i].id == x.files[j].id) return Fail(CacheLoadStatus::Invalid, L"folder and file share an ID");
        x.dirs[i].id < x.files[j].id ? ++i : ++j;
    }
    std::vector<std::uint32_t> extraCount(x.files.size(), 0);
    x.extraLinks.resize(h.linkCount);
    for (std::uint64_t k = 0; k < h.linkCount; ++k) {
        const auto r = Get<LinkRecord>(data, linksAt + k * sizeof(LinkRecord));
        if (r.file >= h.fileCount || r.dir >= h.dirCount) return Fail(CacheLoadStatus::Invalid, L"extra link out of range");
        ++extraCount[r.file];
        x.extraLinks[k] = {r.file, r.dir};
    }
    for (size_t k = 0; k < x.files.size(); ++k) {
        if (extraCount[k] + 1 != x.files[k].countedLinks) {
            return Fail(CacheLoadStatus::Invalid, L"extra links do not match the link count");
        }
    }
    std::vector<std::uint64_t> openIds(h.openCount);
    for (std::uint64_t k = 0; k < h.openCount; ++k) openIds[k] = Get<std::uint64_t>(data, openAt + k * 8);
    x.names.resize(h.nameUnits);
    for (std::uint64_t k = 0; k < h.nameUnits; ++k) {
        x.names[k] = static_cast<wchar_t>(Get<std::uint16_t>(data, namesAt + k * 2));
    }

    // Every folder must reach the root (no cycles, no orphans).
    x.root = h.root;
    const std::vector<std::uint32_t> depth = ComputeDepths(x.dirs, x.root);
    for (std::uint32_t d : depth) {
        if (d == kUnreachableDepth) return Fail(CacheLoadStatus::Invalid, L"folder not reachable from the root");
    }
    // Direct totals, then RollUp. The grand total bounds every partial sum,
    // so checking it for overflow is enough.
    std::uint64_t allLogical = 0, allAllocated = 0;
    auto count = [&](const FileEntry& f, DirIndex dir) {
        Totals& t = x.dirs[dir].subtree;
        if (!AddChecked(allLogical, f.logicalBytes) || !AddChecked(allAllocated, f.allocatedBytes)) return false;
        t.logicalBytes += f.logicalBytes;
        t.allocatedBytes += f.allocatedBytes;
        ++t.files;
        return true;
    };
    for (const FileEntry& f : x.files) {
        if (!count(f, f.dir)) return Fail(CacheLoadStatus::Invalid, L"size totals overflow");
    }
    for (const ExtraLink& l : x.extraLinks) {
        if (!count(x.files[l.file], l.dir)) return Fail(CacheLoadStatus::Invalid, L"size totals overflow");
    }
    RollUp(x);

    x.sortedById = true;  // checked above
    x.excluded = h.excluded;
    x.issues = h.issues;
    x.rootListingMatched = h.rootListingMatched;
    x.usn.active = true;
    x.usn.journalId = h.journalId;
    x.usn.nextUsn = h.appliedUsn;
    std::memcpy(meta.volumeGuid, h.volumeGuid, sizeof(meta.volumeGuid));
    meta.volumeSerial = h.volumeSerial;
    meta.journalId = h.journalId;
    meta.appliedUsn = h.appliedUsn;
    meta.openFileIds = std::move(openIds);
    index = std::move(x);
    return {CacheLoadStatus::Ok, L""};
}

static_assert(CacheSections::kHeaderBytes == sizeof(CacheHeader));
static_assert(CacheSections::kDirBytes == sizeof(DirRecord) && CacheSections::kFileBytes == sizeof(FileRecord) &&
              CacheSections::kLinkBytes == sizeof(LinkRecord));
static_assert(CacheSections::kFormatVersionAt == offsetof(CacheHeader, formatVersion) &&
              CacheSections::kDirCountAt == offsetof(CacheHeader, dirCount) &&
              CacheSections::kHeaderReservedAt == offsetof(CacheHeader, reserved) &&
              CacheSections::kRulesVersionAt == offsetof(CacheHeader, rulesVersion) &&
              CacheSections::kRootAt == offsetof(CacheHeader, root));
static_assert(CacheSections::kDirParentAt == offsetof(DirRecord, parent) &&
              CacheSections::kDirNameOffsetAt == offsetof(DirRecord, nameOffset) &&
              CacheSections::kDirNameLengthAt == offsetof(DirRecord, nameLength) &&
              CacheSections::kDirReservedAt == offsetof(DirRecord, reserved0));
static_assert(CacheSections::kFileLogicalAt == offsetof(FileRecord, logicalBytes) &&
              CacheSections::kFileDirAt == offsetof(FileRecord, dir) &&
              CacheSections::kFileLinksAt == offsetof(FileRecord, countedLinks) &&
              CacheSections::kFileReservedAt == offsetof(FileRecord, reserved));

bool LocateCacheSections(const std::vector<std::byte>& bytes, CacheSections& s) {
    if (bytes.size() < sizeof(CacheHeader)) return false;
    const auto h = Get<CacheHeader>(bytes.data(), 0);
    s.dirCount = h.dirCount;
    s.fileCount = h.fileCount;
    s.linkCount = h.linkCount;
    s.openCount = h.openCount;
    s.nameUnits = h.nameUnits;
    s.header = 0;
    s.dirs = sizeof(CacheHeader);
    s.files = s.dirs + h.dirCount * sizeof(DirRecord);
    s.links = s.files + h.fileCount * sizeof(FileRecord);
    s.openIds = s.links + h.linkCount * sizeof(LinkRecord);
    s.names = s.openIds + h.openCount * 8;
    return s.names + h.nameUnits * 2 == bytes.size();
}

void RecomputeCacheChecksums(std::vector<std::byte>& bytes) {
    if (bytes.size() < sizeof(CacheHeader)) return;
    auto h = Get<CacheHeader>(bytes.data(), 0);
    h.payloadCrc = ComputeCrc32(bytes.data() + sizeof(h), bytes.size() - sizeof(h));
    h.headerCrc = ComputeCrc32(&h, offsetof(CacheHeader, headerCrc));
    std::memcpy(bytes.data(), &h, sizeof(h));
}

}  // namespace dirsize
