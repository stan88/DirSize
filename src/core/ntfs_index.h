#pragma once

#include <cstdint>

#include "core/index.h"
#include "core/layout_parser.h"

namespace dirsize {

struct NtfsIndexResult {
    bool ok = false;
    unsigned long error = 0;     // Win32 error of the failed step
    const wchar_t* failedStep = L"";
    VolumeIndex index;
    LayoutParseStats parse;
    std::uint64_t layoutCalls = 0;
    std::uint64_t rootListed = 0;           // items in the root listing
    unsigned long endError = 0;             // how enumeration ended: ERROR_HANDLE_EOF, or 0 = empty buffer
    double enumerateSeconds = 0;            // FSCTL_QUERY_FILE_LAYOUT loop
    double finishSeconds = 0;               // sorting, linking and roll-up
};

// Builds a whole-volume index of an NTFS volume with FSCTL_QUERY_FILE_LAYOUT.
// Needs an elevated process (the volume is opened with GENERIC_READ).
// The USN journal position is recorded before enumeration starts.
NtfsIndexResult BuildNtfsIndex(wchar_t driveLetter);

}  // namespace dirsize
