#pragma once

// The folder index: a whole-volume (or whole-tree) table of folders and files
// with per-folder subtree totals. Both scan engines (NTFS layout and the
// directory walker) fill the same structure, so their results can be compared
// directly. See docs/DESIGN-phase2.md.
//
// Counting rules (decided 2026-11-03):
// - Only items that a directory listing shows are counted, the same set that
//   Explorer's folder properties count (hidden and system files included).
//   NTFS metafiles, $Extend and named data streams are excluded and reported
//   separately in ExcludedTotals.
// - Hard links are counted once per link (each folder that shows the file
//   counts it).
// - Directory reparse points that are name surrogates (junctions, symbolic
//   links, mount points) are not followed and not counted. Other directory
//   reparse points are ordinary folders.

#include <cstdint>
#include <string_view>
#include <vector>

namespace dirsize {

using DirIndex = std::uint32_t;
constexpr DirIndex kNoDir = 0xFFFFFFFFu;

struct Totals {
    std::uint64_t logicalBytes = 0;    // sum of file lengths (EndOfFile)
    std::uint64_t allocatedBytes = 0;  // sum of space on disk (AllocationSize)
    std::uint64_t files = 0;           // file links
    std::uint64_t folders = 0;         // descendant folders, not counting the folder itself

    void Add(const Totals& other) {
        logicalBytes += other.logicalBytes;
        allocatedBytes += other.allocatedBytes;
        files += other.files;
        folders += other.folders;
    }
    bool operator==(const Totals&) const = default;
};

// One file. `dir` is the folder of its first counted link; further counted
// links are in VolumeIndex::extraLinks. Sizes are those of the unnamed data
// stream. Phase 3 needs the per-file sizes to apply change deltas.
struct FileEntry {
    std::uint64_t id = 0;  // file ID; 0 when the engine cannot get one
    std::uint64_t logicalBytes = 0;
    std::uint64_t allocatedBytes = 0;
    DirIndex dir = kNoDir;
    std::uint16_t countedLinks = 0;  // links in visible folders (>= 1)
    std::uint16_t flags = 0;         // reserved
};
static_assert(sizeof(FileEntry) == 32);

struct ExtraLink {
    std::uint32_t file = 0;  // index into VolumeIndex::files
    DirIndex dir = kNoDir;
};

struct DirEntry {
    std::uint64_t id = 0;
    DirIndex parent = kNoDir;
    DirIndex firstChild = kNoDir;
    DirIndex nextSibling = kNoDir;
    std::uint32_t nameOffset = 0;  // into VolumeIndex::names
    std::uint16_t nameLength = 0;  // in UTF-16 code units
    std::uint16_t flags = 0;       // reserved
    Totals subtree;                // everything below this folder
};
static_assert(sizeof(DirEntry) == 64);

// What was left out by the counting rules. Informational only.
struct ExcludedTotals {
    Totals hiddenRootFiles;  // files in the root that its listing does not show ($MFT, $LogFile, ...)
    Totals hidden;           // files under hidden folders ($Extend, ...)
    std::uint64_t hiddenFolders = 0;
    std::uint64_t namedStreams = 0;  // named data streams on counted files
    std::uint64_t namedStreamLogicalBytes = 0;
    std::uint64_t namedStreamAllocatedBytes = 0;
    std::uint64_t surrogateReparseDirs = 0;  // junctions, symlinks, mount points
    std::uint64_t unreachableFolders = 0;    // parent chain does not reach the root
    std::uint64_t unreachableFiles = 0;
};

// Position of the NTFS change journal when the scan started. Changes after
// this point are replayed in Phase 3.
struct UsnStart {
    bool active = false;
    std::uint32_t error = 0;  // Win32 error when the journal could not be queried
    std::uint64_t journalId = 0;
    std::int64_t nextUsn = 0;
};

struct ScanIssues {
    std::uint64_t accessDenied = 0;
    std::uint64_t otherErrors = 0;
    std::uint64_t malformedEntries = 0;  // layout entries that failed validation
    std::uint64_t duplicateIds = 0;
    std::uint64_t reparseTagFailures = 0;  // folder reparse tags that could not be read (not followed)
};

struct ReparseTagCount {
    std::uint32_t tag = 0;
    std::uint64_t folders = 0;
};

struct VolumeIndex {
    std::vector<FileEntry> files;
    std::vector<ExtraLink> extraLinks;
    std::vector<DirEntry> dirs;
    std::vector<wchar_t> names;
    DirIndex root = kNoDir;
    bool sortedById = false;  // true when dirs and files are sorted by ID (NTFS engine)

    ExcludedTotals excluded;
    UsnStart usn;
    ScanIssues issues;
    // Folder reparse points that are not name surrogates and were followed,
    // by tag (e.g. cloud-file folders).
    std::vector<ReparseTagCount> followedReparseTags;
    // NTFS engine: root-listing IDs found among the layout's root children.
    std::uint64_t rootListingMatched = 0;

    std::wstring_view Name(DirIndex dir) const {
        const DirEntry& d = dirs[dir];
        return {names.data() + d.nameOffset, d.nameLength};
    }
    // Bytes held by the tables (capacity, not size).
    std::uint64_t MemoryBytes() const;
};

// Appends a folder name to the name pool and returns its offset. Names longer
// than 65535 code units are truncated (NTFS names are at most 255).
// Counts one followed folder reparse point under its tag.
void CountFollowedReparseTag(VolumeIndex& index, std::uint32_t tag);

std::uint32_t AddName(VolumeIndex& index, std::wstring_view name, std::uint16_t& length);

// Depth of every folder below `root` (root = 0), following DirEntry::parent.
// Folders whose parent chain does not reach `root`, or that sit on a cycle,
// get kUnreachableDepth.
constexpr std::uint32_t kUnreachableDepth = 0xFFFFFFFFu;
std::vector<std::uint32_t> ComputeDepths(const std::vector<DirEntry>& dirs, DirIndex root);

// Finishes an index whose dirs hold parent links and, in `subtree`, the
// totals of the files directly inside each folder. Links children into
// firstChild/nextSibling lists and rolls the totals up, deepest folders first.
// All folders must be reachable from the root.
void RollUp(VolumeIndex& index);

}  // namespace dirsize
