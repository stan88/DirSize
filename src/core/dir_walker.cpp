#include "core/dir_walker.h"

#include <windows.h>

#include <string_view>

#include "core/path_util.h"

namespace dirsize {
namespace {

// Large buffer so each GetFileInformationByHandleEx call returns many entries.
// ULONGLONG elements keep FILE_FULL_DIR_INFO records 8-byte aligned.
constexpr size_t kBufferBytes = 64 * 1024;

struct PendingFolder {
    std::wstring path;
    int childIndex;  // index into WalkResult::children, or -1 for the root
};

bool IsDotOrDotDot(std::wstring_view name) {
    return name == L"." || name == L"..";
}

void CountError(WalkResult& result, DWORD error) {
    if (error == ERROR_ACCESS_DENIED) {
        ++result.accessDenied;
    } else {
        ++result.otherErrors;
    }
}

}  // namespace

WalkResult WalkTree(const std::wstring& extendedRoot, const std::atomic<bool>* cancel) {
    WalkResult result;
    std::vector<ULONGLONG> buffer(kBufferBytes / sizeof(ULONGLONG));
    const DWORD bufferBytes = static_cast<DWORD>(buffer.size() * sizeof(ULONGLONG));

    // Explicit stack instead of recursion: deep trees cannot overflow the call stack.
    std::vector<PendingFolder> pending;
    pending.push_back({extendedRoot, -1});
    bool isRoot = true;

    while (!pending.empty()) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            return result;
        }
        PendingFolder folder = std::move(pending.back());
        pending.pop_back();

        HANDLE dir = CreateFileW(folder.path.c_str(), FILE_LIST_DIRECTORY,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (dir == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (isRoot) {
                result.rootError = error;
                return result;
            }
            CountError(result, error);
            continue;
        }
        if (isRoot) {
            result.rootOpened = true;
            isRoot = false;
        }

        for (;;) {
            if (!GetFileInformationByHandleEx(dir, FileFullDirectoryInfo, buffer.data(), bufferBytes)) {
                DWORD error = GetLastError();
                if (error != ERROR_NO_MORE_FILES) {
                    CountError(result, error);
                }
                break;
            }

            const auto* entry = reinterpret_cast<const FILE_FULL_DIR_INFO*>(buffer.data());
            for (;;) {
                std::wstring_view name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
                const DWORD attributes = entry->FileAttributes;

                if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (IsDotOrDotDot(name)) {
                        // nothing to count
                    } else if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                        ++result.reparsePointsSkipped;
                    } else {
                        int childIndex = folder.childIndex;
                        if (childIndex < 0) {
                            childIndex = static_cast<int>(result.children.size());
                            result.children.push_back({std::wstring(name), {}});
                        }
                        ++result.totals.folders;
                        ++result.children[childIndex].totals.folders;
                        pending.push_back({JoinPath(folder.path, name), childIndex});
                    }
                } else {
                    const auto logical = static_cast<std::uint64_t>(entry->EndOfFile.QuadPart);
                    const auto allocated = static_cast<std::uint64_t>(entry->AllocationSize.QuadPart);
                    result.totals.logicalBytes += logical;
                    result.totals.allocatedBytes += allocated;
                    ++result.totals.files;
                    if (folder.childIndex >= 0) {
                        SizeTotals& child = result.children[folder.childIndex].totals;
                        child.logicalBytes += logical;
                        child.allocatedBytes += allocated;
                        ++child.files;
                    }
                }

                if (entry->NextEntryOffset == 0) {
                    break;
                }
                entry = reinterpret_cast<const FILE_FULL_DIR_INFO*>(
                    reinterpret_cast<const BYTE*>(entry) + entry->NextEntryOffset);
            }
        }

        CloseHandle(dir);
    }

    return result;
}

}  // namespace dirsize
