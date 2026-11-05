// Unit tests for the DirSize core. No elevation needed.
//
// - Layout parser: hand-built FSCTL_QUERY_FILE_LAYOUT buffers, including
//   every truncation length and corrupted offsets.
// - Layout index builder: a synthetic volume covering the counting rules.
// - Walker index: a real temporary folder tree with a hard link, a hidden
//   system file, a named stream and a junction.

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "core/dir_listing.h"
#include "core/index.h"
#include "core/layout_index_builder.h"
#include "core/layout_parser.h"
#include "core/path_util.h"
#include "core/walk_index.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                                 \
    do {                                                                                               \
        ++g_checks;                                                                                    \
        const auto va_ = static_cast<unsigned long long>(a);                                           \
        const auto vb_ = static_cast<unsigned long long>(b);                                           \
        if (va_ != vb_) {                                                                              \
            std::printf("FAIL %s:%d: %s == %s (%llu vs %llu)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
            ++g_failures;                                                                              \
        }                                                                                              \
    } while (0)

using namespace dirsize;

// ---- layout buffer construction -------------------------------------------

constexpr size_t kNameHeader = offsetof(FILE_LAYOUT_NAME_ENTRY, FileName);
constexpr size_t kStreamHeader = offsetof(STREAM_LAYOUT_ENTRY, StreamIdentifier);

struct NameSpec {
    std::uint64_t parent;
    std::wstring name;
    DWORD flags = FILE_LAYOUT_NAME_ENTRY_PRIMARY;
};

struct StreamSpec {
    DWORD code = kDataAttributeTypeCode;
    std::wstring id;  // empty = unnamed
    std::int64_t eof = 0;
    std::int64_t alloc = 0;
    DWORD version = 1;
};

struct FileSpec {
    std::uint64_t id;
    DWORD attributes = FILE_ATTRIBUTE_ARCHIVE;
    std::vector<NameSpec> names;
    std::vector<StreamSpec> streams;
};

class LayoutBuffer {
public:
    LayoutBuffer() { Append(nullptr, sizeof(QUERY_FILE_LAYOUT_OUTPUT)); }

    void AddFile(const FileSpec& spec) {
        Align8();
        const size_t at = bytes_.size();
        if (lastFile_ != SIZE_MAX) {
            Patch<FILE_LAYOUT_ENTRY>(lastFile_, [&](FILE_LAYOUT_ENTRY& e) {
                e.NextFileOffset = static_cast<DWORD>(at - lastFile_);
            });
        } else {
            Patch<QUERY_FILE_LAYOUT_OUTPUT>(0, [&](QUERY_FILE_LAYOUT_OUTPUT& h) { h.FirstFileOffset = static_cast<DWORD>(at); });
        }
        FILE_LAYOUT_ENTRY entry{};
        entry.Version = 1;
        entry.FileAttributes = spec.attributes;
        entry.FileReferenceNumber = spec.id;
        Append(&entry, sizeof(entry));

        size_t previous = SIZE_MAX;
        for (const NameSpec& n : spec.names) {
            Align8();
            const size_t nameAt = bytes_.size();
            if (previous == SIZE_MAX) {
                Patch<FILE_LAYOUT_ENTRY>(at, [&](FILE_LAYOUT_ENTRY& e) { e.FirstNameOffset = static_cast<DWORD>(nameAt - at); });
            } else {
                Patch<FILE_LAYOUT_NAME_ENTRY>(previous, [&](FILE_LAYOUT_NAME_ENTRY& e) {
                    e.NextNameOffset = static_cast<DWORD>(nameAt - previous);
                });
            }
            FILE_LAYOUT_NAME_ENTRY header{};
            header.Flags = n.flags;
            header.ParentFileReferenceNumber = n.parent;
            header.FileNameLength = static_cast<DWORD>(n.name.size() * sizeof(wchar_t));
            Append(&header, kNameHeader);
            Append(n.name.data(), n.name.size() * sizeof(wchar_t));
            previous = nameAt;
        }
        previous = SIZE_MAX;
        for (const StreamSpec& s : spec.streams) {
            Align8();
            const size_t streamAt = bytes_.size();
            if (previous == SIZE_MAX) {
                Patch<FILE_LAYOUT_ENTRY>(at, [&](FILE_LAYOUT_ENTRY& e) { e.FirstStreamOffset = static_cast<DWORD>(streamAt - at); });
            } else {
                Patch<STREAM_LAYOUT_ENTRY>(previous, [&](STREAM_LAYOUT_ENTRY& e) {
                    e.NextStreamOffset = static_cast<DWORD>(streamAt - previous);
                });
            }
            STREAM_LAYOUT_ENTRY header{};
            header.Version = s.version;
            header.AttributeTypeCode = s.code;
            header.EndOfFile.QuadPart = s.eof;
            header.AllocationSize.QuadPart = s.alloc;
            header.StreamIdentifierLength = static_cast<DWORD>(s.id.size() * sizeof(wchar_t));
            Append(&header, kStreamHeader);
            Append(s.id.data(), s.id.size() * sizeof(wchar_t));
            previous = streamAt;
        }
        lastFile_ = at;
        Patch<QUERY_FILE_LAYOUT_OUTPUT>(0, [&](QUERY_FILE_LAYOUT_OUTPUT& h) { ++h.FileEntryCount; });
    }

    std::vector<std::byte>& bytes() { return bytes_; }
    size_t lastFile() const { return lastFile_; }

    template <typename T, typename F>
    void Patch(size_t at, F&& change) {
        T value{};
        std::memcpy(&value, bytes_.data() + at, std::min(sizeof(T), bytes_.size() - at));
        change(value);
        std::memcpy(bytes_.data() + at, &value, std::min(sizeof(T), bytes_.size() - at));
    }

private:
    void Append(const void* data, size_t n) {
        const size_t at = bytes_.size();
        bytes_.resize(at + n);
        if (data != nullptr && n != 0) std::memcpy(bytes_.data() + at, data, n);
    }
    void Align8() {
        while (bytes_.size() % 8 != 0) bytes_.push_back(std::byte{0});
    }

    std::vector<std::byte> bytes_;
    size_t lastFile_ = SIZE_MAX;
};

std::vector<LayoutEntry> ParseAll(const std::vector<std::byte>& bytes, LayoutParseStats& stats) {
    // Copy into an exact-size heap block so AddressSanitizer flags any read
    // past the end.
    std::vector<std::byte> exact(bytes.begin(), bytes.end());
    std::vector<LayoutEntry> out;
    std::vector<std::vector<wchar_t>> nameCopies;
    ParseLayoutBuffer(exact.data(), exact.size(), stats, [&](const LayoutEntry& e) {
        LayoutEntry copy = e;
        copy.links.clear();
        for (const LayoutLink& l : e.links) {
            (void)l;
            copy.links.push_back({l.parentId, {}});  // names point into `exact`; not kept
        }
        out.push_back(std::move(copy));
    });
    return out;
}

FileSpec SampleFile() {
    FileSpec f;
    f.id = 0x0001000000000042ull;
    f.names = {{5, L"LONGFI~1.TXT", FILE_LAYOUT_NAME_ENTRY_DOS}, {5, L"long file name.txt"}, {7, L"second link.txt", 0}};
    f.streams = {{0x10, L"", 72, 0}, {kDataAttributeTypeCode, L"", 1234, 4096}, {kDataAttributeTypeCode, L"Zone.Identifier", 26, 32}};
    return f;
}

void TestParserBasic() {
    LayoutBuffer buffer;
    buffer.AddFile(SampleFile());
    FileSpec dir;
    dir.id = 77;
    dir.attributes = FILE_ATTRIBUTE_DIRECTORY;
    dir.names = {{5, L"folder"}};
    buffer.AddFile(dir);

    LayoutParseStats stats;
    std::vector<std::wstring> names;
    std::vector<LayoutEntry> entries;
    std::vector<std::byte> exact = buffer.bytes();
    const std::uint32_t count = ParseLayoutBuffer(exact.data(), exact.size(), stats, [&](const LayoutEntry& e) {
        entries.push_back(e);
        for (const LayoutLink& l : e.links) names.emplace_back(l.name);
    });
    CHECK_EQ(count, 2);
    CHECK_EQ(entries.size(), 2);
    CHECK_EQ(stats.malformedEntries, 0);
    CHECK_EQ(stats.dosOnlyNames, 1);
    CHECK_EQ(stats.truncatedChains, 0);
    if (entries.size() == 2) {
        const LayoutEntry& f = entries[0];
        CHECK_EQ(f.id, 0x0001000000000042ull);
        CHECK(!f.malformed);
        CHECK(f.hasData);
        CHECK_EQ(f.dataLogicalBytes, 1234);
        CHECK_EQ(f.dataAllocatedBytes, 4096);
        CHECK_EQ(f.namedStreams, 1);
        CHECK_EQ(f.namedLogicalBytes, 26);
        CHECK_EQ(f.namedAllocatedBytes, 32);
        CHECK_EQ(f.links.size(), 2);
        CHECK(entries[1].attributes & FILE_ATTRIBUTE_DIRECTORY);
        CHECK(!entries[1].hasData);
    }
    CHECK(names.size() == 3 && names[0] == L"long file name.txt" && names[1] == L"second link.txt" &&
          names[2] == L"folder");

    // Version 2 stream entries are accepted too; FileEntryCount == 0 ends.
    LayoutParseStats endStats;
    LayoutBuffer empty;
    CHECK_EQ(ParseLayoutBuffer(empty.bytes().data(), empty.bytes().size(), endStats, [](const LayoutEntry&) {}), 0);
    CHECK_EQ(endStats.entries, 0);
}

void TestParserTruncation() {
    LayoutBuffer buffer;
    buffer.AddFile(SampleFile());
    FileSpec second = SampleFile();
    second.id = 99;
    buffer.AddFile(second);
    const std::vector<std::byte>& full = buffer.bytes();

    // Every prefix must parse without reading past the end. Entries that are
    // reported must either be complete and correct, or flagged malformed.
    int wrongButClean = 0;
    for (size_t cut = 0; cut < full.size(); ++cut) {
        std::vector<std::byte> prefix(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(cut));
        LayoutParseStats stats;
        std::vector<LayoutEntry> entries = ParseAll(prefix, stats);
        for (const LayoutEntry& e : entries) {
            if (!e.malformed && (e.dataLogicalBytes != 1234 || e.namedStreams != 1 || e.links.size() != 2)) {
                ++wrongButClean;
            }
        }
        if (cut < sizeof(QUERY_FILE_LAYOUT_OUTPUT)) {
            CHECK_EQ(stats.badHeaders, 1);
        }
    }
    CHECK_EQ(wrongButClean, 0);
}

void TestParserCorruption() {
    auto parseWith = [](auto&& corrupt) {
        LayoutBuffer buffer;
        buffer.AddFile(SampleFile());
        corrupt(buffer);
        LayoutParseStats stats;
        std::vector<LayoutEntry> entries = ParseAll(buffer.bytes(), stats);
        return std::make_pair(entries, stats);
    };

    {  // first file offset far past the end
        auto [entries, stats] = parseWith([](LayoutBuffer& b) {
            b.Patch<QUERY_FILE_LAYOUT_OUTPUT>(0, [](QUERY_FILE_LAYOUT_OUTPUT& h) { h.FirstFileOffset = 0xFFFFFFF0u; });
        });
        CHECK_EQ(entries.size(), 0);
        CHECK_EQ(stats.truncatedChains, 1);
    }
    {  // name offset past the entry
        auto [entries, stats] = parseWith([](LayoutBuffer& b) {
            b.Patch<FILE_LAYOUT_ENTRY>(b.lastFile(), [](FILE_LAYOUT_ENTRY& e) { e.FirstNameOffset = 0x7FFFFFFFu; });
        });
        CHECK(entries.size() == 1 && entries[0].malformed);
        CHECK_EQ(stats.malformedEntries, 1);
    }
    {  // stream offset past the entry
        auto [entries, stats] = parseWith([](LayoutBuffer& b) {
            b.Patch<FILE_LAYOUT_ENTRY>(b.lastFile(), [](FILE_LAYOUT_ENTRY& e) { e.FirstStreamOffset = 0xFFFFFFFFu; });
        });
        CHECK(entries.size() == 1 && entries[0].malformed);
    }
    {  // name length longer than the buffer
        auto [entries, stats] = parseWith([](LayoutBuffer& b) {
            FILE_LAYOUT_ENTRY e{};
            std::memcpy(&e, b.bytes().data() + b.lastFile(), sizeof(e));
            b.Patch<FILE_LAYOUT_NAME_ENTRY>(b.lastFile() + e.FirstNameOffset,
                                            [](FILE_LAYOUT_NAME_ENTRY& n) { n.FileNameLength = 0x10000; });
        });
        CHECK(entries.size() == 1 && entries[0].malformed);
    }
    {  // next-name offset that does not advance past the header
        auto [entries, stats] = parseWith([](LayoutBuffer& b) {
            FILE_LAYOUT_ENTRY e{};
            std::memcpy(&e, b.bytes().data() + b.lastFile(), sizeof(e));
            b.Patch<FILE_LAYOUT_NAME_ENTRY>(b.lastFile() + e.FirstNameOffset,
                                            [](FILE_LAYOUT_NAME_ENTRY& n) { n.NextNameOffset = 4; });
        });
        CHECK(entries.size() == 1 && entries[0].malformed);
    }
    {  // unknown stream version
        auto [entries, stats] = parseWith([](LayoutBuffer& b) {
            FILE_LAYOUT_ENTRY e{};
            std::memcpy(&e, b.bytes().data() + b.lastFile(), sizeof(e));
            b.Patch<STREAM_LAYOUT_ENTRY>(b.lastFile() + e.FirstStreamOffset, [](STREAM_LAYOUT_ENTRY& s) { s.Version = 3; });
        });
        CHECK(entries.size() == 1 && entries[0].malformed);
        CHECK_EQ(stats.unknownStreamVersions, 1);
    }
    {  // unknown file entry version
        auto [entries, stats] = parseWith([](LayoutBuffer& b) {
            b.Patch<FILE_LAYOUT_ENTRY>(b.lastFile(), [](FILE_LAYOUT_ENTRY& e) { e.Version = 9; });
        });
        CHECK(entries.size() == 1 && entries[0].malformed);
        CHECK_EQ(stats.unknownFileVersions, 1);
    }
    {  // negative size
        auto [entries, stats] = parseWith([](LayoutBuffer& b) {
            FILE_LAYOUT_ENTRY e{};
            std::memcpy(&e, b.bytes().data() + b.lastFile(), sizeof(e));
            b.Patch<STREAM_LAYOUT_ENTRY>(b.lastFile() + e.FirstStreamOffset,
                                         [](STREAM_LAYOUT_ENTRY& s) { s.EndOfFile.QuadPart = -1; });
        });
        CHECK(entries.size() == 1 && entries[0].malformed);
    }
    {  // next-file offset past the end stops the buffer
        auto [entries, stats] = parseWith([](LayoutBuffer& b) {
            b.Patch<QUERY_FILE_LAYOUT_OUTPUT>(0, [](QUERY_FILE_LAYOUT_OUTPUT& h) { h.FileEntryCount = 2; });
            b.Patch<FILE_LAYOUT_ENTRY>(b.lastFile(), [](FILE_LAYOUT_ENTRY& e) { e.NextFileOffset = 0x40000000u; });
        });
        CHECK_EQ(entries.size(), 0);
        CHECK_EQ(stats.truncatedChains, 1);
    }
}

// ---- depth and roll-up -----------------------------------------------------

void TestDepths() {
    std::vector<DirEntry> dirs(6);
    dirs[0].parent = kNoDir;  // root
    dirs[1].parent = 0;
    dirs[2].parent = 1;
    dirs[3].parent = 4;  // 3 <-> 4 cycle
    dirs[4].parent = 3;
    dirs[5].parent = 99;  // parent outside the table
    const std::vector<std::uint32_t> d = ComputeDepths(dirs, 0);
    CHECK_EQ(d[0], 0);
    CHECK_EQ(d[1], 1);
    CHECK_EQ(d[2], 2);
    CHECK_EQ(d[3], kUnreachableDepth);
    CHECK_EQ(d[4], kUnreachableDepth);
    CHECK_EQ(d[5], kUnreachableDepth);
}

// ---- layout index builder --------------------------------------------------

LayoutEntry Dir(std::uint64_t id, std::uint64_t parent, const wchar_t* name, DWORD extra = 0) {
    LayoutEntry e;
    e.id = id;
    e.attributes = FILE_ATTRIBUTE_DIRECTORY | extra;
    e.links.push_back({parent, name});
    return e;
}

LayoutEntry File(std::uint64_t id, std::vector<std::uint64_t> parents, std::uint64_t size) {
    LayoutEntry e;
    e.id = id;
    e.attributes = FILE_ATTRIBUTE_ARCHIVE;
    for (std::uint64_t p : parents) e.links.push_back({p, L"f"});
    e.hasData = true;
    e.dataLogicalBytes = size;
    e.dataAllocatedBytes = 4096;
    return e;
}

void TestLayoutBuilder() {
    LayoutIndexBuilder b;
    // Entries arrive in arbitrary order.
    b.Add(File(303, {600}, 50));        // in C\CSub
    b.Add(Dir(600, 500, L"CSub"));
    b.Add(Dir(100, 5, L"A"));
    b.Add(Dir(5, 5, L"."));             // root
    b.Add(Dir(11, 5, L"$Extend"));      // not in the root listing
    b.Add(Dir(12, 11, L"$RmMetadata"));
    b.Add(Dir(400, 5, L"J", FILE_ATTRIBUTE_REPARSE_POINT));  // junction
    b.Add(Dir(500, 5, L"C", FILE_ATTRIBUTE_REPARSE_POINT));  // non-surrogate reparse folder
    b.Add(Dir(700, 9999, L"Orphan"));
    b.Add(File(1, {5}, 1000));          // $MFT: in root, not listed
    LayoutEntry withNamed = File(200, {5}, 10);
    withNamed.namedStreams = 1;
    withNamed.namedLogicalBytes = 7;
    withNamed.namedAllocatedBytes = 8;
    b.Add(withNamed);
    b.Add(File(300, {5, 100}, 20));     // hard link in root and A
    b.Add(File(301, {100}, 30));
    b.Add(File(302, {12}, 40));         // under $Extend
    b.Add(File(304, {700}, 60));        // under an orphan folder
    b.Add(File(305, {8888}, 70));       // parent unknown
    LayoutEntry bad = File(306, {100}, 80);
    bad.malformed = true;
    b.Add(bad);

    int tagCalls = 0;
    constexpr std::uint32_t kCloudTag = 0x9000001Au;  // not a name surrogate
    VolumeIndex index = b.Finish(5, {100, 200, 300, 400, 500}, [&](std::uint64_t id, std::uint32_t& tag) {
        ++tagCalls;
        tag = id == 400 ? IO_REPARSE_TAG_MOUNT_POINT : kCloudTag;
        return true;
    });
    CHECK_EQ(tagCalls, 2);
    CHECK(index.followedReparseTags.size() == 1 && index.followedReparseTags[0].tag == kCloudTag);
    // Root children found in the listing: folders A, J, C and files 200, 300.
    CHECK_EQ(index.rootListingMatched, 5);
    CHECK(index.sortedById);
    CHECK_EQ(index.dirs.size(), 4);
    CHECK(index.root != kNoDir);
    if (index.dirs.size() != 4 || index.root == kNoDir) return;
    CHECK_EQ(index.dirs[0].id, 5);
    CHECK_EQ(index.dirs[1].id, 100);
    CHECK_EQ(index.dirs[2].id, 500);
    CHECK_EQ(index.dirs[3].id, 600);
    CHECK(index.Name(1) == L"A");
    CHECK(index.Name(3) == L"CSub");

    const Totals& root = index.dirs[index.root].subtree;
    CHECK_EQ(root.files, 5);
    CHECK_EQ(root.logicalBytes, 10 + 20 + 20 + 30 + 50);
    CHECK_EQ(root.allocatedBytes, 5 * 4096);
    CHECK_EQ(root.folders, 3);
    CHECK_EQ(index.dirs[1].subtree.files, 2);
    CHECK_EQ(index.dirs[1].subtree.logicalBytes, 50);
    CHECK_EQ(index.dirs[2].subtree.folders, 1);
    CHECK_EQ(index.dirs[2].subtree.logicalBytes, 50);

    CHECK_EQ(index.files.size(), 4);
    CHECK_EQ(index.extraLinks.size(), 1);
    for (size_t i = 1; i < index.files.size(); ++i) CHECK(index.files[i - 1].id < index.files[i].id);

    CHECK_EQ(index.excluded.hiddenRootFiles.files, 1);
    CHECK_EQ(index.excluded.hiddenRootFiles.logicalBytes, 1000);
    CHECK_EQ(index.excluded.hidden.files, 1);
    CHECK_EQ(index.excluded.hidden.logicalBytes, 40);
    CHECK_EQ(index.excluded.hiddenFolders, 2);
    CHECK_EQ(index.excluded.surrogateReparseDirs, 1);
    CHECK_EQ(index.excluded.unreachableFolders, 1);
    CHECK_EQ(index.excluded.unreachableFiles, 2);
    CHECK(index.unreachableFileSamples.size() == 2 && index.unreachableFileSamples[0].id == 304 &&
          index.unreachableFileSamples[0].parentId == 700 && index.unreachableFileSamples[1].id == 305 &&
          index.unreachableFileSamples[1].parentId == 8888);
    CHECK(index.followedReparseDirIds.size() == 1 && index.followedReparseDirIds[0] == 500);
    CHECK_EQ(index.excluded.namedStreams, 1);
    CHECK_EQ(index.excluded.namedStreamLogicalBytes, 7);
    CHECK_EQ(index.issues.malformedEntries, 1);

    // Child list of the root: A and C.
    int children = 0;
    for (DirIndex c = index.dirs[index.root].firstChild; c != kNoDir; c = index.dirs[c].nextSibling) ++children;
    CHECK_EQ(children, 2);
}

void TestLayoutBuilderNoRoot() {
    LayoutIndexBuilder b;
    b.Add(Dir(100, 5, L"A"));
    VolumeIndex index = b.Finish(5, {100}, [](std::uint64_t, std::uint32_t&) { return false; });
    CHECK(index.root == kNoDir);
}

// ---- walker index on a real temporary tree --------------------------------

bool WriteFileBytes(const std::wstring& path, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<char> data(size, 'x');
    DWORD written = 0;
    const bool ok = size == 0 || (WriteFile(h, data.data(), static_cast<DWORD>(size), &written, nullptr) && written == size);
    CloseHandle(h);
    return ok;
}

void TestWalkerTree() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring root = std::wstring(temp) + L"dirsize-test-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring sub = root + L"\\sub";
    const std::wstring junction = root + L"\\junction";
    CHECK(CreateDirectoryW(root.c_str(), nullptr));
    CHECK(CreateDirectoryW(sub.c_str(), nullptr));
    CHECK(WriteFileBytes(root + L"\\a.txt", 100));
    CHECK(WriteFileBytes(root + L"\\hidden.bin", 50));
    CHECK(SetFileAttributesW((root + L"\\hidden.bin").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
    CHECK(WriteFileBytes(root + L"\\ads.txt", 10));
    CHECK(WriteFileBytes(root + L"\\ads.txt:extra", 1000));  // named stream: not counted
    CHECK(WriteFileBytes(sub + L"\\b.txt", 200));
    CHECK(CreateHardLinkW((sub + L"\\b-link.txt").c_str(), (sub + L"\\b.txt").c_str(), nullptr));
    const std::wstring mklink = L"cmd /c mklink /J \"" + junction + L"\" \"" + sub + L"\" >nul 2>&1";
    const bool haveJunction = _wsystem(mklink.c_str()) == 0;
    if (!haveJunction) std::printf("note: junction not created; junction checks skipped\n");

    WalkIndexResult r = BuildIndexByWalking(ToExtendedPath(root));
    CHECK(r.rootOpened);
    const VolumeIndex& index = r.index;
    if (r.rootOpened) {
        const Totals& t = index.dirs[index.root].subtree;
        CHECK_EQ(t.files, 5);  // a, hidden, ads, b, b-link
        CHECK_EQ(t.logicalBytes, 100 + 50 + 10 + 200 + 200);
        CHECK_EQ(t.folders, 1);
        CHECK_EQ(index.dirs.size(), 2);
        CHECK(!index.sortedById);
        if (index.dirs.size() == 2) {
            CHECK(index.Name(1) == L"sub");
            CHECK_EQ(index.dirs[1].subtree.files, 2);
            CHECK_EQ(index.dirs[1].subtree.logicalBytes, 400);
        }
        if (haveJunction) CHECK_EQ(index.excluded.surrogateReparseDirs, 1);

        // The listing's file IDs must equal the IDs from an open handle
        // (GetFileInformationByHandle), the ID space the layout uses.
        auto idOf = [](const std::wstring& path) {
            std::uint64_t id = 0;
            HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                QueryFileId(h, id);
                CloseHandle(h);
            }
            return id;
        };
        const std::uint64_t fileId = idOf(root + L"\\a.txt");
        const std::uint64_t subId = idOf(sub);
        CHECK(fileId != 0 && subId != 0);
        bool fileFound = false;
        for (const FileEntry& f : index.files) fileFound = fileFound || f.id == fileId;
        CHECK(fileFound);
        if (index.dirs.size() == 2) CHECK_EQ(index.dirs[1].id, subId);
        CHECK_EQ(index.dirs[index.root].id, idOf(root));
        CHECK_EQ(index.issues.accessDenied + index.issues.otherErrors, 0);
    }

    if (haveJunction) RemoveDirectoryW(junction.c_str());  // removes the link only
    DeleteFileW((sub + L"\\b-link.txt").c_str());
    DeleteFileW((sub + L"\\b.txt").c_str());
    SetFileAttributesW((root + L"\\hidden.bin").c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW((root + L"\\hidden.bin").c_str());
    DeleteFileW((root + L"\\a.txt").c_str());
    DeleteFileW((root + L"\\ads.txt").c_str());
    CHECK(RemoveDirectoryW(sub.c_str()));
    CHECK(RemoveDirectoryW(root.c_str()));
}

}  // namespace

// Listing the same folder twice with one handle must return every item both
// times: GetFileInformationByHandleEx resumes an enumeration on the same
// handle, so each List() has to restart it (a bug found in Phase 3: catch-ups
// after the first saw an empty root listing).
void TestListSameHandleTwice() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring root = std::wstring(temp) + L"dirsize-list-test-" + std::to_wstring(GetCurrentProcessId());
    CHECK(CreateDirectoryW(root.c_str(), nullptr));
    for (int i = 0; i < 300; ++i) CHECK(WriteFileBytes(root + L"\\file-" + std::to_wstring(i) + L".txt", 1));
    HANDLE h = OpenDirectoryForListing(root.c_str());
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        for (int round = 0; round < 3; ++round) {
            DirectoryLister lister;  // a new lister each round, as CatchUp does
            size_t n = 0;
            CHECK(lister.List(h, [&](const ListedItem&) { ++n; }) == ERROR_SUCCESS);
            CHECK_EQ(n, 300);
        }
        DirectoryLister reused;  // and one lister used repeatedly
        for (int round = 0; round < 2; ++round) {
            size_t n = 0;
            CHECK(reused.List(h, [&](const ListedItem&) { ++n; }) == ERROR_SUCCESS);
            CHECK_EQ(n, 300);
        }
        CloseHandle(h);
    }
    for (int i = 0; i < 300; ++i) DeleteFileW((root + L"\\file-" + std::to_wstring(i) + L".txt").c_str());
    RemoveDirectoryW(root.c_str());
}

int main() {
    TestParserBasic();
    TestParserTruncation();
    TestParserCorruption();
    TestDepths();
    TestLayoutBuilder();
    TestLayoutBuilderNoRoot();
    TestWalkerTree();
    TestListSameHandleTwice();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
