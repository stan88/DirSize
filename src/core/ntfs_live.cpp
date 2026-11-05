#include "core/ntfs_live.h"

#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/dir_listing.h"
#include "core/layout_parser.h"
#include "core/usn_parser.h"

namespace dirsize {
namespace {

double SecondsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

CatchUpResult Rescan(CatchUpResult r, const wchar_t* reason, unsigned long error = 0) {
    r.needsRescan = true;
    r.reason = reason;
    r.error = error;
    return r;
}

// Re-queries the current state of the given record-number ranges.
DWORD QueryRanges(HANDLE volume, const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges, size_t begin,
                  size_t end, std::vector<std::byte>& out, std::vector<CurrentEntry>& entries,
                  std::uint64_t& calls, bool& malformed) {
    const size_t n = end - begin;
    const size_t inputBytes = offsetof(QUERY_FILE_LAYOUT_INPUT, Filter) + n * sizeof(FILE_REFERENCE_RANGE);
    std::vector<std::uint64_t> storage((inputBytes + 7) / 8);
    auto* input = reinterpret_cast<QUERY_FILE_LAYOUT_INPUT*>(storage.data());
    input->FilterEntryCount = static_cast<DWORD>(n);
    input->FilterType = QUERY_FILE_LAYOUT_FILTER_TYPE_FILEID;
    for (size_t i = 0; i < n; ++i) {
        input->Filter.FileReferenceRanges[i].StartingFileReferenceNumber = ranges[begin + i].first;
        input->Filter.FileReferenceRanges[i].EndingFileReferenceNumber = ranges[begin + i].second;
    }
    LayoutParseStats stats;
    for (bool first = true;; first = false) {
        input->Flags = QUERY_FILE_LAYOUT_INCLUDE_NAMES | QUERY_FILE_LAYOUT_INCLUDE_STREAMS |
                       QUERY_FILE_LAYOUT_INCLUDE_STREAMS_WITH_NO_CLUSTERS_ALLOCATED |
                       (first ? QUERY_FILE_LAYOUT_RESTART : 0);
        DWORD returned = 0;
        if (!DeviceIoControl(volume, FSCTL_QUERY_FILE_LAYOUT, input, static_cast<DWORD>(inputBytes), out.data(),
                             static_cast<DWORD>(out.size()), &returned, nullptr)) {
            const DWORD error = GetLastError();
            return error == ERROR_HANDLE_EOF ? ERROR_SUCCESS : error;
        }
        ++calls;
        const std::uint32_t count = ParseLayoutBuffer(out.data(), returned, stats, [&](const LayoutEntry& e) {
            if (e.malformed) malformed = true;
            entries.push_back(ToCurrentEntry(e));
        });
        if (stats.badHeaders != 0 || stats.truncatedChains != 0) malformed = true;
        if (count == 0) return ERROR_SUCCESS;
    }
}

}  // namespace

NtfsLiveVolume::NtfsLiveVolume(wchar_t drive, VolumeIndex index) : drive_(drive), live_(std::move(index)) {}

NtfsLiveVolume::~NtfsLiveVolume() {
    if (volume_ != INVALID_HANDLE_VALUE) CloseHandle(volume_);
    if (root_ != INVALID_HANDLE_VALUE) CloseHandle(root_);
}

std::unique_ptr<NtfsLiveVolume> NtfsLiveVolume::Build(wchar_t drive, NtfsIndexResult& build) {
    build = BuildNtfsIndex(drive);
    if (!build.ok) return nullptr;
    if (!build.index.usn.active) {
        build.ok = false;
        build.failedStep = L"USN journal not active";
        build.error = build.index.usn.error;
        return nullptr;
    }
    const std::uint64_t journalId = build.index.usn.journalId;
    const std::int64_t startUsn = build.index.usn.nextUsn;
    std::unique_ptr<NtfsLiveVolume> v(new NtfsLiveVolume(drive, std::move(build.index)));
    build.index = VolumeIndex{};
    if (const DWORD error = v->OpenHandles(); error != ERROR_SUCCESS) {
        build.ok = false;
        build.failedStep = L"reopen volume for updates";
        build.error = error;
        return nullptr;
    }
    v->journalId_ = journalId;
    v->appliedUsn_ = startUsn;
    return v;
}

DWORD NtfsLiveVolume::OpenHandles() {
    const std::wstring volumePath = std::wstring(L"\\\\.\\") + drive_ + L":";
    volume_ = CreateFileW(volumePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, 0, nullptr);
    if (volume_ == INVALID_HANDLE_VALUE) return GetLastError();
    const std::wstring rootPath = std::wstring(1, drive_) + L":\\";
    root_ = OpenDirectoryForListing(rootPath.c_str());
    if (root_ == INVALID_HANDLE_VALUE) return GetLastError();
    return ERROR_SUCCESS;
}

std::unique_ptr<NtfsLiveVolume> NtfsLiveVolume::FromCache(wchar_t drive, CacheOpenResult& result) {
    result = CacheOpenResult{};
    auto fail = [&](const StoreResult& s) {
        result.missing = s.missing;
        result.reason = s.reason;
        result.error = s.error;
        return nullptr;
    };
    VolumeIdentity identity;
    if (StoreResult s = GetVolumeIdentity(drive, identity); !s.ok) return fail(s);
    std::wstring folder;
    if (StoreResult s = PrepareCacheFolder(false, folder); !s.ok) return fail(s);
    result.path = CacheFilePath(folder, identity);
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::byte> bytes;
    if (StoreResult s = ReadWholeFile(result.path, kMaxCacheBytes, bytes); !s.ok) return fail(s);
    result.bytes = bytes.size();
    result.readSeconds = SecondsSince(t0);

    const auto t1 = std::chrono::steady_clock::now();
    VolumeIndex index;
    CacheMeta meta;
    const CacheLoadResult loaded = DeserializeIndex(bytes.data(), bytes.size(), index, meta);
    result.parseSeconds = SecondsSince(t1);
    result.status = loaded.status;
    if (loaded.status != CacheLoadStatus::Ok) {
        result.reason = loaded.reason;
        return nullptr;
    }
    if (std::memcmp(meta.volumeGuid, identity.guid, sizeof(identity.guid)) != 0) {
        result.reason = L"cache is for another volume";
        return nullptr;
    }
    if (meta.volumeSerial != identity.serial) {
        result.reason = L"volume serial number changed (formatted?)";
        return nullptr;
    }
    std::unique_ptr<NtfsLiveVolume> v(new NtfsLiveVolume(drive, std::move(index)));
    if (const DWORD error = v->OpenHandles(); error != ERROR_SUCCESS) {
        result.reason = L"cannot open the volume for updates";
        result.error = error;
        return nullptr;
    }
    std::uint64_t rootId = 0;
    const VolumeIndex& x = v->live_.Index();
    if (!QueryFileId(v->root_, rootId) || rootId != x.dirs[x.root].id) {
        result.reason = L"cache root folder ID does not match the volume";
        return nullptr;
    }
    v->journalId_ = meta.journalId;
    v->appliedUsn_ = meta.appliedUsn;
    for (std::uint64_t id : meta.openFileIds) {
        v->open_.Track(id);
        v->seen_.emplace(id, 0);  // seen in the journal before the save
    }
    result.ok = true;
    return v;
}

CacheSaveResult NtfsLiveVolume::SaveCache() const {
    CacheSaveResult r;
    auto fail = [&](const StoreResult& s) {
        r.reason = s.reason;
        r.error = s.error;
        return r;
    };
    if (live_.Broken()) {
        r.reason = L"index inconsistent; not saved";
        return r;
    }
    VolumeIdentity identity;
    if (StoreResult s = GetVolumeIdentity(drive_, identity); !s.ok) return fail(s);
    std::wstring folder;
    if (StoreResult s = PrepareCacheFolder(true, folder); !s.ok) return fail(s);
    r.path = CacheFilePath(folder, identity);

    const auto t0 = std::chrono::steady_clock::now();
    CacheMeta meta;
    std::memcpy(meta.volumeGuid, identity.guid, sizeof(identity.guid));
    meta.volumeSerial = identity.serial;
    meta.journalId = journalId_;
    meta.appliedUsn = appliedUsn_;
    for (const auto& [id, unused] : open_.Ids()) {
        (void)unused;
        meta.openFileIds.push_back(id);
    }
    std::sort(meta.openFileIds.begin(), meta.openFileIds.end());
    const std::vector<std::byte> bytes = SerializeIndex(live_.Compacted(), meta);
    r.compactSeconds = SecondsSince(t0);
    r.bytes = bytes.size();
    const auto t1 = std::chrono::steady_clock::now();
    if (StoreResult s = WriteFileReplacing(r.path, bytes); !s.ok) return fail(s);
    r.writeSeconds = SecondsSince(t1);
    r.ok = true;
    return r;
}

IdProbe NtfsLiveVolume::ProbeId(std::uint64_t id) const {
    IdProbe p;
    const std::uint64_t record = id & kRecordNumberMask;
    std::vector<std::uint64_t> storage((offsetof(QUERY_FILE_LAYOUT_INPUT, Filter) + sizeof(FILE_REFERENCE_RANGE) + 7) / 8);
    auto* input = reinterpret_cast<QUERY_FILE_LAYOUT_INPUT*>(storage.data());
    input->FilterEntryCount = 1;
    input->FilterType = QUERY_FILE_LAYOUT_FILTER_TYPE_FILEID;
    input->Filter.FileReferenceRanges[0].StartingFileReferenceNumber = record;
    input->Filter.FileReferenceRanges[0].EndingFileReferenceNumber = record;
    std::vector<std::byte> out(256 * 1024);
    LayoutParseStats stats;
    for (bool first = true;; first = false) {
        input->Flags = QUERY_FILE_LAYOUT_INCLUDE_NAMES | QUERY_FILE_LAYOUT_INCLUDE_STREAMS |
                       QUERY_FILE_LAYOUT_INCLUDE_STREAMS_WITH_NO_CLUSTERS_ALLOCATED |
                       (first ? QUERY_FILE_LAYOUT_RESTART : 0);
        DWORD returned = 0;
        if (!DeviceIoControl(volume_, FSCTL_QUERY_FILE_LAYOUT, input, static_cast<DWORD>(storage.size() * 8),
                             out.data(), static_cast<DWORD>(out.size()), &returned, nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_HANDLE_EOF) p.error = error;
            break;
        }
        const std::uint32_t count = ParseLayoutBuffer(out.data(), returned, stats, [&](const LayoutEntry& e) {
            ++p.entries;
            if (p.entries == 1) p.returnedId = e.id;
            if (e.id != id) return;
            p.found = true;
            p.attributes = e.attributes;
            p.hasData = e.hasData;
            p.logicalBytes = e.dataLogicalBytes;
            for (const LayoutLink& l : e.links) p.links.push_back({l.parentId, std::wstring(l.name)});
        });
        if (count == 0) break;
    }
    DirectoryLister lister;
    p.listError = lister.List(root_, [&](const ListedItem& item) {
        if (item.hasId && item.id == id) p.listedInRoot = true;
    });
    return p;
}

CatchUpResult NtfsLiveVolume::CatchUp() {
    CatchUpResult r;
    r.fromUsn = r.toUsn = appliedUsn_;
    if (live_.Broken()) return Rescan(r, L"index inconsistent");

    // ---- journal state ----------------------------------------------------------
    USN_JOURNAL_DATA_V0 journal{};
    DWORD returned = 0;
    if (!DeviceIoControl(volume_, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &journal, sizeof(journal), &returned, nullptr)) {
        return Rescan(r, L"cannot query the journal", GetLastError());
    }
    if (journal.UsnJournalID != journalId_) return Rescan(r, L"journal ID changed");
    if (appliedUsn_ < journal.FirstUsn || appliedUsn_ < journal.LowestValidUsn) {
        return Rescan(r, L"journal records were trimmed");
    }

    // ---- read changes to the end of the journal ---------------------------------
    const auto t0 = std::chrono::steady_clock::now();
    ChangeCollector collector;
    std::vector<std::uint64_t> buffer(1024 * 1024 / sizeof(std::uint64_t));
    READ_USN_JOURNAL_DATA_V1 read{};
    read.StartUsn = appliedUsn_;
    read.ReasonMask = 0xFFFFFFFFu;
    read.ReturnOnlyOnClose = 0;  // open files (logs, -wal) must not lag
    read.Timeout = 0;
    read.BytesToWaitFor = 0;     // return at the end of the journal
    read.UsnJournalID = journalId_;
    read.MinMajorVersion = 2;
    read.MaxMajorVersion = 2;
    for (;;) {
        if (!DeviceIoControl(volume_, FSCTL_READ_USN_JOURNAL, &read, sizeof(read), buffer.data(),
                             static_cast<DWORD>(buffer.size() * sizeof(std::uint64_t)), &returned, nullptr)) {
            return Rescan(r, L"cannot read the journal", GetLastError());
        }
        std::int64_t next = 0;
        const UsnParseStatus status = ParseUsnBuffer(reinterpret_cast<const std::byte*>(buffer.data()), returned, next,
                                                     [&](const UsnChange& c) {
                                                         ++r.records;
                                                         collector.Add(c);
                                                         open_.Observe(c);
                                                         seen_[c.id] |= c.reason;
                                                     });
        if (status != UsnParseStatus::Ok) return Rescan(r, L"malformed journal record");
        if (collector.Size() > kMaxChangedIdsPerCatchUp) return Rescan(r, L"too many changes");
        if (next == read.StartUsn) break;  // no more records
        read.StartUsn = next;
    }
    const std::int64_t endUsn = read.StartUsn;
    r.readSeconds = SecondsSince(t0);
    std::vector<ChangedId> changed = collector.Take();
    r.changedIds = changed.size();
    // Files still open may have grown without new records: re-query them too.
    {
        std::unordered_set<std::uint64_t> have;
        for (const ChangedId& c : changed) have.insert(c.id);
        for (const auto& [id, unused] : open_.Ids()) {
            (void)unused;
            if (!have.count(id)) changed.push_back({id, 0});
        }
        r.openTracked = open_.Size();
    }
    if (changed.size() > kMaxChangedIdsPerCatchUp) return Rescan(r, L"too many changes");
    if (changed.empty()) {
        appliedUsn_ = endUsn;
        r.toUsn = endUsn;
        return r;
    }

    // ---- re-query their current state ---------------------------------------------
    const auto t1 = std::chrono::steady_clock::now();
    std::vector<std::uint64_t> ids;
    ids.reserve(changed.size());
    for (const ChangedId& c : changed) ids.push_back(c.id);
    const auto ranges = RecordRanges(ids);
    r.ranges = ranges.size();
    std::vector<std::byte> out(1024 * 1024);
    std::vector<CurrentEntry> entries;
    bool malformed = false;
    for (size_t begin = 0; begin < ranges.size(); begin += kMaxRangesPerQuery) {
        const size_t end = std::min(ranges.size(), begin + kMaxRangesPerQuery);
        const DWORD error = QueryRanges(volume_, ranges, begin, end, out, entries, r.layoutCalls, malformed);
        if (error != ERROR_SUCCESS) return Rescan(r, L"cannot re-query changed files", error);
    }
    if (malformed) return Rescan(r, L"malformed layout entry");
    r.entries = entries.size();
    {
        std::unordered_set<std::uint64_t> requeried;
        for (const CurrentEntry& e : entries) requeried.insert(e.id);
        for (const ChangedId& c : changed) {
            if (requeried.count(c.id) || (c.reasons & USN_REASON_FILE_DELETE)) continue;
            ++r.vanished;
            if (r.vanishedSamples.size() < 16) r.vanishedSamples.push_back(c);
        }
    }

    std::unordered_set<std::uint64_t> rootListing;
    {
        DirectoryLister lister;
        const DWORD error = lister.List(root_, [&](const ListedItem& item) { rootListing.insert(item.id); });
        if (error != ERROR_SUCCESS) return Rescan(r, L"cannot list the root folder", error);
    }
    r.querySeconds = SecondsSince(t1);

    // ---- apply ------------------------------------------------------------------------
    const auto t2 = std::chrono::steady_clock::now();
    UpdateRules rules;
    rules.listedInRoot = [&](std::uint64_t id) { return rootListing.count(id) != 0; };
    rules.readReparseTag = [&](std::uint64_t id, std::uint32_t& tag) { return ReadReparseTagById(root_, id, tag); };
    r.update = live_.Apply(changed, entries, rules);
    r.applySeconds = SecondsSince(t2);
    if (r.update.status != UpdateStatus::Applied) return Rescan(r, r.update.reason);
    // Stop tracking files that no longer exist under the same ID.
    {
        std::unordered_set<std::uint64_t> present;
        for (const CurrentEntry& e : entries) present.insert(e.id);
        std::vector<std::uint64_t> gone;
        for (const auto& [id, unused] : open_.Ids()) {
            (void)unused;
            if (!present.count(id)) gone.push_back(id);
        }
        for (std::uint64_t id : gone) open_.Forget(id);
    }
    appliedUsn_ = endUsn;
    r.toUsn = endUsn;
    return r;
}

}  // namespace dirsize
