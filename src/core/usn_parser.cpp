#include "core/usn_parser.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>

namespace dirsize {

UsnParseStatus ParseUsnBuffer(const std::byte* data, size_t size, std::int64_t& nextUsn,
                              const std::function<void(const UsnChange&)>& onChange) {
    if (data == nullptr || size < sizeof(USN)) return UsnParseStatus::Malformed;
    USN next = 0;
    std::memcpy(&next, data, sizeof(next));
    nextUsn = next;
    size_t offset = sizeof(USN);
    constexpr size_t kFixed = offsetof(USN_RECORD_V2, FileName);
    while (offset < size) {
        const size_t remaining = size - offset;
        USN_RECORD_COMMON_HEADER header{};
        if (remaining < sizeof(header)) return UsnParseStatus::Malformed;
        std::memcpy(&header, data + offset, sizeof(header));
        if (header.RecordLength < kFixed || header.RecordLength > remaining || header.RecordLength % 8 != 0 ||
            header.MajorVersion != 2) {
            return UsnParseStatus::Malformed;
        }
        USN_RECORD_V2 r{};
        std::memcpy(&r, data + offset, kFixed);
        if (r.FileNameOffset < kFixed || static_cast<size_t>(r.FileNameOffset) + r.FileNameLength > r.RecordLength ||
            r.FileNameLength % 2 != 0) {
            return UsnParseStatus::Malformed;
        }
        UsnChange c;
        c.id = r.FileReferenceNumber;
        c.parentId = r.ParentFileReferenceNumber;
        c.usn = r.Usn;
        c.reason = r.Reason;
        c.attributes = r.FileAttributes;
        onChange(c);
        offset += header.RecordLength;
    }
    return UsnParseStatus::Ok;
}

std::vector<ChangedId> ChangeCollector::Take() {
    std::vector<ChangedId> out;
    out.reserve(reasons_.size());
    for (const auto& [id, why] : reasons_) out.push_back({id, why});
    reasons_.clear();
    std::sort(out.begin(), out.end(), [](const ChangedId& a, const ChangedId& b) { return a.id < b.id; });
    return out;
}

void OpenFileTracker::Observe(const UsnChange& c) {
    if (c.attributes & FILE_ATTRIBUTE_DIRECTORY) return;  // folder totals come from their files
    if ((c.reason & USN_REASON_CLOSE) || (c.reason & USN_REASON_FILE_DELETE)) {
        open_.erase(c.id);
    } else {
        open_[c.id] = true;
    }
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> RecordRanges(const std::vector<std::uint64_t>& ids) {
    std::vector<std::uint64_t> records;
    records.reserve(ids.size());
    for (std::uint64_t id : ids) records.push_back(id & kRecordNumberMask);
    std::sort(records.begin(), records.end());
    records.erase(std::unique(records.begin(), records.end()), records.end());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    for (std::uint64_t r : records) {
        if (!ranges.empty() && ranges.back().second + 1 == r) {
            ranges.back().second = r;
        } else {
            ranges.push_back({r, r});
        }
    }
    return ranges;
}

}  // namespace dirsize
