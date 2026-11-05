// usn-probe: Phase 3 feasibility probe for the NTFS change journal.
//
// Measures what the documentation does not state:
//   1. Which volume handles can query and read the journal (run it both
//      unelevated and elevated).
//   2. How much history the journal keeps (oldest record time, records per day).
//   3. Whether USN_RECORD_V2.FileReferenceNumber is the same ID space as the
//      Phase 2 index.
//   4. How FSCTL_QUERY_FILE_LAYOUT with QUERY_FILE_LAYOUT_FILTER_TYPE_FILEID
//      matches IDs, and how fast it is against OpenFileById + FileStandardInfo.
// Measurement code only; not part of the core.
//
// Structures: Windows SDK 10.0.26100.0 um/winioctl.h. Records are requested
// as version 2 only (MinMajorVersion = MaxMajorVersion = 2).
//
// Usage: usn-probe <drive letter>      e.g. usn-probe C

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/dir_listing.h"
#include "core/layout_parser.h"
#include "core/ntfs_index.h"

namespace {

// ---- output -----------------------------------------------------------------

void Line(const wchar_t* format, ...) {
    wchar_t buffer[2048];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    std::wstring text = buffer;
    text += L'\n';
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), bytes, nullptr, nullptr);
    std::fwrite(utf8.data(), 1, utf8.size(), stdout);
    std::fflush(stdout);
}

double Seconds(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

std::wstring FormatTime(std::int64_t fileTime) {
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(fileTime);
    ft.dwHighDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(fileTime) >> 32);
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&ft, &st)) return L"(invalid time)";
    wchar_t buffer[64];
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u UTC", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond);
    return buffer;
}

std::int64_t NowFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

constexpr double kFileTimePerHour = 36e9;  // 100 ns units

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != 0;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

class Handle {
public:
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : h_(h) {}
    ~Handle() {
        if (valid()) CloseHandle(h_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return h_; }
    bool valid() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }

private:
    HANDLE h_;
};

HANDLE OpenVolume(wchar_t drive, DWORD access) {
    const std::wstring path = std::wstring(L"\\\\.\\") + drive + L":";
    return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, 0, nullptr);
}

bool QueryJournal(HANDLE volume, USN_JOURNAL_DATA_V0& data, DWORD& error) {
    DWORD returned = 0;
    if (!DeviceIoControl(volume, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &data, sizeof(data), &returned, nullptr)) {
        error = GetLastError();
        return false;
    }
    error = 0;
    return true;
}

// ---- journal records ----------------------------------------------------------

struct UsnRecord {
    std::uint64_t id = 0;
    std::uint64_t parentId = 0;
    std::int64_t usn = 0;
    std::int64_t timeStamp = 0;
    std::uint32_t reason = 0;
    std::uint32_t attributes = 0;
};

struct ReadStats {
    std::uint64_t calls = 0;
    std::uint64_t bytes = 0;
    std::uint64_t records = 0;
    std::uint64_t badRecords = 0;     // failed bounds or version checks
    std::uint64_t otherVersions = 0;  // MajorVersion != 2
    DWORD error = 0;                  // the error that stopped reading, 0 = reached the end
};

// Reads records from `startUsn` to the current end of the journal.
template <typename OnRecord>
ReadStats ReadJournal(HANDLE volume, std::uint64_t journalId, std::int64_t startUsn, OnRecord onRecord) {
    ReadStats stats;
    std::vector<std::uint64_t> buffer(1024 * 1024 / sizeof(std::uint64_t));  // 1 MB, 8-byte aligned
    READ_USN_JOURNAL_DATA_V1 input{};
    input.StartUsn = startUsn;
    input.ReasonMask = 0xFFFFFFFFu;
    input.ReturnOnlyOnClose = 0;
    input.Timeout = 0;
    input.BytesToWaitFor = 0;
    input.UsnJournalID = journalId;
    input.MinMajorVersion = 2;
    input.MaxMajorVersion = 2;
    for (;;) {
        DWORD returned = 0;
        if (!DeviceIoControl(volume, FSCTL_READ_USN_JOURNAL, &input, sizeof(input), buffer.data(),
                             static_cast<DWORD>(buffer.size() * sizeof(std::uint64_t)), &returned, nullptr)) {
            stats.error = GetLastError();
            return stats;
        }
        ++stats.calls;
        stats.bytes += returned;
        const auto* base = reinterpret_cast<const std::byte*>(buffer.data());
        if (returned < sizeof(USN)) {
            stats.error = ERROR_INVALID_DATA;
            return stats;
        }
        USN next = 0;
        std::memcpy(&next, base, sizeof(next));
        size_t offset = sizeof(USN);
        while (offset < returned) {
            USN_RECORD_COMMON_HEADER header{};
            if (returned - offset < sizeof(header)) {
                ++stats.badRecords;
                break;
            }
            std::memcpy(&header, base + offset, sizeof(header));
            if (header.RecordLength < sizeof(header) || header.RecordLength > returned - offset) {
                ++stats.badRecords;
                break;
            }
            if (header.MajorVersion != 2) {
                ++stats.otherVersions;
            } else if (header.RecordLength < offsetof(USN_RECORD_V2, FileName)) {
                ++stats.badRecords;
            } else {
                USN_RECORD_V2 r{};
                std::memcpy(&r, base + offset, offsetof(USN_RECORD_V2, FileName));
                if (static_cast<size_t>(r.FileNameOffset) + r.FileNameLength > header.RecordLength) {
                    ++stats.badRecords;
                } else {
                    ++stats.records;
                    onRecord(UsnRecord{r.FileReferenceNumber, r.ParentFileReferenceNumber, r.Usn,
                                       r.TimeStamp.QuadPart, r.Reason, r.FileAttributes});
                }
            }
            offset += header.RecordLength;
        }
        if (next == input.StartUsn) return stats;  // no more records
        input.StartUsn = next;
    }
}

// ---- layout by file ID ----------------------------------------------------------

struct FilterResult {
    DWORD error = 0;  // first error other than ERROR_HANDLE_EOF
    std::uint64_t calls = 0;
    std::vector<std::uint64_t> ids;  // returned file IDs in order
    double seconds = 0;
};

FilterResult QueryLayoutByIds(HANDLE volume, const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges,
                              std::vector<std::byte>& out) {
    FilterResult result;
    const size_t inputBytes = offsetof(QUERY_FILE_LAYOUT_INPUT, Filter) + ranges.size() * sizeof(FILE_REFERENCE_RANGE);
    std::vector<std::uint64_t> inputStorage((inputBytes + 7) / 8 + 1);
    auto* input = reinterpret_cast<QUERY_FILE_LAYOUT_INPUT*>(inputStorage.data());
    input->FilterEntryCount = static_cast<DWORD>(ranges.size());
    input->FilterType = QUERY_FILE_LAYOUT_FILTER_TYPE_FILEID;
    for (size_t i = 0; i < ranges.size(); ++i) {
        input->Filter.FileReferenceRanges[i].StartingFileReferenceNumber = ranges[i].first;
        input->Filter.FileReferenceRanges[i].EndingFileReferenceNumber = ranges[i].second;
    }
    dirsize::LayoutParseStats stats;
    const auto t0 = std::chrono::steady_clock::now();
    for (bool first = true;; first = false) {
        input->Flags = QUERY_FILE_LAYOUT_INCLUDE_NAMES | QUERY_FILE_LAYOUT_INCLUDE_STREAMS |
                       QUERY_FILE_LAYOUT_INCLUDE_STREAMS_WITH_NO_CLUSTERS_ALLOCATED |
                       (first ? QUERY_FILE_LAYOUT_RESTART : 0);
        DWORD returned = 0;
        if (!DeviceIoControl(volume, FSCTL_QUERY_FILE_LAYOUT, input, static_cast<DWORD>(inputBytes), out.data(),
                             static_cast<DWORD>(out.size()), &returned, nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_HANDLE_EOF) result.error = error;
            break;
        }
        ++result.calls;
        const std::uint32_t count = dirsize::ParseLayoutBuffer(
            out.data(), returned, stats, [&](const dirsize::LayoutEntry& e) { result.ids.push_back(e.id); });
        if (count == 0) break;
    }
    result.seconds = Seconds(t0);
    return result;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> Exact(const std::vector<std::uint64_t>& ids) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    for (std::uint64_t id : ids) ranges.push_back({id, id});
    return ranges;
}

bool Contains(const std::vector<std::uint64_t>& v, std::uint64_t id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}

std::uint64_t CountMatches(const std::vector<std::uint64_t>& returned, const std::vector<std::uint64_t>& wanted) {
    std::unordered_set<std::uint64_t> w(wanted.begin(), wanted.end());
    std::uint64_t n = 0;
    for (std::uint64_t id : returned) n += w.count(id);
    return n;
}

// The documented per-file alternative: OpenFileById + FileStandardInfo.
void TimeOpenById(HANDLE hint, const std::vector<std::uint64_t>& ids) {
    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t ok = 0;
    std::map<DWORD, std::uint64_t> errors;
    for (std::uint64_t id : ids) {
        FILE_ID_DESCRIPTOR d{};
        d.dwSize = sizeof(d);
        d.Type = FileIdType;
        d.FileId.QuadPart = static_cast<LONGLONG>(id);
        HANDLE h = OpenFileById(hint, &d, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
        if (h == INVALID_HANDLE_VALUE) {
            ++errors[GetLastError()];
            continue;
        }
        FILE_STANDARD_INFO info{};
        if (GetFileInformationByHandleEx(h, FileStandardInfo, &info, sizeof(info))) {
            ++ok;
        } else {
            ++errors[GetLastError()];
        }
        CloseHandle(h);
    }
    std::wstring e;
    for (const auto& [code, n] : errors) e += L" error " + std::to_wstring(code) + L" x" + std::to_wstring(n);
    Line(L"    OpenFileById+FileStandardInfo: %zu IDs, %.3f s, ok %llu%s", ids.size(), Seconds(t0), ok, e.c_str());
}

// ---- sections ---------------------------------------------------------------------

void ProbeAccess(wchar_t drive) {
    Line(L"== Access (this process %s elevated)", IsElevated() ? L"is" : L"is not");
    struct Mode {
        DWORD access;
        const wchar_t* name;
    };
    for (const Mode& m : {Mode{GENERIC_READ, L"GENERIC_READ"}, Mode{FILE_READ_ATTRIBUTES, L"FILE_READ_ATTRIBUTES"},
                          Mode{0, L"0 (no access)"}}) {
        Handle volume(OpenVolume(drive, m.access));
        if (!volume.valid()) {
            Line(L"  open %s: error %lu", m.name, GetLastError());
            continue;
        }
        USN_JOURNAL_DATA_V0 data{};
        DWORD error = 0;
        const bool queried = QueryJournal(volume.get(), data, error);
        std::wstring read;
        if (queried) {
            // Read from the current end: returns at once with no records.
            const ReadStats s = ReadJournal(volume.get(), data.UsnJournalID, data.NextUsn, [](const UsnRecord&) {});
            read = s.error == 0 ? L"ok" : L"error " + std::to_wstring(s.error);
        } else {
            // Without the journal ID, try anyway: the error shows whether access is the problem.
            const ReadStats s = ReadJournal(volume.get(), 0, 0, [](const UsnRecord&) {});
            read = s.error == 0 ? L"ok (journal ID 0)" : L"error " + std::to_wstring(s.error) + L" (journal ID 0)";
        }
        Line(L"  open %s: ok; FSCTL_QUERY_USN_JOURNAL %s; FSCTL_READ_USN_JOURNAL %s", m.name,
             queried ? L"ok" : (L"error " + std::to_wstring(error)).c_str(), read.c_str());
    }
}

void ProbeJournal(HANDLE volume, const USN_JOURNAL_DATA_V0& j) {
    Line(L"");
    Line(L"== Journal");
    Line(L"  ID 0x%016llx", j.UsnJournalID);
    Line(L"  FirstUsn %lld, NextUsn %lld, LowestValidUsn %lld, MaxUsn %lld", j.FirstUsn, j.NextUsn, j.LowestValidUsn,
         j.MaxUsn);
    Line(L"  MaximumSize %llu B, AllocationDelta %llu B, readable span %lld B", j.MaximumSize, j.AllocationDelta,
         j.NextUsn - j.FirstUsn);

    std::int64_t firstTime = 0, lastTime = 0;
    std::unordered_set<std::uint64_t> ids;
    std::map<int, std::uint64_t> reasonBits;
    std::map<std::int64_t, std::uint64_t> perDay;  // days before now -> records
    const std::int64_t now = NowFileTime();
    std::uint64_t closeRecords = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const ReadStats s = ReadJournal(volume, j.UsnJournalID, j.FirstUsn, [&](const UsnRecord& r) {
        if (firstTime == 0) firstTime = r.timeStamp;
        lastTime = r.timeStamp;
        ids.insert(r.id);
        for (int b = 0; b < 32; ++b) {
            if (r.reason & (1u << b)) ++reasonBits[b];
        }
        if (r.reason & USN_REASON_CLOSE) ++closeRecords;
        ++perDay[static_cast<std::int64_t>((now - r.timeStamp) / (kFileTimePerHour * 24))];
    });
    const double seconds = Seconds(t0);
    Line(L"  Read whole journal: %.3f s, %llu calls, %llu B, %llu records, %zu distinct file IDs", seconds, s.calls,
         s.bytes, s.records, ids.size());
    Line(L"  Bad records %llu, other versions %llu, ended with %s", s.badRecords, s.otherVersions,
         s.error == 0 ? L"end of journal" : (L"error " + std::to_wstring(s.error)).c_str());
    if (s.records != 0) {
        Line(L"  Oldest record %s, newest %s, now %s", FormatTime(firstTime).c_str(), FormatTime(lastTime).c_str(),
             FormatTime(now).c_str());
        Line(L"  History kept: %.1f hours", (now - firstTime) / kFileTimePerHour);
        Line(L"  Records with USN_REASON_CLOSE: %llu", closeRecords);
    }
    std::wstring bitsHex;
    for (const auto& [b, n] : reasonBits) {
        wchar_t item[48];
        swprintf_s(item, L" %08lx=%llu", 1ul << b, n);
        bitsHex += item;
    }
    Line(L"  Reason flags (flag=records):%s", bitsHex.c_str());
    std::wstring days;
    for (const auto& [d, n] : perDay) days += L" " + std::to_wstring(d) + L"d=" + std::to_wstring(n);
    Line(L"  Records by days before now:%s", days.c_str());
}

bool InIndex(const dirsize::VolumeIndex& index, std::uint64_t id) {
    // The NTFS engine keeps both tables sorted by ID.
    auto f = std::lower_bound(index.files.begin(), index.files.end(), id,
                              [](const dirsize::FileEntry& e, std::uint64_t v) { return e.id < v; });
    if (f != index.files.end() && f->id == id) return true;
    auto d = std::lower_bound(index.dirs.begin(), index.dirs.end(), id,
                              [](const dirsize::DirEntry& e, std::uint64_t v) { return e.id < v; });
    return d != index.dirs.end() && d->id == id;
}

void ProbeIds(HANDLE volume, HANDLE rootHint, const USN_JOURNAL_DATA_V0& j, const dirsize::VolumeIndex& index) {
    Line(L"");
    Line(L"== Journal file IDs vs index");
    std::unordered_map<std::uint64_t, std::uint64_t> byRecord;  // low 48 bits -> full ID
    for (const auto& f : index.files) byRecord[f.id & 0x0000FFFFFFFFFFFFull] = f.id;
    for (const auto& d : index.dirs) byRecord[d.id & 0x0000FFFFFFFFFFFFull] = d.id;

    std::unordered_set<std::uint64_t> seen;
    std::uint64_t found = 0, sameRecordOtherSequence = 0, missing = 0, withSequence = 0;
    std::vector<std::uint64_t> missingSample;
    // Records written since the index was built say nothing about it; count
    // only those before the index's starting USN.
    ReadJournal(volume, j.UsnJournalID, j.FirstUsn, [&](const UsnRecord& r) {
        if (r.usn >= index.usn.nextUsn || !seen.insert(r.id).second) return;
        if (r.id >> 48) ++withSequence;
        if (InIndex(index, r.id)) {
            ++found;
            return;
        }
        auto it = byRecord.find(r.id & 0x0000FFFFFFFFFFFFull);
        if (it != byRecord.end()) {
            ++sameRecordOtherSequence;  // record reused by a newer file
        } else {
            ++missing;
            if (missingSample.size() < 2000) missingSample.push_back(r.id);
        }
    });
    Line(L"  Distinct IDs before the index's USN: %zu (with nonzero sequence %llu)", seen.size(), withSequence);
    Line(L"  In index %llu; same MFT record but other sequence %llu; not in index %llu", found,
         sameRecordOtherSequence, missing);
    // Of the IDs not in the index, how many still open (the rest are deleted)?
    std::uint64_t opens = 0;
    std::map<DWORD, std::uint64_t> errors;
    for (std::uint64_t id : missingSample) {
        FILE_ID_DESCRIPTOR d{};
        d.dwSize = sizeof(d);
        d.Type = FileIdType;
        d.FileId.QuadPart = static_cast<LONGLONG>(id);
        HANDLE h = OpenFileById(rootHint, &d, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
        if (h == INVALID_HANDLE_VALUE) {
            ++errors[GetLastError()];
        } else {
            ++opens;
            CloseHandle(h);
        }
    }
    std::wstring e;
    for (const auto& [code, n] : errors) e += L" error " + std::to_wstring(code) + L" x" + std::to_wstring(n);
    Line(L"  Of %zu not-in-index IDs: open by ID ok %llu (excluded items such as $Extend, or new);%s",
         missingSample.size(), opens, e.c_str());
}

void ProbeFilter(HANDLE volume, HANDLE rootHint, const dirsize::VolumeIndex& index, const std::wstring& tempDir) {
    Line(L"");
    Line(L"== FSCTL_QUERY_FILE_LAYOUT with QUERY_FILE_LAYOUT_FILTER_TYPE_FILEID");
    std::vector<std::byte> out(1024 * 1024);
    if (index.files.size() < 20000) {
        Line(L"  too few files in the index");
        return;
    }
    // Two neighbours in ID order, both with a nonzero sequence number.
    size_t k = index.files.size() / 2;
    while (k + 1 < index.files.size() && ((index.files[k].id >> 48) == 0 || (index.files[k + 1].id >> 48) == 0)) ++k;
    const std::uint64_t a = index.files[k].id, b = index.files[k + 1].id;
    Line(L"  Sample IDs a=0x%016llx b=0x%016llx", a, b);

    auto show = [&](const wchar_t* what, const FilterResult& r) {
        std::wstring ids;
        for (size_t i = 0; i < r.ids.size() && i < 8; ++i) {
            wchar_t item[24];
            swprintf_s(item, L" %016llx", r.ids[i]);
            ids += item;
        }
        Line(L"  %s: error %lu, calls %llu, returned %zu:%s%s", what, r.error, r.calls, r.ids.size(), ids.c_str(),
             r.ids.size() > 8 ? L" ..." : L"");
    };
    show(L"[a,a] full ID", QueryLayoutByIds(volume, {{a, a}}, out));
    const std::uint64_t ra = a & 0x0000FFFFFFFFFFFFull;
    show(L"[a,a] record number only", QueryLayoutByIds(volume, {{ra, ra}}, out));
    show(L"[a,b] full IDs (is b included?)", QueryLayoutByIds(volume, {{a, b}}, out));
    show(L"[a,b-1] full IDs", QueryLayoutByIds(volume, {{a, b - 1}}, out));
    show(L"two ranges [b,b],[a,a] (unsorted)", QueryLayoutByIds(volume, {{b, b}, {a, a}}, out));

    // A file that exists, then the same ID after deletion.
    const std::wstring path = tempDir + L"\\usn-probe-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, "probe", 5, &written, nullptr);
        std::uint64_t id = 0;
        dirsize::QueryFileId(h, id);
        CloseHandle(h);
        Line(L"  Temp file id 0x%016llx", id);
        show(L"new temp file", QueryLayoutByIds(volume, {{id, id}}, out));
        DeleteFileW(path.c_str());
        show(L"same ID after delete", QueryLayoutByIds(volume, {{id, id}}, out));
    } else {
        Line(L"  cannot create temp file: error %lu", GetLastError());
    }

    // Many single-ID ranges, evenly spread over the index, against per-file opens.
    for (size_t n : {size_t{10}, size_t{1000}, size_t{10000}, size_t{100000}}) {
        std::vector<std::uint64_t> ids;
        const size_t step = std::max<size_t>(1, index.files.size() / n);
        for (size_t i = 0; i < index.files.size() && ids.size() < n; i += step) ids.push_back(index.files[i].id);
        const FilterResult r = QueryLayoutByIds(volume, Exact(ids), out);
        Line(L"  %zu IDs: error %lu, %.3f s, calls %llu, returned %zu, matching %llu", ids.size(), r.error, r.seconds,
             r.calls, r.ids.size(), CountMatches(r.ids, ids));
        if (n <= 10000) TimeOpenById(rootHint, ids);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || !iswalpha(argv[1][0])) {
        Line(L"usage: usn-probe <drive letter>");
        return 2;
    }
    const wchar_t drive = static_cast<wchar_t>(towupper(argv[1][0]));
    Line(L"Volume %c:", drive);
    ProbeAccess(drive);
    if (!IsElevated()) {
        Line(L"");
        Line(L"Not elevated: journal history, ID and filter checks skipped.");
        return 0;
    }

    Handle volume(OpenVolume(drive, GENERIC_READ));
    USN_JOURNAL_DATA_V0 journal{};
    DWORD error = 0;
    if (!volume.valid() || !QueryJournal(volume.get(), journal, error)) {
        Line(L"cannot open or query the journal: error %lu", volume.valid() ? error : GetLastError());
        return 1;
    }
    ProbeJournal(volume.get(), journal);

    Line(L"");
    Line(L"Building the Phase 2 index ...");
    dirsize::NtfsIndexResult ntfs = dirsize::BuildNtfsIndex(drive);
    if (!ntfs.ok) {
        Line(L"index failed: %s (error %lu)", ntfs.failedStep, ntfs.error);
        return 1;
    }
    Line(L"  %zu files, %zu folders, index starts at USN %lld", ntfs.index.files.size(), ntfs.index.dirs.size(),
         ntfs.index.usn.nextUsn);

    const std::wstring rootPath = std::wstring(1, drive) + L":\\";
    Handle root(dirsize::OpenDirectoryForListing(rootPath.c_str()));
    ProbeIds(volume.get(), root.get(), journal, ntfs.index);

    wchar_t temp[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, temp);
    std::wstring tempDir(temp, n);
    while (!tempDir.empty() && tempDir.back() == L'\\') tempDir.pop_back();
    if (tempDir.empty() || towupper(tempDir[0]) != drive) {
        tempDir = std::wstring(1, drive) + L":";  // the probed volume's root; the file is deleted at once
    }
    ProbeFilter(volume.get(), root.get(), ntfs.index, tempDir);
    return 0;
}
