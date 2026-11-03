#pragma once

#include <string>

#include "core/index.h"

namespace dirsize {

struct WalkIndexResult {
    bool rootOpened = false;
    unsigned long rootError = 0;
    VolumeIndex index;
};

// Builds a folder index by listing every folder under `extendedRoot` (an
// extended-length path, see ToExtendedPath). Works on any file system and
// needs no elevation. Folders are linked to their parents structurally, not
// by file ID, because IDs are not reliable on every file system (FAT, ReFS).
// Folders appear in discovery order (VolumeIndex::sortedById is false).
WalkIndexResult BuildIndexByWalking(const std::wstring& extendedRoot);

}  // namespace dirsize
