#pragma once

// Parser for FSCTL_QUERY_FILE_LAYOUT output buffers. Pure parsing, no I/O.
//
// Structures come from the Windows SDK header winioctl.h (10.0.26100.0).
// Learn documents only the control code and QUERY_FILE_LAYOUT_INPUT/OUTPUT.
// NTFS returns STREAM_LAYOUT_ENTRY version 1 while the SDK defines version 2;
// the fields used here sit at the same offsets in both, which the Phase 2
// probe verified against OpenFileById + FileStreamInfo (docs/DEVLOG.md).
//
// Every read is bounds-checked against the buffer. A malformed file entry is
// reported with `malformed = true` and its sizes must not be trusted.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace dirsize {

// NTFS attribute type code of data streams ($DATA). The unnamed data stream
// has an empty stream identifier; named streams have their name there.
constexpr std::uint32_t kDataAttributeTypeCode = 0x80;

struct LayoutLink {
    std::uint64_t parentId = 0;
    std::wstring_view name;  // points into the parsed buffer
};

struct LayoutEntry {
    std::uint64_t id = 0;
    std::uint32_t attributes = 0;
    std::vector<LayoutLink> links;  // DOS-only (8.3) aliases are skipped
    bool hasData = false;           // an unnamed data stream was found
    std::uint64_t dataLogicalBytes = 0;
    std::uint64_t dataAllocatedBytes = 0;
    std::uint32_t namedStreams = 0;
    std::uint64_t namedLogicalBytes = 0;
    std::uint64_t namedAllocatedBytes = 0;
    bool malformed = false;
};

struct LayoutParseStats {
    std::uint64_t buffers = 0;
    std::uint64_t badHeaders = 0;
    std::uint64_t entries = 0;
    std::uint64_t malformedEntries = 0;
    std::uint64_t dosOnlyNames = 0;
    std::uint64_t unknownFileVersions = 0;
    std::uint64_t unknownStreamVersions = 0;
    std::uint64_t truncatedChains = 0;  // fewer entries than FileEntryCount
};

using LayoutEntryCallback = std::function<void(const LayoutEntry&)>;

// Parses one output buffer (`size` = bytes returned by DeviceIoControl).
// Calls `onEntry` for each file entry. Returns the FileEntryCount from the
// header (0 = end of enumeration), or 0 when the header is malformed.
std::uint32_t ParseLayoutBuffer(const std::byte* data, size_t size, LayoutParseStats& stats,
                                const LayoutEntryCallback& onEntry);

}  // namespace dirsize
