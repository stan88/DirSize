#include "core/dir_listing.h"

namespace dirsize {
namespace {

constexpr size_t kBufferBytes = 64 * 1024;

bool IsDotOrDotDot(std::wstring_view name) {
    return name == L"." || name == L"..";
}

// Walks one buffer of FILE_ID_BOTH_DIR_INFO or FILE_FULL_DIR_INFO records.
// `returned` bounds are not reported by GetFileInformationByHandleEx, so the
// walk relies on NextEntryOffset and the buffer size.
template <typename Info, bool kHasId>
void WalkBuffer(const std::vector<ULONGLONG>& buffer, const std::function<void(const ListedItem&)>& onItem) {
    const auto* base = reinterpret_cast<const BYTE*>(buffer.data());
    const size_t size = buffer.size() * sizeof(ULONGLONG);
    size_t offset = 0;
    for (;;) {
        if (offset + offsetof(Info, FileName) > size) {
            return;
        }
        const auto* info = reinterpret_cast<const Info*>(base + offset);
        const size_t nameBytes = info->FileNameLength;
        if (offset + offsetof(Info, FileName) + nameBytes > size) {
            return;
        }
        ListedItem item;
        item.name = std::wstring_view(info->FileName, nameBytes / sizeof(wchar_t));
        if (!IsDotOrDotDot(item.name)) {
            if constexpr (kHasId) {
                item.id = static_cast<std::uint64_t>(info->FileId.QuadPart);
                item.hasId = true;
            }
            item.attributes = info->FileAttributes;
            item.logicalBytes = static_cast<std::uint64_t>(info->EndOfFile.QuadPart);
            item.allocatedBytes = static_cast<std::uint64_t>(info->AllocationSize.QuadPart);
            onItem(item);
        }
        if (info->NextEntryOffset == 0) {
            return;
        }
        offset += info->NextEntryOffset;
    }
}

}  // namespace

DirectoryLister::DirectoryLister() : buffer_(kBufferBytes / sizeof(ULONGLONG)) {}

DWORD DirectoryLister::List(HANDLE dir, const std::function<void(const ListedItem&)>& onItem) {
    const auto bytes = static_cast<DWORD>(buffer_.size() * sizeof(ULONGLONG));
    bool first = true;
    for (;;) {
        const FILE_INFO_BY_HANDLE_CLASS cls = useIds_ ? FileIdBothDirectoryInfo : FileFullDirectoryInfo;
        if (!GetFileInformationByHandleEx(dir, cls, buffer_.data(), bytes)) {
            const DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_FILES) {
                return ERROR_SUCCESS;
            }
            if (first && useIds_ &&
                (error == ERROR_INVALID_PARAMETER || error == ERROR_INVALID_LEVEL || error == ERROR_NOT_SUPPORTED)) {
                useIds_ = false;  // file system without file IDs in listings; retry this folder
                continue;
            }
            return error;
        }
        first = false;
        if (useIds_) {
            WalkBuffer<FILE_ID_BOTH_DIR_INFO, true>(buffer_, onItem);
        } else {
            WalkBuffer<FILE_FULL_DIR_INFO, false>(buffer_, onItem);
        }
    }
}

HANDLE OpenDirectoryForListing(const wchar_t* path) {
    return CreateFileW(path, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

bool QueryReparseTag(HANDLE handle, DWORD& tag) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info))) {
        return false;
    }
    tag = info.ReparseTag;
    return true;
}

bool QueryFileId(HANDLE handle, std::uint64_t& id) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        return false;
    }
    id = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    return true;
}

}  // namespace dirsize
