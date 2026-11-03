#pragma once

// Builds a VolumeIndex from parsed FSCTL_QUERY_FILE_LAYOUT entries. Pure
// logic: the caller supplies the root ID, the IDs that a listing of the root
// folder returns, and a way to classify directory reparse points.

#include <cstdint>
#include <functional>
#include <vector>

#include "core/index.h"
#include "core/layout_parser.h"

namespace dirsize {

class LayoutIndexBuilder {
public:
    // Stores what the index needs from one entry; names other than folder
    // names are not kept.
    void Add(const LayoutEntry& entry);

    // `rootListingIds`: file IDs returned by listing the root folder. Root
    // children missing from it (NTFS metafiles, $Extend) are excluded with
    // everything below them.
    // `readReparseTag(id, tag)`: called for each visible directory reparse
    // point; returns false when the tag cannot be read. Name surrogates
    // (junctions, symlinks, mount points) and unreadable tags are not
    // followed.
    VolumeIndex Finish(std::uint64_t rootId, std::vector<std::uint64_t> rootListingIds,
                       const std::function<bool(std::uint64_t id, std::uint32_t& tag)>& readReparseTag);

private:
    struct RawDir {
        std::uint64_t id = 0;
        std::uint64_t parentId = 0;
        std::uint32_t nameOffset = 0;
        std::uint16_t nameLength = 0;
        bool reparse = false;
        bool hasParent = false;
    };
    struct RawFile {
        std::uint64_t id = 0;
        std::uint64_t logicalBytes = 0;
        std::uint64_t allocatedBytes = 0;
        std::uint32_t firstLink = 0;  // into links_
        std::uint16_t linkCount = 0;
        bool hasNamedStreams = false;
    };
    struct RawNamed {
        std::uint32_t file = 0;  // index into files_
        std::uint32_t count = 0;
        std::uint64_t logicalBytes = 0;
        std::uint64_t allocatedBytes = 0;
    };

    std::vector<RawDir> dirs_;
    std::vector<RawFile> files_;
    std::vector<std::uint64_t> links_;  // parent IDs of file links
    std::vector<RawNamed> named_;
    std::vector<wchar_t> names_;
    std::uint64_t malformed_ = 0;
};

}  // namespace dirsize
