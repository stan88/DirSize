#include "core/layout_parser.h"

#include <windows.h>
#include <winioctl.h>

#include <cstring>

namespace dirsize {
namespace {

constexpr size_t kNameHeader = offsetof(FILE_LAYOUT_NAME_ENTRY, FileName);
constexpr size_t kStreamHeader = offsetof(STREAM_LAYOUT_ENTRY, StreamIdentifier);
constexpr size_t kFileHeader = sizeof(FILE_LAYOUT_ENTRY);
// winioctl.h: "current version number is 1" (no named constant is defined).
constexpr DWORD kFileLayoutEntryVersion = 1;

// Reads a fixed-size header with memcpy so an unaligned offset in a bad
// buffer cannot fault.
template <typename T>
T ReadHeader(const std::byte* p, size_t bytes) {
    T value{};
    std::memcpy(&value, p, bytes);
    return value;
}

// Offset `offset` from `base` lands inside [base, end) with `need` bytes.
bool Fits(const std::byte* base, const std::byte* end, std::uint64_t offset, size_t need) {
    const auto room = static_cast<std::uint64_t>(end - base);
    return offset <= room && need <= room - offset;
}

void ParseNames(const std::byte* entry, const std::byte* entryEnd, DWORD firstOffset, LayoutEntry& out,
                LayoutParseStats& stats) {
    std::uint64_t offset = firstOffset;
    for (;;) {
        if (!Fits(entry, entryEnd, offset, kNameHeader)) {
            out.malformed = true;
            return;
        }
        const std::byte* p = entry + offset;
        const auto name = ReadHeader<FILE_LAYOUT_NAME_ENTRY>(p, kNameHeader);
        const std::byte* nameStart = p + kNameHeader;
        if (name.FileNameLength % sizeof(wchar_t) != 0 ||
            !Fits(entry, entryEnd, offset + kNameHeader, name.FileNameLength) ||
            reinterpret_cast<std::uintptr_t>(nameStart) % alignof(wchar_t) != 0) {
            out.malformed = true;
            return;
        }
        if (name.Flags == FILE_LAYOUT_NAME_ENTRY_DOS) {
            ++stats.dosOnlyNames;  // 8.3 alias of a long name; the long name is listed too
        } else {
            LayoutLink link;
            link.parentId = name.ParentFileReferenceNumber;
            link.name = std::wstring_view(reinterpret_cast<const wchar_t*>(nameStart),
                                          name.FileNameLength / sizeof(wchar_t));
            out.links.push_back(link);
        }
        if (name.NextNameOffset == 0) {
            return;
        }
        if (name.NextNameOffset < kNameHeader) {
            out.malformed = true;  // would not advance past this header
            return;
        }
        offset += name.NextNameOffset;
    }
}

void ParseStreams(const std::byte* entry, const std::byte* entryEnd, DWORD firstOffset, LayoutEntry& out,
                  LayoutParseStats& stats) {
    std::uint64_t offset = firstOffset;
    for (;;) {
        if (!Fits(entry, entryEnd, offset, kStreamHeader)) {
            out.malformed = true;
            return;
        }
        const std::byte* p = entry + offset;
        const auto stream = ReadHeader<STREAM_LAYOUT_ENTRY>(p, kStreamHeader);
        if (stream.Version != 1 && stream.Version != 2) {
            ++stats.unknownStreamVersions;
            out.malformed = true;
            return;
        }
        if (!Fits(entry, entryEnd, offset + kStreamHeader, stream.StreamIdentifierLength) ||
            stream.EndOfFile.QuadPart < 0 || stream.AllocationSize.QuadPart < 0) {
            out.malformed = true;
            return;
        }
        if (stream.AttributeTypeCode == kDataAttributeTypeCode) {
            const auto logical = static_cast<std::uint64_t>(stream.EndOfFile.QuadPart);
            const auto allocated = static_cast<std::uint64_t>(stream.AllocationSize.QuadPart);
            if (stream.StreamIdentifierLength == 0) {
                if (out.hasData) {
                    out.malformed = true;  // two unnamed data streams
                    return;
                }
                out.hasData = true;
                out.dataLogicalBytes = logical;
                out.dataAllocatedBytes = allocated;
            } else {
                ++out.namedStreams;
                out.namedLogicalBytes += logical;
                out.namedAllocatedBytes += allocated;
            }
        }
        if (stream.NextStreamOffset == 0) {
            return;
        }
        if (stream.NextStreamOffset < kStreamHeader) {
            out.malformed = true;
            return;
        }
        offset += stream.NextStreamOffset;
    }
}

}  // namespace

std::uint32_t ParseLayoutBuffer(const std::byte* data, size_t size, LayoutParseStats& stats,
                                const LayoutEntryCallback& onEntry) {
    ++stats.buffers;
    const std::byte* end = data + size;
    if (size < sizeof(QUERY_FILE_LAYOUT_OUTPUT)) {
        ++stats.badHeaders;
        return 0;
    }
    const auto header = ReadHeader<QUERY_FILE_LAYOUT_OUTPUT>(data, sizeof(QUERY_FILE_LAYOUT_OUTPUT));
    if (header.FileEntryCount == 0) {
        return 0;
    }

    LayoutEntry entry;
    std::uint64_t offset = header.FirstFileOffset;
    DWORD parsed = 0;
    while (parsed < header.FileEntryCount) {
        if (!Fits(data, end, offset, kFileHeader)) {
            break;
        }
        const std::byte* p = data + offset;
        const auto file = ReadHeader<FILE_LAYOUT_ENTRY>(p, kFileHeader);
        // The entry ends at the next entry, or at the end of the buffer.
        const std::byte* entryEnd = end;
        if (file.NextFileOffset != 0) {
            if (file.NextFileOffset < kFileHeader || !Fits(data, end, offset, file.NextFileOffset)) {
                break;  // cannot find the next entry; stop this buffer
            }
            entryEnd = p + file.NextFileOffset;
        }

        entry.id = file.FileReferenceNumber;
        entry.attributes = file.FileAttributes;
        entry.links.clear();
        entry.hasData = false;
        entry.dataLogicalBytes = entry.dataAllocatedBytes = 0;
        entry.namedStreams = 0;
        entry.namedLogicalBytes = entry.namedAllocatedBytes = 0;
        entry.malformed = false;

        if (file.Version != kFileLayoutEntryVersion) {
            ++stats.unknownFileVersions;
            entry.malformed = true;
        } else {
            if (file.FirstNameOffset != 0) {
                ParseNames(p, entryEnd, file.FirstNameOffset, entry, stats);
            }
            if (file.FirstStreamOffset != 0) {
                ParseStreams(p, entryEnd, file.FirstStreamOffset, entry, stats);
            }
        }
        ++stats.entries;
        if (entry.malformed) {
            ++stats.malformedEntries;
        }
        onEntry(entry);
        ++parsed;
        if (file.NextFileOffset == 0) {
            break;
        }
        offset += file.NextFileOffset;
    }
    if (parsed < header.FileEntryCount) {
        ++stats.truncatedChains;
    }
    return header.FileEntryCount;
}

}  // namespace dirsize
