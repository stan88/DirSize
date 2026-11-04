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
#include "core/path_util.h"
#include "core/walk_index.h"

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
    WriteLine();
    WriteLine(L"Walks the folder tree under <path> and reports total size, file and");
    WriteLine(L"folder counts, elapsed time and peak memory use.");
    WriteLine(L"  --top N   list the N largest immediate sub-folders (default 20, 0 = all)");
    WriteLine(L"  --json    machine-readable output");
    WriteLine();
    WriteLine(L"verify builds the folder index of a whole NTFS volume with both engines");
    WriteLine(L"and compares them folder by folder.");
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
            if (l != layout.root) CompareFilesIn(PathOf(layout, l, rootPath), layout, l, indent + L"  ", 5, false);
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

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    if (argc == 3 && std::wstring_view(argv[1]) == L"verify") {
        return RunVerify(argv[2]);
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
