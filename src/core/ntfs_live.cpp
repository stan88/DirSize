#include "core/ntfs_live.h"

#include <winioctl.h>

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
    const std::wstring volumePath = std::wstring(L"\\\\.\\") + drive + L":";
    v->volume_ = CreateFileW(volumePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    const std::wstring rootPath = std::wstring(1, drive) + L":\\";
    v->root_ = OpenDirectoryForListing(rootPath.c_str());
    if (v->volume_ == INVALID_HANDLE_VALUE || v->root_ == INVALID_HANDLE_VALUE) {
        build.ok = false;
        build.failedStep = L"reopen volume for updates";
        build.error = GetLastError();
        return nullptr;
    }
    v->journalId_ = journalId;
    v->appliedUsn_ = startUsn;
    return v;
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
                                                         seen_.insert(c.id);
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
