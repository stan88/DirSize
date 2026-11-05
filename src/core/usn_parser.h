#pragma once

// Parser for FSCTL_READ_USN_JOURNAL output buffers, and the ID helpers used
// to re-query changed files. Pure logic, no I/O.
//
// Buffer layout (Learn, "Walking a Buffer of Change Journal Records"): the
// first 8 bytes are the USN to start the next read from, followed by
// records advanced by RecordLength. Records are requested as version 2 only
// (USN_RECORD_V2, SDK 10.0.26100.0 winioctl.h); anything else is malformed.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/live_index.h"

namespace dirsize {

struct UsnChange {
    std::uint64_t id = 0;
    std::uint64_t parentId = 0;
    std::int64_t usn = 0;
    std::uint32_t reason = 0;
    std::uint32_t attributes = 0;
};

enum class UsnParseStatus {
    Ok,
    Malformed,  // bad header, bad RecordLength, name past the record, or not version 2
};

// Parses one buffer (`size` = bytes returned). `nextUsn` receives the USN in
// the first 8 bytes. Calls `onChange` for each valid record before the first
// malformed one.
UsnParseStatus ParseUsnBuffer(const std::byte* data, size_t size, std::int64_t& nextUsn,
                              const std::function<void(const UsnChange&)>& onChange);

// Collects changed IDs with the OR of their reasons.
class ChangeCollector {
public:
    void Add(const UsnChange& c) { reasons_[c.id] |= c.reason; }
    size_t Size() const { return reasons_.size(); }
    std::vector<ChangedId> Take();

private:
    std::unordered_map<std::uint64_t, std::uint32_t> reasons_;
};

// Files seen in the journal without a later USN_REASON_CLOSE record. While a
// file stays open, NTFS writes a record only the first time each reason is
// set (Learn, READ_USN_JOURNAL_DATA_V1 and USN_RECORD_V2: reasons accumulate
// from open to close), so further growth of an open log file produces no
// records until it closes. These IDs are re-queried on every catch-up.
class OpenFileTracker {
public:
    void Observe(const UsnChange& c);
    void Forget(std::uint64_t id) { open_.erase(id); }
    void Track(std::uint64_t id) { open_[id] = true; }  // e.g. restored from a cache
    const std::unordered_map<std::uint64_t, bool>& Ids() const { return open_; }
    size_t Size() const { return open_.size(); }

private:
    std::unordered_map<std::uint64_t, bool> open_;  // value unused
};

// The MFT record number of a file ID (low 48 bits; the high 16 bits are the
// sequence number). The FILEID layout filter matches record numbers only.
constexpr std::uint64_t kRecordNumberMask = 0x0000FFFFFFFFFFFFull;

// Inclusive, non-overlapping ranges of record numbers covering `ids`:
// sequence numbers stripped, duplicates removed, consecutive numbers merged.
std::vector<std::pair<std::uint64_t, std::uint64_t>> RecordRanges(const std::vector<std::uint64_t>& ids);

}  // namespace dirsize
