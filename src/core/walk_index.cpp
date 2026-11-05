#include "core/walk_index.h"

#include <windows.h>

#include "core/dir_listing.h"
#include "core/path_util.h"

namespace dirsize {
namespace {

void CountError(ScanIssues& issues, DWORD error) {
    if (error == ERROR_ACCESS_DENIED) {
        ++issues.accessDenied;
    } else {
        ++issues.otherErrors;
    }
}

// True when the folder reparse point at `path` is followed: its tag can be
// read and is not a name surrogate (junction, symlink, mount point).
bool ShouldFollowReparseDir(const std::wstring& path, std::uint64_t id, VolumeIndex& index) {
    auto readTag = [&](std::uint32_t& tag) {
        HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD t = 0;
        const bool ok = QueryReparseTag(h, t);
        CloseHandle(h);
        tag = t;
        return ok;
    };
    std::uint32_t tag = 0;
    // The walker lists the root itself, so every root child it sees is listed.
    switch (ClassifyChildDir(false, true, true, readTag, tag)) {
        case ChildDirVisibility::TagUnreadable:
            ++index.issues.reparseTagFailures;
            return false;
        case ChildDirVisibility::NameSurrogate:
            ++index.excluded.surrogateReparseDirs;
            return false;
        default:
            CountFollowedReparseTag(index, tag, id);
            return true;
    }
}

}  // namespace

WalkIndexResult BuildIndexByWalking(const std::wstring& extendedRoot) {
    WalkIndexResult result;
    VolumeIndex& index = result.index;

    struct Pending {
        std::wstring path;
        DirIndex dir;
    };
    std::vector<Pending> pending;

    HANDLE rootHandle = OpenDirectoryForListing(extendedRoot.c_str());
    if (rootHandle == INVALID_HANDLE_VALUE) {
        result.rootError = GetLastError();
        return result;
    }
    result.rootOpened = true;
    DirEntry root;
    QueryFileId(rootHandle, root.id);
    index.dirs.push_back(root);
    index.root = 0;

    DirectoryLister lister;
    HANDLE dir = rootHandle;
    std::wstring path = extendedRoot;
    DirIndex current = 0;
    for (;;) {
        const DWORD error = lister.List(dir, [&](const ListedItem& item) {
            if (item.attributes & FILE_ATTRIBUTE_DIRECTORY) {
                std::wstring childPath = JoinPath(path, item.name);
                if ((item.attributes & FILE_ATTRIBUTE_REPARSE_POINT) && !ShouldFollowReparseDir(childPath, item.id, index)) {
                    return;
                }
                DirEntry child;
                child.id = item.id;
                child.parent = current;
                child.nameOffset = AddName(index, item.name, child.nameLength);
                const auto childIndex = static_cast<DirIndex>(index.dirs.size());
                index.dirs.push_back(child);
                pending.push_back({std::move(childPath), childIndex});
                return;
            }
            FileEntry f;
            f.id = item.id;
            f.logicalBytes = item.logicalBytes;
            f.allocatedBytes = item.allocatedBytes;
            f.dir = current;
            f.countedLinks = 1;  // each listed name is one link
            index.files.push_back(f);
            Totals& t = index.dirs[current].subtree;
            t.logicalBytes += item.logicalBytes;
            t.allocatedBytes += item.allocatedBytes;
            ++t.files;
        });
        if (error != ERROR_SUCCESS) {
            CountError(index.issues, error);
        }
        CloseHandle(dir);

        // Next folder that can be opened.
        dir = INVALID_HANDLE_VALUE;
        while (dir == INVALID_HANDLE_VALUE && !pending.empty()) {
            Pending next = std::move(pending.back());
            pending.pop_back();
            dir = OpenDirectoryForListing(next.path.c_str());
            if (dir == INVALID_HANDLE_VALUE) {
                CountError(index.issues, GetLastError());
                continue;
            }
            path = std::move(next.path);
            current = next.dir;
        }
        if (dir == INVALID_HANDLE_VALUE) {
            break;
        }
    }

    RollUp(index);
    return result;
}

}  // namespace dirsize
