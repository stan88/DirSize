#include "core/ntfs_index.h"

#include <windows.h>
#include <winioctl.h>

#include <chrono>
#include <string>
#include <vector>

#include "core/dir_listing.h"
#include "core/layout_index_builder.h"

namespace dirsize {
namespace {

constexpr DWORD kLayoutBufferBytes = 4 * 1024 * 1024;

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

UsnStart QueryUsnStart(HANDLE volume) {
    UsnStart usn;
    USN_JOURNAL_DATA_V0 data{};
    DWORD returned = 0;
    if (!DeviceIoControl(volume, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &data, sizeof(data), &returned, nullptr)) {
        usn.error = GetLastError();  // e.g. ERROR_JOURNAL_NOT_ACTIVE
        return usn;
    }
    usn.active = true;
    usn.journalId = data.UsnJournalID;
    usn.nextUsn = data.NextUsn;
    return usn;
}

// Reads the reparse tag of a folder by ID.
bool ReadReparseTagById(HANDLE hint, std::uint64_t id, std::uint32_t& tagOut) {
    FILE_ID_DESCRIPTOR descriptor{};
    descriptor.dwSize = sizeof(descriptor);
    descriptor.Type = FileIdType;
    descriptor.FileId.QuadPart = static_cast<LONGLONG>(id);
    Handle h(OpenFileById(hint, &descriptor, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT));
    DWORD tag = 0;
    if (!h.valid() || !QueryReparseTag(h.get(), tag)) {
        return false;
    }
    tagOut = tag;
    return true;
}

}  // namespace

NtfsIndexResult BuildNtfsIndex(wchar_t driveLetter) {
    NtfsIndexResult result;
    auto fail = [&](const wchar_t* step) {
        result.error = GetLastError();
        result.failedStep = step;
        return std::move(result);
    };

    const std::wstring rootPath = std::wstring(1, driveLetter) + L":\\";
    Handle root(OpenDirectoryForListing(rootPath.c_str()));
    if (!root.valid()) return fail(L"open root folder");
    std::uint64_t rootId = 0;
    if (!QueryFileId(root.get(), rootId)) return fail(L"query root file ID");

    // What a listing of the root shows decides which root items are visible.
    std::vector<std::uint64_t> rootListing;
    {
        DirectoryLister lister;
        bool idsMissing = false;
        const DWORD error = lister.List(root.get(), [&](const ListedItem& item) {
            if (!item.hasId) idsMissing = true;
            rootListing.push_back(item.id);
        });
        if (error != ERROR_SUCCESS) {
            SetLastError(error);
            return fail(L"list root folder");
        }
        if (idsMissing) {
            SetLastError(ERROR_NOT_SUPPORTED);
            return fail(L"list root folder with file IDs");
        }
    }
    result.rootListed = rootListing.size();

    const std::wstring volumePath = std::wstring(L"\\\\.\\") + driveLetter + L":";
    Handle volume(CreateFileW(volumePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr));
    if (!volume.valid()) return fail(L"open volume (needs elevation)");

    // Record the journal position first: changes made during the scan are
    // then replayed in Phase 3 instead of being lost.
    const UsnStart usn = QueryUsnStart(volume.get());

    auto* buffer = static_cast<std::byte*>(
        VirtualAlloc(nullptr, kLayoutBufferBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (buffer == nullptr) return fail(L"allocate layout buffer");

    LayoutIndexBuilder builder;
    const auto t0 = std::chrono::steady_clock::now();
    QUERY_FILE_LAYOUT_INPUT input{};
    input.FilterType = QUERY_FILE_LAYOUT_FILTER_TYPE_NONE;
    DWORD endError = 0;
    for (bool first = true;; first = false) {
        input.Flags = QUERY_FILE_LAYOUT_INCLUDE_NAMES | QUERY_FILE_LAYOUT_INCLUDE_STREAMS |
                      QUERY_FILE_LAYOUT_INCLUDE_STREAMS_WITH_NO_CLUSTERS_ALLOCATED |
                      (first ? QUERY_FILE_LAYOUT_RESTART : 0);
        DWORD returned = 0;
        if (!DeviceIoControl(volume.get(), FSCTL_QUERY_FILE_LAYOUT, &input, sizeof(input), buffer, kLayoutBufferBytes,
                             &returned, nullptr)) {
            endError = GetLastError();
            break;
        }
        ++result.layoutCalls;
        const std::uint32_t count = ParseLayoutBuffer(buffer, returned, result.parse,
                                                      [&](const LayoutEntry& entry) { builder.Add(entry); });
        if (count == 0) break;
    }
    VirtualFree(buffer, 0, MEM_RELEASE);
    const auto t1 = std::chrono::steady_clock::now();
    result.enumerateSeconds = std::chrono::duration<double>(t1 - t0).count();
    if (endError != 0 && endError != ERROR_HANDLE_EOF) {
        SetLastError(endError);
        return fail(L"FSCTL_QUERY_FILE_LAYOUT");
    }

    result.endError = endError;
    result.index = builder.Finish(rootId, std::move(rootListing), [&](std::uint64_t id, std::uint32_t& tag) {
        return ReadReparseTagById(root.get(), id, tag);
    });
    result.finishSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    result.index.usn = usn;
    if (result.index.root == kNoDir) {
        SetLastError(ERROR_NOT_FOUND);
        return fail(L"find root folder in layout");
    }
    result.ok = true;
    return result;
}

}  // namespace dirsize
