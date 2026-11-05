// dirsize-cli: command-line front end for the DirSize indexer.
//
// scan:   Phase 1 baseline scanner (directory enumeration).
// verify: builds the Phase 2 folder index of an NTFS volume with both engines
//         (FSCTL_QUERY_FILE_LAYOUT and the directory walker) and compares them
//         folder by folder.

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <unordered_map>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

#include "core/dir_listing.h"
#include "core/dir_walker.h"
#include "core/index.h"
#include "core/ntfs_index.h"
#include "core/ntfs_live.h"
#include "core/path_util.h"
#include "core/walk_index.h"
#include "client/pipe_client.h"

namespace {

using dirsize::ChildFolder;
using dirsize::WalkResult;

// ---- output -----------------------------------------------------------------

void Write(std::wstring_view text) {
    if (text.empty()) {
        return;
    }
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                    nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), bytes, nullptr,
                        nullptr);
    std::fwrite(utf8.data(), 1, utf8.size(), stdout);
}

void WriteLine(std::wstring_view text = {}) {
    Write(text);
    Write(L"\n");
}

std::wstring Format(const wchar_t* format, ...) {
    wchar_t buffer[512];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

std::wstring FormatBytes(std::uint64_t bytes) {
    static const wchar_t* const kUnits[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? Format(L"%llu B", bytes) : Format(L"%.2f %s", value, kUnits[unit]);
}

std::wstring JsonEscape(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size() + 2);
    for (wchar_t c : text) {
        switch (c) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20) {
                    out += Format(L"\\u%04x", static_cast<unsigned>(c));
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// ---- environment ------------------------------------------------------------

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                    elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

// Enables SeBackupPrivilege on the process token. Combined with
// FILE_FLAG_BACKUP_SEMANTICS, this lets an elevated process list folders whose
// ACLs would otherwise deny access. Returns false when the privilege is not
// held (for example, when not elevated).
bool EnableBackupPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool enabled = false;
    if (LookupPrivilegeValueW(nullptr, SE_BACKUP_NAME, &privileges.Privileges[0].Luid) &&
        AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr)) {
        // AdjustTokenPrivileges succeeds even if the privilege is not held;
        // ERROR_NOT_ALL_ASSIGNED reports that case.
        enabled = GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(token);
    return enabled;
}

std::uint64_t PeakWorkingSetBytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return counters.PeakWorkingSetSize;
    }
    return 0;
}

std::uint64_t WorkingSetBytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return counters.WorkingSetSize;
    }
    return 0;
}

// Process resource use at one moment (GetProcessTimes, GetProcessIoCounters,
// GetProcessMemoryInfo). Differences between two snapshots give the cost of
// the work in between.
struct Usage {
    std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now();
    std::uint64_t kernel100ns = 0, user100ns = 0;
    IO_COUNTERS io{};
    PROCESS_MEMORY_COUNTERS memory{};
};

Usage TakeUsage() {
    Usage u;
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        u.kernel100ns = (static_cast<std::uint64_t>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
        u.user100ns = (static_cast<std::uint64_t>(user.dwHighDateTime) << 32) | user.dwLowDateTime;
    }
    GetProcessIoCounters(GetCurrentProcess(), &u.io);
    u.memory.cb = sizeof(u.memory);
    GetProcessMemoryInfo(GetCurrentProcess(), &u.memory, sizeof(u.memory));
    return u;
}

// CPU time is summed over all threads, so the average can exceed 100% of one
// core; "of one core" makes that explicit.
void PrintResources(const wchar_t* what, const Usage& a, const Usage& b) {
    const double wall = std::chrono::duration<double>(b.at - a.at).count();
    const double kernel = (b.kernel100ns - a.kernel100ns) / 1e7;
    const double user = (b.user100ns - a.user100ns) / 1e7;
    const double percent = wall > 0 ? (kernel + user) / wall * 100 : 0;
    WriteLine(Format(L"  Resources (%s): wall %.3f s, CPU %.3f s (kernel %.3f, user %.3f), %.2f%% of one core", what,
                     wall, kernel + user, kernel, user, percent));
    WriteLine(Format(L"    I/O: read ops %llu (%s), other ops %llu (%s), write ops %llu (%s)",
                     b.io.ReadOperationCount - a.io.ReadOperationCount,
                     FormatBytes(b.io.ReadTransferCount - a.io.ReadTransferCount).c_str(),
                     b.io.OtherOperationCount - a.io.OtherOperationCount,
                     FormatBytes(b.io.OtherTransferCount - a.io.OtherTransferCount).c_str(),
                     b.io.WriteOperationCount - a.io.WriteOperationCount,
                     FormatBytes(b.io.WriteTransferCount - a.io.WriteTransferCount).c_str()));
    WriteLine(Format(L"    Memory now: working set %s, commit %s; peaks so far: working set %s, commit %s",
                     FormatBytes(b.memory.WorkingSetSize).c_str(), FormatBytes(b.memory.PagefileUsage).c_str(),
                     FormatBytes(b.memory.PeakWorkingSetSize).c_str(),
                     FormatBytes(b.memory.PeakPagefileUsage).c_str()));
}

// ---- commands ---------------------------------------------------------------

struct ScanOptions {
    std::wstring path;
    size_t top = 20;
    bool json = false;
};

struct RunInfo {
    double seconds = 0;
    bool elevated = false;
    bool backupPrivilege = false;
    std::uint64_t peakBytes = 0;
};

void PrintUsage() {
    WriteLine(L"Usage:");
    WriteLine(L"  dirsize-cli scan <path> [--top N] [--json]");
    WriteLine(L"  dirsize-cli verify <drive letter>      (NTFS, elevated)");
    WriteLine(L"  dirsize-cli watch <drive letter> [seconds]  (NTFS, elevated; default 300)");
    WriteLine(L"  dirsize-cli cache save|load <drive letter>  (NTFS, elevated)");
    WriteLine(L"  dirsize-cli status [--console]              (asks the DirSize service)");
    WriteLine(L"  dirsize-cli query <folder> [--start N] [--count N] [--console]");
    WriteLine();
    WriteLine(L"Walks the folder tree under <path> and reports total size, file and");
    WriteLine(L"folder counts, elapsed time and peak memory use.");
    WriteLine(L"  --top N   list the N largest immediate sub-folders (default 20, 0 = all)");
    WriteLine(L"  --json    machine-readable output");
    WriteLine();
    WriteLine(L"verify builds the folder index of a whole NTFS volume with both engines");
    WriteLine(L"and compares them folder by folder.");
    WriteLine();
    WriteLine(L"watch builds the index, keeps it current with the USN journal for the");
    WriteLine(L"given time, then compares it with a fresh full build folder by folder.");
    WriteLine();
    WriteLine(L"cache save builds the index and saves it under ProgramData\\DirSize\\cache.");
    WriteLine(L"cache load loads it, applies the journal changes since the save, compares");
    WriteLine(L"the result with a fresh full build, and saves it again.");
    WriteLine();
    WriteLine(L"status and query ask the DirSize service through its pipe; no elevation");
    WriteLine(L"needed. --console accepts a pipe served by \"dirsize-service console\"");
    WriteLine(L"instead of the installed service (tests only).");
}

void PrintText(const std::wstring& displayPath, const WalkResult& r, const RunInfo& info,
               const std::vector<const ChildFolder*>& top) {
    const double seconds = info.seconds;
    WriteLine(Format(L"Path              %s", displayPath.c_str()));
    WriteLine(Format(L"Elevated          %s", info.elevated ? L"yes" : L"no"));
    WriteLine(Format(L"Backup privilege  %s", info.backupPrivilege ? L"enabled" : L"not available"));
    WriteLine(Format(L"Elapsed           %.3f s", seconds));
    WriteLine(Format(L"Folders           %llu", r.totals.folders));
    WriteLine(Format(L"Files             %llu", r.totals.files));
    WriteLine(Format(L"Size              %s (%llu bytes)", FormatBytes(r.totals.logicalBytes).c_str(),
                     r.totals.logicalBytes));
    WriteLine(Format(L"Size on disk      %s (%llu bytes)", FormatBytes(r.totals.allocatedBytes).c_str(),
                     r.totals.allocatedBytes));
    WriteLine(Format(L"Access denied     %llu", r.accessDenied));
    WriteLine(Format(L"Other errors      %llu", r.otherErrors));
    WriteLine(Format(L"Links skipped     %llu", r.reparsePointsSkipped));
    WriteLine(Format(L"Peak memory       %s", FormatBytes(info.peakBytes).c_str()));
    if (seconds > 0) {
        WriteLine(Format(L"Throughput        %.0f entries/s",
                         static_cast<double>(r.totals.files + r.totals.folders) / seconds));
    }

    if (!top.empty()) {
        WriteLine();
        WriteLine(L"         Size    Size on disk       Files     Folders  Name");
        for (const ChildFolder* child : top) {
            WriteLine(Format(L"%13s %15s %11llu %11llu  %s", FormatBytes(child->totals.logicalBytes).c_str(),
                             FormatBytes(child->totals.allocatedBytes).c_str(), child->totals.files,
                             child->totals.folders, child->name.c_str()));
        }
    }
}

void PrintJson(const std::wstring& displayPath, const WalkResult& r, const RunInfo& info,
               const std::vector<const ChildFolder*>& top) {
    WriteLine(L"{");
    WriteLine(Format(L"  \"path\": \"%s\",", JsonEscape(displayPath).c_str()));
    WriteLine(Format(L"  \"elevated\": %s,", info.elevated ? L"true" : L"false"));
    WriteLine(Format(L"  \"backupPrivilege\": %s,", info.backupPrivilege ? L"true" : L"false"));
    WriteLine(Format(L"  \"elapsedSeconds\": %.3f,", info.seconds));
    WriteLine(Format(L"  \"folders\": %llu,", r.totals.folders));
    WriteLine(Format(L"  \"files\": %llu,", r.totals.files));
    WriteLine(Format(L"  \"logicalBytes\": %llu,", r.totals.logicalBytes));
    WriteLine(Format(L"  \"allocatedBytes\": %llu,", r.totals.allocatedBytes));
    WriteLine(Format(L"  \"accessDenied\": %llu,", r.accessDenied));
    WriteLine(Format(L"  \"otherErrors\": %llu,", r.otherErrors));
    WriteLine(Format(L"  \"reparsePointsSkipped\": %llu,", r.reparsePointsSkipped));
    WriteLine(Format(L"  \"peakWorkingSetBytes\": %llu,", info.peakBytes));
    WriteLine(L"  \"children\": [");
    for (size_t i = 0; i < top.size(); ++i) {
        const ChildFolder* c = top[i];
        WriteLine(Format(L"    {\"name\": \"%s\", \"logicalBytes\": %llu, \"allocatedBytes\": %llu, "
                         L"\"files\": %llu, \"folders\": %llu}%s",
                         JsonEscape(c->name).c_str(), c->totals.logicalBytes, c->totals.allocatedBytes,
                         c->totals.files, c->totals.folders, i + 1 < top.size() ? L"," : L""));
    }
    WriteLine(L"  ]");
    WriteLine(L"}");
}

int RunScan(const ScanOptions& options) {
    const std::wstring extended = dirsize::ToExtendedPath(options.path);
    if (extended.empty()) {
        WriteLine(Format(L"error: invalid path: %s", options.path.c_str()));
        return 2;
    }
    const std::wstring displayPath = dirsize::ToDisplayPath(extended);

    RunInfo info;
    info.elevated = IsElevated();
    info.backupPrivilege = EnableBackupPrivilege();

    const auto start = std::chrono::steady_clock::now();
    const WalkResult result = dirsize::WalkTree(extended);
    info.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    if (!result.rootOpened) {
        WriteLine(Format(L"error: cannot open %s (Win32 error %lu)", displayPath.c_str(), result.rootError));
        return 1;
    }

    std::vector<const ChildFolder*> top;
    top.reserve(result.children.size());
    for (const ChildFolder& child : result.children) {
        top.push_back(&child);
    }
    std::sort(top.begin(), top.end(), [](const ChildFolder* a, const ChildFolder* b) {
        return a->totals.logicalBytes > b->totals.logicalBytes;
    });
    if (options.top > 0 && top.size() > options.top) {
        top.resize(options.top);
    }

    info.peakBytes = PeakWorkingSetBytes();
    if (options.json) {
        PrintJson(displayPath, result, info, top);
    } else {
        PrintText(displayPath, result, info, top);
    }
    return 0;
}

// ---- verify -------------------------------------------------------------------

using dirsize::DirIndex;
using dirsize::kNoDir;
using dirsize::Totals;
using dirsize::VolumeIndex;

void PrintIndexSummary(const VolumeIndex& index) {
    const Totals& t = index.dirs[index.root].subtree;
    WriteLine(Format(L"  Folders %llu, files %llu, size %llu B, on disk %llu B", t.folders, t.files, t.logicalBytes,
                     t.allocatedBytes));
    WriteLine(Format(L"  Tables: %zu files, %zu extra links, %zu folders, %zu name chars = %s",
                     index.files.size(), index.extraLinks.size(), index.dirs.size(), index.names.size(),
                     FormatBytes(index.MemoryBytes()).c_str()));
    const auto& x = index.excluded;
    WriteLine(Format(L"  Excluded: unlisted root files %llu (%llu B, on disk %llu B), files under hidden folders "
                     L"%llu (%llu B, on disk %llu B), hidden folders %llu",
                     x.hiddenRootFiles.files, x.hiddenRootFiles.logicalBytes, x.hiddenRootFiles.allocatedBytes,
                     x.hidden.files, x.hidden.logicalBytes, x.hidden.allocatedBytes, x.hiddenFolders));
    WriteLine(Format(L"  Excluded: named streams %llu (%llu B), surrogate reparse dirs %llu, unreachable folders %llu, "
                     L"unreachable files %llu",
                     x.namedStreams, x.namedStreamLogicalBytes, x.surrogateReparseDirs, x.unreachableFolders,
                     x.unreachableFiles));
    std::wstring tags;
    for (const auto& rt : index.followedReparseTags) tags += Format(L" 0x%08lx=%llu", rt.tag, rt.folders);
    WriteLine(Format(L"  Followed reparse folders by tag:%s", tags.empty() ? L" none" : tags.c_str()));
    const auto& i = index.issues;
    WriteLine(Format(L"  Issues: access denied %llu, other errors %llu, malformed entries %llu, duplicate IDs %llu, "
                     L"reparse tag failures %llu",
                     i.accessDenied, i.otherErrors, i.malformedEntries, i.duplicateIds, i.reparseTagFailures));
}

// Totals of the files directly in `dir` (subtree minus child subtrees).
Totals DirectTotals(const VolumeIndex& index, DirIndex dir) {
    Totals t = index.dirs[dir].subtree;
    for (DirIndex c = index.dirs[dir].firstChild; c != kNoDir; c = index.dirs[c].nextSibling) {
        const Totals& s = index.dirs[c].subtree;
        t.logicalBytes -= s.logicalBytes;
        t.allocatedBytes -= s.allocatedBytes;
        t.files -= s.files;
        t.folders -= s.folders + 1;
    }
    return t;
}

std::int64_t Diff(std::uint64_t a, std::uint64_t b) {
    return static_cast<std::int64_t>(a) - static_cast<std::int64_t>(b);
}

// Full path of a folder in the index; rootPath ends with a backslash.
std::wstring PathOf(const VolumeIndex& index, DirIndex dir, const std::wstring& rootPath) {
    std::vector<DirIndex> chain;
    for (DirIndex d = dir; d != kNoDir && d != index.root; d = index.dirs[d].parent) chain.push_back(d);
    std::wstring path = rootPath;
    for (size_t i = chain.size(); i-- > 0;) {
        path += index.Name(chain[i]);
        if (i != 0) path += L'\\';
    }
    return path;
}

DirIndex FindDirById(const VolumeIndex& index, std::uint64_t id) {
    if (index.sortedById) {
        auto it = std::lower_bound(index.dirs.begin(), index.dirs.end(), id,
                                   [](const dirsize::DirEntry& d, std::uint64_t value) { return d.id < value; });
        return (it != index.dirs.end() && it->id == id) ? static_cast<DirIndex>(it - index.dirs.begin()) : kNoDir;
    }
    for (DirIndex i = 0; i < index.dirs.size(); ++i) {
        if (index.dirs[i].id == id) return i;
    }
    return kNoDir;
}

// Size of a file read from an open handle (FileStandardInfo), the file's
// own current size, to tell which of the two engines is right.
std::wstring CurrentSize(const std::wstring& folder, std::wstring_view name) {
    std::wstring path = folder;
    if (path.back() != L'\\') path += L'\\';
    path += name;
    HANDLE h = CreateFileW(dirsize::ToExtendedPath(path).c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Format(L"(cannot open: error %lu)", GetLastError());
    FILE_STANDARD_INFO info{};
    const bool ok = GetFileInformationByHandleEx(h, FileStandardInfo, &info, sizeof(info)) != 0;
    const DWORD error = GetLastError();
    CloseHandle(h);
    if (!ok) return Format(L"(FileStandardInfo failed: error %lu)", error);
    return Format(L"%lld B (on disk %lld B)", info.EndOfFile.QuadPart, info.AllocationSize.QuadPart);
}

// Lists `path` and compares each file in it, by file ID, with the layout
// index's files in `dir`. Prints at most `maxLines` differing files.
void CompareFilesIn(const std::wstring& path, const VolumeIndex& layout, DirIndex dir, const std::wstring& indent,
                    size_t maxLines, bool summary) {
    struct Sizes {
        std::uint64_t logical, allocated;
    };
    std::unordered_map<std::uint64_t, Sizes> inLayout;
    for (const auto& f : layout.files) {
        if (f.dir == dir) inLayout[f.id] = {f.logicalBytes, f.allocatedBytes};
    }
    for (const auto& x : layout.extraLinks) {
        const auto& f = layout.files[x.file];
        if (x.dir == dir) inLayout[f.id] = {f.logicalBytes, f.allocatedBytes};
    }
    HANDLE h = dirsize::OpenDirectoryForListing(dirsize::ToExtendedPath(path).c_str());
    if (h == INVALID_HANDLE_VALUE) {
        WriteLine(Format(L"%scannot list folder: error %lu", indent.c_str(), GetLastError()));
        return;
    }
    dirsize::DirectoryLister lister;
    std::uint64_t listed = 0, same = 0, differing = 0;
    auto line = [&](const std::wstring& text) {
        if (++differing <= maxLines) WriteLine(indent + text);
    };
    lister.List(h, [&](const dirsize::ListedItem& item) {
        if (item.attributes & FILE_ATTRIBUTE_DIRECTORY) return;
        ++listed;
        auto it = inLayout.find(item.id);
        if (it == inLayout.end()) {
            line(Format(L"%s: listed (%llu B, on disk %llu B) but not in layout", std::wstring(item.name).c_str(),
                        item.logicalBytes, item.allocatedBytes));
            return;
        }
        if (it->second.logical != item.logicalBytes || it->second.allocated != item.allocatedBytes) {
            line(Format(L"%s: listing %llu B (on disk %llu B), layout %llu B (on disk %llu B), now %s, attributes 0x%lx",
                        std::wstring(item.name).c_str(), item.logicalBytes, item.allocatedBytes, it->second.logical,
                        it->second.allocated, CurrentSize(path, item.name).c_str(),
                        static_cast<unsigned long>(item.attributes)));
        } else {
            ++same;
        }
        inLayout.erase(it);
    });
    CloseHandle(h);
    for (const auto& [id, sizes] : inLayout) {
        line(Format(L"id 0x%016llx: in layout (%llu B, on disk %llu B) but not listed", id, sizes.logical,
                    sizes.allocated));
    }
    if (differing > maxLines) {
        WriteLine(Format(L"%s... %llu more differing files", indent.c_str(), differing - maxLines));
    }
    if (summary) WriteLine(Format(L"%sFiles listed %llu, same size %llu", indent.c_str(), listed, same));
}

// Path of a file or folder by ID (OpenFileById, GetFinalPathNameByHandleW).
std::wstring PathById(HANDLE hint, std::uint64_t id) {
    FILE_ID_DESCRIPTOR descriptor{};
    descriptor.dwSize = sizeof(descriptor);
    descriptor.Type = FileIdType;
    descriptor.FileId.QuadPart = static_cast<LONGLONG>(id);
    HANDLE h = OpenFileById(hint, &descriptor, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
    if (h == INVALID_HANDLE_VALUE) return Format(L"(cannot open by ID: error %lu)", GetLastError());
    std::wstring path(1024, L'\0');
    const DWORD n = GetFinalPathNameByHandleW(h, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED);
    const DWORD error = GetLastError();
    CloseHandle(h);
    if (n == 0 || n >= path.size()) return Format(L"(no path: error %lu)", error);
    path.resize(n);
    return path;
}

// Attributes and reparse tag of a path, opened without following reparse points.
std::wstring DescribePath(const std::wstring& path) {
    HANDLE h = CreateFileW(dirsize::ToExtendedPath(path).c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Format(L"cannot open: error %lu", GetLastError());
    FILE_ATTRIBUTE_TAG_INFO info{};
    const bool ok = GetFileInformationByHandleEx(h, FileAttributeTagInfo, &info, sizeof(info)) != 0;
    const DWORD error = GetLastError();
    CloseHandle(h);
    if (!ok) return Format(L"FileAttributeTagInfo failed: error %lu", error);
    return Format(L"attributes 0x%08lx, tag 0x%08lx", info.FileAttributes, info.ReparseTag);
}

void PrintDiagnostics(const wchar_t* engine, const VolumeIndex& index, const std::wstring& rootPath, HANDLE rootHandle) {
    WriteLine(Format(L"  %s: followed reparse folders (sample of %zu)", engine, index.followedReparseDirIds.size()));
    for (std::uint64_t id : index.followedReparseDirIds) {
        const DirIndex d = FindDirById(index, id);
        const std::wstring path = d == kNoDir ? PathById(rootHandle, id) : PathOf(index, d, rootPath);
        WriteLine(Format(L"    %s: %s", path.c_str(), DescribePath(path).c_str()));
    }
    if (index.unreachableFileSamples.empty()) return;
    WriteLine(Format(L"  %s: unreachable files (sample of %zu)", engine, index.unreachableFileSamples.size()));
    for (const auto& u : index.unreachableFileSamples) {
        WriteLine(Format(L"    id 0x%016llx: %s", u.id, PathById(rootHandle, u.id).c_str()));
        const std::wstring parent = u.parentId == 0 ? std::wstring(L"(no link)") : PathById(rootHandle, u.parentId);
        WriteLine(Format(L"      parent id 0x%016llx: %s", u.parentId, parent.c_str()));
    }
}

struct Comparer {
    const VolumeIndex& layout;
    const VolumeIndex& walk;
    const std::wstring& rootPath;
    std::uint64_t differingFolders = 0;
    std::uint64_t onlyInLayout = 0;
    std::uint64_t onlyInWalk = 0;
    bool listFiles = true;  // name differing files from a new directory listing
    static constexpr int kMaxDepth = 14;
    static constexpr std::int64_t kBytesShown = 1 << 20;

    bool Differs(const Totals& a, const Totals& b) const {
        return a.files != b.files || a.folders != b.folders || a.logicalBytes != b.logicalBytes ||
               a.allocatedBytes != b.allocatedBytes;
    }

    // Compares the children of two matching folders; drills into the worst.
    void Compare(DirIndex l, DirIndex w, int level) {
        std::unordered_map<std::wstring_view, DirIndex> walkChildren;
        for (DirIndex c = walk.dirs[w].firstChild; c != kNoDir; c = walk.dirs[c].nextSibling) {
            walkChildren.emplace(walk.Name(c), c);
        }
        struct Pair {
            DirIndex l, w;
            std::int64_t weight;
        };
        std::vector<Pair> differing;
        const std::wstring indent(static_cast<size_t>(level) * 2 + 2, L' ');
        for (DirIndex c = layout.dirs[l].firstChild; c != kNoDir; c = layout.dirs[c].nextSibling) {
            auto it = walkChildren.find(layout.Name(c));
            const Totals& lt = layout.dirs[c].subtree;
            if (it == walkChildren.end()) {
                ++onlyInLayout;
                WriteLine(Format(L"%s[%s] only in layout: files %llu, %llu B", indent.c_str(),
                                 std::wstring(layout.Name(c)).c_str(), lt.files, lt.logicalBytes));
                continue;
            }
            const DirIndex wc = it->second;
            walkChildren.erase(it);
            const Totals& wt = walk.dirs[wc].subtree;
            if (!Differs(lt, wt)) continue;
            ++differingFolders;
            const std::int64_t db = Diff(lt.logicalBytes, wt.logicalBytes);
            const std::int64_t df = Diff(lt.files, wt.files);
            const std::int64_t dd = Diff(lt.folders, wt.folders);
            const std::int64_t da = Diff(lt.allocatedBytes, wt.allocatedBytes);
            if (df != 0 || dd != 0 || std::llabs(db) > kBytesShown || std::llabs(da) > kBytesShown || level == 0) {
                WriteLine(Format(L"%s[%s] layout-walk: files %+lld, folders %+lld, bytes %+lld, on disk %+lld",
                                 indent.c_str(), std::wstring(layout.Name(c)).c_str(), df, dd, db, da));
            }
            differing.push_back(
                {c, wc, std::max(std::llabs(db), std::llabs(da)) + (std::llabs(df) + std::llabs(dd)) * 4096});
        }
        for (const auto& [name, wc] : walkChildren) {
            ++onlyInWalk;
            const Totals& wt = walk.dirs[wc].subtree;
            WriteLine(Format(L"%s[%s] only in walk: files %llu, %llu B", indent.c_str(), std::wstring(name).c_str(),
                             wt.files, wt.logicalBytes));
        }
        const Totals ld = DirectTotals(layout, l);
        const Totals wd = DirectTotals(walk, w);
        if (ld.files != wd.files || ld.logicalBytes != wd.logicalBytes || ld.allocatedBytes != wd.allocatedBytes) {
            WriteLine(Format(L"%s(files directly here) layout-walk: files %+lld, bytes %+lld, on disk %+lld",
                             indent.c_str(), Diff(ld.files, wd.files), Diff(ld.logicalBytes, wd.logicalBytes),
                             Diff(ld.allocatedBytes, wd.allocatedBytes)));
            // Names the differing files; the folder is listed again now, so
            // files that changed since the walk may also show up.
            if (listFiles && l != layout.root) CompareFilesIn(PathOf(layout, l, rootPath), layout, l, indent + L"  ", 5, false);
        }
        if (level >= kMaxDepth) return;
        std::sort(differing.begin(), differing.end(), [](const Pair& a, const Pair& b) { return a.weight > b.weight; });
        for (size_t i = 0; i < differing.size() && i < 3; ++i) {
            WriteLine(Format(L"%s> %s", indent.c_str(), std::wstring(layout.Name(differing[i].l)).c_str()));
            Compare(differing[i].l, differing[i].w, level + 1);
        }
    }
};

int RunVerify(const wchar_t* driveArg) {
    if (!iswalpha(driveArg[0]) || (driveArg[1] != L'\0' && driveArg[1] != L':')) {
        PrintUsage();
        return 2;
    }
    const wchar_t drive = static_cast<wchar_t>(towupper(driveArg[0]));
    const std::wstring rootPath = std::wstring(1, drive) + L":\\";
    WriteLine(Format(L"Volume            %c:", drive));
    WriteLine(Format(L"Elevated          %s", IsElevated() ? L"yes" : L"no"));
    WriteLine(Format(L"Backup privilege  %s", EnableBackupPrivilege() ? L"enabled" : L"not available"));

    // NTFS engine first, so its peak memory is not mixed with the walker's.
    const auto t0 = std::chrono::steady_clock::now();
    dirsize::NtfsIndexResult ntfs = dirsize::BuildNtfsIndex(drive);
    const double ntfsSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const std::uint64_t peakAfterNtfs = PeakWorkingSetBytes();
    const std::uint64_t residentAfterNtfs = WorkingSetBytes();
    WriteLine();
    WriteLine(L"== NTFS engine (FSCTL_QUERY_FILE_LAYOUT)");
    if (!ntfs.ok) {
        WriteLine(Format(L"  failed: %s (Win32 error %lu)", ntfs.failedStep, ntfs.error));
        return 1;
    }
    const auto& p = ntfs.parse;
    WriteLine(Format(L"  Time %.3f s (enumerate %.3f s, build %.3f s), %llu calls, %llu entries", ntfsSeconds,
                     ntfs.enumerateSeconds, ntfs.finishSeconds, ntfs.layoutCalls, p.entries));
    WriteLine(Format(L"  Parser: malformed %llu, unknown file versions %llu, unknown stream versions %llu, "
                     L"truncated chains %llu, bad headers %llu, DOS-only names %llu",
                     p.malformedEntries, p.unknownFileVersions, p.unknownStreamVersions, p.truncatedChains,
                     p.badHeaders, p.dosOnlyNames));
    WriteLine(Format(L"  Enumeration ended: %s", ntfs.endError == ERROR_HANDLE_EOF ? L"ERROR_HANDLE_EOF"
                                                     : L"empty buffer (no ERROR_HANDLE_EOF)"));
    WriteLine(Format(L"  Root listing: %llu items, matched in layout %llu (should be equal)", ntfs.rootListed,
                     ntfs.index.rootListingMatched));
    const auto& usn = ntfs.index.usn;
    if (usn.active) {
        WriteLine(Format(L"  USN journal 0x%016llx, next USN %lld (recorded before the scan)", usn.journalId,
                         usn.nextUsn));
    } else {
        WriteLine(Format(L"  USN journal not available (error %lu)", usn.error));
    }
    WriteLine(Format(L"  Memory: index tables %s; process peak working set %s, working set after build %s "
                     L"(includes freed heap pages)",
                     FormatBytes(ntfs.index.MemoryBytes()).c_str(), FormatBytes(peakAfterNtfs).c_str(),
                     FormatBytes(residentAfterNtfs).c_str()));
    PrintIndexSummary(ntfs.index);

    const auto t1 = std::chrono::steady_clock::now();
    dirsize::WalkIndexResult walk = dirsize::BuildIndexByWalking(dirsize::ToExtendedPath(rootPath));
    const double walkSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    WriteLine();
    WriteLine(L"== Walker engine (directory listing)");
    if (!walk.rootOpened) {
        WriteLine(Format(L"  failed to open %s (Win32 error %lu)", rootPath.c_str(), walk.rootError));
        return 1;
    }
    WriteLine(Format(L"  Time %.3f s", walkSeconds));
    PrintIndexSummary(walk.index);

    WriteLine();
    WriteLine(L"== Comparison (layout minus walk; folders listed only when they differ)");
    const Totals& lt = ntfs.index.dirs[ntfs.index.root].subtree;
    const Totals& wt = walk.index.dirs[walk.index.root].subtree;
    WriteLine(Format(L"  Whole volume: files %+lld, folders %+lld, bytes %+lld, on disk %+lld", Diff(lt.files, wt.files),
                     Diff(lt.folders, wt.folders), Diff(lt.logicalBytes, wt.logicalBytes),
                     Diff(lt.allocatedBytes, wt.allocatedBytes)));
    Comparer comparer{ntfs.index, walk.index, rootPath};
    comparer.Compare(ntfs.index.root, walk.index.root, 0);
    WriteLine(Format(L"  Differing folders %llu, only in layout %llu, only in walk %llu", comparer.differingFolders,
                     comparer.onlyInLayout, comparer.onlyInWalk));
    WriteLine();
    WriteLine(L"== Files directly in the root");
    CompareFilesIn(rootPath, ntfs.index, ntfs.index.root, L"  ", SIZE_MAX, true);
    WriteLine();
    WriteLine(L"== Diagnostics");
    {
        HANDLE rootHandle = dirsize::OpenDirectoryForListing(rootPath.c_str());
        PrintDiagnostics(L"NTFS engine", ntfs.index, rootPath, rootHandle);
        PrintDiagnostics(L"Walker engine", walk.index, rootPath, rootHandle);
        if (rootHandle != INVALID_HANDLE_VALUE) CloseHandle(rootHandle);
    }
    WriteLine();
    WriteLine(Format(L"Result: %s", comparer.Differs(lt, wt) ? L"DIFFERENT (see above)"
                                                    : L"IDENTICAL totals"));
    return 0;
}

void PrintCatchUp(const wchar_t* what, const dirsize::CatchUpResult& r) {
    WriteLine(Format(L"  %s: USN %lld -> %lld, %llu records, %llu changed IDs, %llu open files, %llu ranges, "
                     L"%llu entries, %llu calls; read %.3f s, query %.3f s, apply %.3f s",
                     what, r.fromUsn, r.toUsn, r.records, r.changedIds, r.openTracked, r.ranges, r.entries,
                     r.layoutCalls,
                     r.readSeconds, r.querySeconds, r.applySeconds));
    const auto& u = r.update;
    if (r.changedIds != 0) {
        WriteLine(Format(L"    files changed %llu, folders created %llu, moved %llu, removed %llu", u.filesChanged,
                         u.foldersCreated, u.foldersMoved, u.foldersRemoved));
    }
    if (r.vanished != 0) {
        WriteLine(Format(L"    not returned by the re-query without a delete record: %llu IDs", r.vanished));
        for (const auto& c : r.vanishedSamples) {
            WriteLine(Format(L"      id 0x%016llx, reasons 0x%08lx", c.id, static_cast<unsigned long>(c.reasons)));
        }
    }
    if (r.needsRescan) WriteLine(Format(L"    NEEDS RESCAN: %s (Win32 error %lu)", r.reason, r.error));
}

// Compares every file by ID between the updated and the fresh index and says
// whether the journal ever mentioned it. A differing file the journal never
// mentioned changed without a record we read (for example a file opened
// before the build and still being written).
void PrintDifferingFiles(const dirsize::NtfsLiveVolume& volume, const VolumeIndex& updated, const VolumeIndex& fresh,
                         const std::wstring& rootPath) {
    struct Sizes {
        std::uint64_t logical = 0, allocated = 0;
    };
    std::unordered_map<std::uint64_t, Sizes> inFresh;
    inFresh.reserve(fresh.files.size());
    for (const auto& f : fresh.files) inFresh[f.id] = {f.logicalBytes, f.allocatedBytes};
    struct Item {
        std::uint64_t id;
        Sizes updated, fresh;
        bool inUpdated, inFresh;
        std::uint64_t weight;  // |logical difference|
    };
    std::vector<Item> items;
    for (const auto& f : updated.files) {
        auto it = inFresh.find(f.id);
        if (it == inFresh.end()) {
            items.push_back({f.id, {f.logicalBytes, f.allocatedBytes}, {}, true, false, f.logicalBytes});
            continue;
        }
        if (it->second.logical != f.logicalBytes || it->second.allocated != f.allocatedBytes) {
            items.push_back({f.id, {f.logicalBytes, f.allocatedBytes}, it->second, true, true,
                             static_cast<std::uint64_t>(std::llabs(Diff(f.logicalBytes, it->second.logical)))});
        }
        inFresh.erase(it);
    }
    for (const auto& [id, sizes] : inFresh) items.push_back({id, {}, sizes, false, true, sizes.logical});

    auto group = [&](const Item& i) { return !volume.SeenInJournal(i.id) ? 0 : volume.TrackedOpen(i.id) ? 2 : 1; };
    const wchar_t* names[3] = {L"never in journal", L"closed since", L"tracked open"};
    std::uint64_t count[3] = {}, bytes[3] = {};
    for (const Item& i : items) {
        ++count[group(i)];
        bytes[group(i)] += i.weight;
    }
    WriteLine();
    WriteLine(L"== Files that differ (updated index vs fresh build, by file ID)");
    WriteLine(L"  Journal state as read by this process; after a cache load, files restored as open count as seen.");
    WriteLine(Format(L"  Differing files %zu", items.size()));
    for (int g = 0; g < 3; ++g) {
        WriteLine(Format(L"    %-17s %llu files, sum of |byte differences| %llu", names[g], count[g], bytes[g]));
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.weight > b.weight; });
    HANDLE hint = dirsize::OpenDirectoryForListing(rootPath.c_str());
    constexpr size_t kShown = 60;
    for (size_t n = 0; n < items.size() && n < kShown; ++n) {
        const Item& i = items[n];
        const std::wstring u = i.inUpdated ? Format(L"%llu B (on disk %llu B)", i.updated.logical, i.updated.allocated)
                                           : std::wstring(L"(missing)");
        const std::wstring f = i.inFresh ? Format(L"%llu B (on disk %llu B)", i.fresh.logical, i.fresh.allocated)
                                         : std::wstring(L"(missing)");
        WriteLine(Format(L"  id 0x%016llx: updated %s, fresh %s; %s", i.id, u.c_str(), f.c_str(), names[group(i)]));
        WriteLine(Format(L"    %s", hint == INVALID_HANDLE_VALUE ? L"(no path)" : PathById(hint, i.id).c_str()));
    }
    if (items.size() > kShown) WriteLine(Format(L"  ... %zu more", items.size() - kShown));
    if (hint != INVALID_HANDLE_VALUE) CloseHandle(hint);

    // Files the fresh build has and the updated index lost: what does a
    // re-query by ID return now, and does the root listing show them?
    WriteLine();
    WriteLine(L"== Re-query of files missing from the updated index (largest first, at most 10)");
    size_t probed = 0;
    for (const Item& i : items) {
        if (i.inUpdated || !i.inFresh || probed == 10) continue;
        ++probed;
        const dirsize::IdProbe p = volume.ProbeId(i.id);
        WriteLine(Format(L"  id 0x%016llx (fresh %llu B, %s): query error %lu, %llu entries, first id 0x%016llx, "
                         L"%s; listed in root %s (list error %lu)",
                         i.id, i.fresh.logical, names[group(i)], p.error, p.entries, p.returnedId,
                         p.found ? L"found" : L"NOT FOUND", p.listedInRoot ? L"yes" : L"no", p.listError));
        WriteLine(Format(L"    journal reasons read by this process: 0x%08lx",
                         static_cast<unsigned long>(volume.JournalReasons(i.id))));
        if (p.found) {
            WriteLine(Format(L"    attributes 0x%08lx, data %s, %llu B", static_cast<unsigned long>(p.attributes),
                             p.hasData ? L"yes" : L"no", p.logicalBytes));
            for (const auto& [parent, name] : p.links) {
                WriteLine(Format(L"    link: parent 0x%016llx, name %s", parent, name.c_str()));
            }
        }
    }
    if (probed == 0) WriteLine(L"  none");
}

// Builds the volume again from scratch and compares it with the maintained
// index, folder by folder and file by file.
int CompareWithFresh(dirsize::NtfsLiveVolume& volume, wchar_t drive, const std::wstring& rootPath) {
    const Usage before = TakeUsage();
    WriteLine();
    WriteLine(L"== Compare with a fresh full build");
    dirsize::NtfsIndexResult fresh = dirsize::BuildNtfsIndex(drive);
    if (!fresh.ok) {
        WriteLine(Format(L"  failed: %s (Win32 error %lu)", fresh.failedStep, fresh.error));
        return 1;
    }
    PrintResources(L"fresh full build, a second index in memory", before, TakeUsage());
    const dirsize::CatchUpResult r = volume.CatchUp();  // include what changed during the fresh build
    PrintCatchUp(L"last catch-up", r);
    if (r.needsRescan) return 1;
    const VolumeIndex live = volume.Live().Compacted();
    std::uint64_t mismatched = 0;
    for (const auto& d : live.dirs) {
        const DirIndex i = volume.Live().FindDir(d.id);
        if (!volume.Live().DirLive(i) || !(volume.Live().Index().dirs[i].subtree == d.subtree)) ++mismatched;
    }
    WriteLine(Format(L"  Maintained totals vs recomputed: %llu folders differ (should be 0)", mismatched));
    WriteLine(L"  In the lines below, \"layout\" is the updated index and \"walk\" the fresh full build.");
    const Totals& lt = live.dirs[live.root].subtree;
    const Totals& ft = fresh.index.dirs[fresh.index.root].subtree;
    WriteLine(Format(L"  Whole volume: files %+lld, folders %+lld, bytes %+lld, on disk %+lld", Diff(lt.files, ft.files),
                     Diff(lt.folders, ft.folders), Diff(lt.logicalBytes, ft.logicalBytes),
                     Diff(lt.allocatedBytes, ft.allocatedBytes)));
    Comparer comparer{live, fresh.index, rootPath};
    comparer.listFiles = false;  // files are compared by ID with the fresh build below
    comparer.Compare(live.root, fresh.index.root, 0);
    WriteLine(Format(L"  Differing folders %llu, only in updated %llu, only in fresh %llu", comparer.differingFolders,
                     comparer.onlyInLayout, comparer.onlyInWalk));
    PrintDifferingFiles(volume, live, fresh.index, rootPath);
    WriteLine();
    WriteLine(Format(L"Result: %s", comparer.Differs(lt, ft) ? L"DIFFERENT (see above)" : L"IDENTICAL totals"));
    return 0;
}

int RunWatch(const wchar_t* driveArg, unsigned seconds) {
    if (!iswalpha(driveArg[0]) || (driveArg[1] != L'\0' && driveArg[1] != L':')) {
        PrintUsage();
        return 2;
    }
    const wchar_t drive = static_cast<wchar_t>(towupper(driveArg[0]));
    const std::wstring rootPath = std::wstring(1, drive) + L":\\";
    WriteLine(Format(L"Volume            %c:", drive));
    WriteLine(Format(L"Elevated          %s", IsElevated() ? L"yes" : L"no"));
    WriteLine(Format(L"Backup privilege  %s", EnableBackupPrivilege() ? L"enabled" : L"not available"));
    WriteLine(Format(L"Watch time        %u s", seconds));

    WriteLine();
    WriteLine(L"== Build");
    const Usage u0 = TakeUsage();
    dirsize::NtfsIndexResult build;
    std::unique_ptr<dirsize::NtfsLiveVolume> volume = dirsize::NtfsLiveVolume::Build(drive, build);
    if (!volume) {
        WriteLine(Format(L"  failed: %s (Win32 error %lu)", build.failedStep, build.error));
        return 1;
    }
    WriteLine(Format(L"  enumerate %.3f s, build %.3f s; journal 0x%016llx, start USN %lld", build.enumerateSeconds,
                     build.finishSeconds, volume->JournalId(), volume->AppliedUsn()));
    dirsize::CatchUpResult r = volume->CatchUp();
    PrintCatchUp(L"replay of changes during the scan", r);
    if (r.needsRescan) return 1;
    const Usage u1 = TakeUsage();
    PrintResources(L"build and replay", u0, u1);
    WriteLine(Format(L"  Index tables %s", FormatBytes(volume->Live().Index().MemoryBytes()).c_str()));

    WriteLine();
    WriteLine(L"== Watch (catch-up every 5 s; lines only when something changed)");
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t rounds = 0, totalIds = 0, maxOpen = 0;
    double worstSeconds = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
        Sleep(5000);
        r = volume->CatchUp();
        ++rounds;
        totalIds += r.changedIds;
        worstSeconds = std::max(worstSeconds, r.readSeconds + r.querySeconds + r.applySeconds);
        if (r.records != 0 || r.needsRescan) PrintCatchUp(L"catch-up", r);
        maxOpen = std::max<std::uint64_t>(maxOpen, r.openTracked);
        if (r.needsRescan) return 1;
    }
    WriteLine(Format(L"  %llu catch-ups, %llu changed IDs in total, at most %llu open files tracked, slowest %.3f s",
                     rounds, totalIds, maxOpen, worstSeconds));
    const Usage u2 = TakeUsage();
    PrintResources(L"watching, mostly asleep between catch-ups", u1, u2);

    return CompareWithFresh(*volume, drive, rootPath);
}

void PrintSave(const dirsize::CacheSaveResult& s) {
    if (!s.ok) {
        WriteLine(Format(L"  save failed: %s (Win32 error %lu)", s.reason, s.error));
        return;
    }
    WriteLine(Format(L"  saved %s (%s); compact and serialize %.3f s, write and flush %.3f s", s.path.c_str(),
                     FormatBytes(s.bytes).c_str(), s.compactSeconds, s.writeSeconds));
}

// "cache save": full build, catch up, save. "cache load": load the cache,
// catch up with the journal (the changes made since the save), compare with
// a fresh full build, then save again.
int RunCache(const wchar_t* action, const wchar_t* driveArg) {
    const std::wstring_view what(action);
    if ((what != L"save" && what != L"load") || !iswalpha(driveArg[0]) ||
        (driveArg[1] != L'\0' && driveArg[1] != L':')) {
        PrintUsage();
        return 2;
    }
    const wchar_t drive = static_cast<wchar_t>(towupper(driveArg[0]));
    const std::wstring rootPath = std::wstring(1, drive) + L":\\";
    WriteLine(Format(L"Volume            %c:", drive));
    WriteLine(Format(L"Elevated          %s", IsElevated() ? L"yes" : L"no"));
    WriteLine(Format(L"Backup privilege  %s", EnableBackupPrivilege() ? L"enabled" : L"not available"));
    WriteLine(Format(L"Action            cache %s", action));

    if (what == L"save") {
        WriteLine();
        WriteLine(L"== Build");
        const Usage u0 = TakeUsage();
        dirsize::NtfsIndexResult build;
        std::unique_ptr<dirsize::NtfsLiveVolume> volume = dirsize::NtfsLiveVolume::Build(drive, build);
        if (!volume) {
            WriteLine(Format(L"  failed: %s (Win32 error %lu)", build.failedStep, build.error));
            return 1;
        }
        const dirsize::CatchUpResult r = volume->CatchUp();
        PrintCatchUp(L"replay of changes during the scan", r);
        if (r.needsRescan) return 1;
        const Usage u1 = TakeUsage();
        PrintResources(L"build and replay", u0, u1);
        WriteLine();
        WriteLine(L"== Save");
        const dirsize::CacheSaveResult saved = volume->SaveCache();
        PrintSave(saved);
        PrintResources(L"save", u1, TakeUsage());
        WriteLine(Format(L"  applied USN %lld, journal 0x%016llx", volume->AppliedUsn(), volume->JournalId()));
        return saved.ok ? 0 : 1;
    }

    WriteLine();
    WriteLine(L"== Load");
    const Usage u0 = TakeUsage();
    dirsize::CacheOpenResult opened;
    std::unique_ptr<dirsize::NtfsLiveVolume> volume = dirsize::NtfsLiveVolume::FromCache(drive, opened);
    const Usage u1 = TakeUsage();
    if (!volume) {
        WriteLine(Format(L"  no usable cache%s: %s (Win32 error %lu)", opened.missing ? L" (missing)" : L"",
                         opened.reason, opened.error));
        if (!opened.path.empty()) WriteLine(Format(L"  path %s", opened.path.c_str()));
        return 1;
    }
    WriteLine(Format(L"  loaded %s (%s); read %.3f s, validate and rebuild totals %.3f s", opened.path.c_str(),
                     FormatBytes(opened.bytes).c_str(), opened.readSeconds, opened.parseSeconds));
    WriteLine(Format(L"  cached at USN %lld, journal 0x%016llx", volume->AppliedUsn(), volume->JournalId()));
    PrintResources(L"load", u0, u1);
    WriteLine(Format(L"  Index tables %s", FormatBytes(volume->Live().Index().MemoryBytes()).c_str()));
    const dirsize::CatchUpResult r = volume->CatchUp();
    PrintCatchUp(L"catch-up since the save", r);
    PrintResources(L"catch-up since the save", u1, TakeUsage());
    if (r.needsRescan) return 1;
    const int compared = CompareWithFresh(*volume, drive, rootPath);
    WriteLine();
    WriteLine(L"== Save again");
    PrintSave(volume->SaveCache());
    return compared;
}

// ---- service client ----------------------------------------------------------------

const wchar_t* ServiceErrorName(dirsize::proto::ErrorCode c) {
    using dirsize::proto::ErrorCode;
    switch (c) {
        case ErrorCode::BadRequest: return L"bad request";
        case ErrorCode::UnsupportedVersion: return L"unsupported protocol version";
        case ErrorCode::PathRejected: return L"path rejected (only canonical local drive-letter paths: no UNC, device paths, . or ..)";
        case ErrorCode::NotFound: return L"not found";
        case ErrorCode::AccessDenied: return L"access denied (you may not list this folder)";
        case ErrorCode::NotIndexed: return L"volume not indexed";
        case ErrorCode::Busy: return L"busy";
        case ErrorCode::Internal: return L"internal error";
    }
    return L"?";
}

bool ConnectToService(dirsize::client::PipeClient& client, bool console) {
    dirsize::client::ClientError e;
    if (!client.Connect(!console, e)) {
        WriteLine(Format(L"cannot use the service: %s (Win32 error %lu)", e.what.c_str(), e.win32));
        return false;
    }
    if (console) {
        WriteLine(Format(L"warning: --console skips the check that the pipe belongs to the DirSize service "
                         L"(server process %lu)", client.ServerProcessId()));
    }
    return true;
}

void ReportFailure(const dirsize::client::ClientError& e, const dirsize::proto::ErrorResponse& serverError) {
    if (e.win32 == ERROR_SUCCESS) {
        WriteLine(Format(L"service: %s", ServiceErrorName(serverError.code)));
    } else {
        WriteLine(Format(L"%s (Win32 error %lu)", e.what.c_str(), e.win32));
    }
}

int RunServiceStatus(bool console) {
    dirsize::client::PipeClient client;
    if (!ConnectToService(client, console)) return 1;
    dirsize::proto::StatusResponse status;
    dirsize::proto::ErrorResponse serverError;
    dirsize::client::ClientError e;
    if (!client.Status(status, serverError, e)) {
        ReportFailure(e, serverError);
        return 1;
    }
    for (const auto& v : status.volumes) {
        const wchar_t* state = v.state == dirsize::proto::VolumeState::Ready      ? L"ready"
                               : v.state == dirsize::proto::VolumeState::Updating ? L"updating"
                                                                                  : L"no index yet";
        std::wstring age = v.secondsSinceCatchUp == UINT64_MAX ? std::wstring(L"never")
                                                               : std::to_wstring(v.secondsSinceCatchUp) + L" s ago";
        WriteLine(Format(L"%c:  %-12s index %s, last catch-up %s", v.drive, state, FormatBytes(v.memoryBytes).c_str(),
                         age.c_str()));
    }
    if (status.volumes.empty()) WriteLine(L"no volumes indexed");
    return 0;
}

int RunServiceQuery(const std::wstring& path, std::uint32_t start, std::uint32_t count, bool console) {
    dirsize::client::PipeClient client;
    if (!ConnectToService(client, console)) return 1;
    const auto t0 = std::chrono::steady_clock::now();
    dirsize::proto::FolderResponse folder;
    dirsize::proto::ErrorResponse serverError;
    dirsize::client::ClientError e;
    if (!client.QueryFolder({path, start, count}, folder, serverError, e)) {
        ReportFailure(e, serverError);
        return 1;
    }
    const double ms = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1000;
    const auto& t = folder.totals;
    WriteLine(Format(L"%s%s", path.c_str(), folder.updating ? L"  (updating: a rescan is running)" : L""));
    WriteLine(Format(L"  size %s (%llu bytes), on disk %s, files %llu, folders %llu", FormatBytes(t.logicalBytes).c_str(),
                     t.logicalBytes, FormatBytes(t.allocatedBytes).c_str(), t.files, t.folders));
    WriteLine(Format(L"  child folders %u, showing %u from %u (largest on disk first); answered in %.1f ms",
                     folder.childCount, static_cast<unsigned>(folder.children.size()), folder.pageStart, ms));
    for (const auto& c : folder.children) {
        WriteLine(Format(L"  %12s  %12s  %s%s", FormatBytes(c.totals.allocatedBytes).c_str(),
                         FormatBytes(c.totals.logicalBytes).c_str(), c.name.c_str(),
                         c.accessible ? L"" : L"  (no access)"));
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    if (argc == 3 && std::wstring_view(argv[1]) == L"verify") {
        return RunVerify(argv[2]);
    }
    if ((argc == 3 || argc == 4) && std::wstring_view(argv[1]) == L"watch") {
        const unsigned seconds = argc == 4 ? static_cast<unsigned>(std::wcstoul(argv[3], nullptr, 10)) : 300u;
        return RunWatch(argv[2], seconds);
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"status") {
        const bool console = argc == 3 && std::wstring_view(argv[2]) == L"--console";
        if (argc > 3 || (argc == 3 && !console)) {
            PrintUsage();
            return 2;
        }
        return RunServiceStatus(console);
    }
    if (argc >= 3 && std::wstring_view(argv[1]) == L"query") {
        std::uint32_t start = 0, count = 50;
        bool console = false;
        for (int i = 3; i < argc; ++i) {
            const std::wstring_view a = argv[i];
            if (a == L"--console") {
                console = true;
            } else if (a == L"--start" && i + 1 < argc) {
                start = static_cast<std::uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
            } else if (a == L"--count" && i + 1 < argc) {
                count = static_cast<std::uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
            } else {
                PrintUsage();
                return 2;
            }
        }
        return RunServiceQuery(argv[2], start, count, console);
    }
    if (argc == 4 && std::wstring_view(argv[1]) == L"cache") {
        return RunCache(argv[2], argv[3]);
    }
    if (argc < 3 || std::wstring_view(argv[1]) != L"scan") {
        PrintUsage();
        return 2;
    }

    ScanOptions options;
    options.path = argv[2];
    for (int i = 3; i < argc; ++i) {
        std::wstring_view arg = argv[i];
        if (arg == L"--json") {
            options.json = true;
        } else if (arg == L"--top" && i + 1 < argc) {
            options.top = static_cast<size_t>(std::wcstoull(argv[++i], nullptr, 10));
        } else {
            PrintUsage();
            return 2;
        }
    }
    return RunScan(options);
}
