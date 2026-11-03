#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dirsize {

struct SizeTotals {
    std::uint64_t logicalBytes = 0;    // sum of file lengths (EndOfFile)
    std::uint64_t allocatedBytes = 0;  // sum of space on disk (AllocationSize)
    std::uint64_t files = 0;
    std::uint64_t folders = 0;
};

struct ChildFolder {
    std::wstring name;
    SizeTotals totals;  // includes the folder itself in `folders`
};

struct WalkResult {
    SizeTotals totals;                  // everything under the root
    std::vector<ChildFolder> children;  // immediate sub-folders of the root
    std::uint64_t accessDenied = 0;     // folders that could not be opened or read
    std::uint64_t otherErrors = 0;
    std::uint64_t reparsePointsSkipped = 0;  // junctions / directory symlinks
    bool rootOpened = false;
    unsigned long rootError = 0;  // Win32 error if the root could not be opened
};

// Walks the folder tree under `extendedRoot` (an extended-length path, see
// ToExtendedPath) using directory enumeration. Works on any file system.
//
// - Directory reparse points (junctions, symlinks) are not followed, so data
//   is never counted twice through a link.
// - Hard-linked files are counted once per link.
WalkResult WalkTree(const std::wstring& extendedRoot);

}  // namespace dirsize
