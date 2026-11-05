#pragma once

// Directory listing helpers shared by both index engines.

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace dirsize {

struct ListedItem {
    std::wstring_view name;
    std::uint64_t id = 0;  // valid only when hasId
    bool hasId = false;
    std::uint32_t attributes = 0;
    std::uint64_t logicalBytes = 0;
    std::uint64_t allocatedBytes = 0;
};

class DirectoryLister {
public:
    DirectoryLister();

    // Lists every entry of an open directory handle except "." and "..".
    // Uses FileIdBothDirectoryInfo (gives file IDs) and falls back to
    // FileFullDirectoryInfo for file systems that do not support it.
    // Returns ERROR_SUCCESS or the Win32 error that stopped the listing.
    DWORD List(HANDLE dir, const std::function<void(const ListedItem&)>& onItem);

private:
    std::vector<ULONGLONG> buffer_;  // ULONGLONG keeps entries 8-byte aligned
    bool useIds_ = true;
};

// Opens a folder for listing (FILE_LIST_DIRECTORY, backup semantics).
HANDLE OpenDirectoryForListing(const wchar_t* path);

// Reparse tag of an open handle (FileAttributeTagInfo). Returns false on error.
bool QueryReparseTag(HANDLE handle, DWORD& tag);

// Reparse tag of a file or folder by ID (OpenFileById with `hint`, a handle
// on the same volume). Returns false when it cannot be opened or read.
bool ReadReparseTagById(HANDLE hint, std::uint64_t id, std::uint32_t& tag);

// The 64-bit file ID of an open handle (GetFileInformationByHandle).
bool QueryFileId(HANDLE handle, std::uint64_t& id);

}  // namespace dirsize
