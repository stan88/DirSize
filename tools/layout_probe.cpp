// layout-probe: Phase 2 feasibility probe for FSCTL_QUERY_FILE_LAYOUT.
//
// Enumerates a whole NTFS volume with FSCTL_QUERY_FILE_LAYOUT and checks what
// the returned entries contain. Measurement code only; not part of the core.
//
// The SDK header (10.0.26100.0 um/winioctl.h) defines STREAM_LAYOUT_ENTRY
// version 2 only, but NTFS returns version 1. The probe therefore decodes
// stream entries with the version-2 field offsets *as a hypothesis* and
// verifies them against an oracle built from documented APIs:
// OpenFileById + GetFileInformationByHandleEx(FileStreamInfo). Folder totals
// are printed only when the hypothesis matches the oracle.
//
// Usage: layout-probe <drive letter>      e.g. layout-probe D

#include <windows.h>
#include <winioctl.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// ---- output -----------------------------------------------------------------

void Write(std::wstring_view text) {
    if (text.empty()) {
        return;
    }
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                    nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), bytes, nullptr,
                        nullptr);
    std::fwrite(utf8.data(), 1, utf8.size(), stdout);
}

void Line(const wchar_t* format, ...) {
    wchar_t buffer[2048];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    Write(buffer);
    Write(L"\n");
}

std::wstring Printable(std::wstring_view text) {
    std::wstring out;
    for (wchar_t c : text) {
        out.push_back(c < 0x20 ? L'?' : c);
    }
    return out;
}

// ---- environment ------------------------------------------------------------

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                    elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

bool EnableBackupPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool enabled = false;
    if (LookupPrivilegeValueW(nullptr, SE_BACKUP_NAME, &privileges.Privileges[0].Luid) &&
        AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr)) {
        enabled = GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(token);
    return enabled;
}

std::uint64_t PeakWorkingSetBytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return counters.PeakWorkingSetSize;
    }
    return 0;
}

std::uint32_t Hash(const BYTE* data, size_t size) {  // FNV-1a
    std::uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        h = (h ^ data[i]) * 16777619u;
    }
    return h;
}

// ---- decoded data -----------------------------------------------------------

struct Link {
    std::uint64_t parent = 0;
    std::wstring name;  // kept only when the parent is the root
};

// One stream entry decoded with the version-2 offsets from the SDK header.
struct Stream {
    DWORD version = 0;
    DWORD flags = 0;
    DWORD code = 0;
    std::uint32_t idHash = 0;
    DWORD idLength = 0;
    std::uint64_t endOfFile = 0;
    std::uint64_t allocation = 0;
};

struct FileRecord {
    DWORD attributes = 0;
    std::vector<Link> links;  // name entries except DOS-only aliases
    std::vector<Stream> streams;
};

struct Sample {
    std::uint64_t id = 0;
    const wchar_t* reason = L"";
    std::vector<BYTE> raw;  // bytes from the first stream entry to the end of the file entry
};

struct Sizes {
    std::uint64_t count = 0;
    std::uint64_t endOfFile = 0;
    std::uint64_t allocation = 0;
    void Add(std::uint64_t eof, std::uint64_t alloc) {
        ++count;
        endOfFile += eof;
        allocation += alloc;
    }
};

constexpr size_t kIdOffset = offsetof(STREAM_LAYOUT_ENTRY, StreamIdentifier);

// Decodes a stream chain. Bounds-checked so a wrong layout cannot crash.
std::vector<Stream> DecodeStreams(const BYTE* first, const BYTE* end, std::map<DWORD, std::uint64_t>& versions,
                                  std::uint64_t& badBounds) {
    std::vector<Stream> out;
    const BYTE* p = first;
    for (int guard = 0; guard < 4096; ++guard) {
        if (p + kIdOffset > end) {
            ++badBounds;
            break;
        }
        const auto* s = reinterpret_cast<const STREAM_LAYOUT_ENTRY*>(p);
        ++versions[s->Version];
        Stream d;
        d.version = s->Version;
        d.flags = s->Flags;
        d.code = s->AttributeTypeCode;
        d.endOfFile = static_cast<std::uint64_t>(s->EndOfFile.QuadPart);
        d.allocation = static_cast<std::uint64_t>(s->AllocationSize.QuadPart);
        d.idLength = s->StreamIdentifierLength;
        const BYTE* idEnd = p + kIdOffset + s->StreamIdentifierLength;
        const BYTE* limit = s->NextStreamOffset ? p + s->NextStreamOffset : end;
        if (idEnd > limit || idEnd > end) {
            ++badBounds;
            d.idHash = 0xFFFFFFFFu;
        } else {
            d.idHash = Hash(p + kIdOffset, s->StreamIdentifierLength);
        }
        out.push_back(d);
        if (s->NextStreamOffset == 0) break;
        p += s->NextStreamOffset;
    }
    return out;
}

// ---- volume enumeration ----------------------------------------------------

using EntryCallback = std::function<void(const FILE_LAYOUT_ENTRY*, const BYTE* entryEnd)>;

struct EnumResult {
    DWORD endError = 0;
    std::uint64_t calls = 0;
    std::uint64_t entries = 0;
    DWORD outputFlags = 0;
    std::map<DWORD, std::uint64_t> fileVersions;
};

EnumResult Enumerate(HANDLE volume, DWORD flags, BYTE* buffer, DWORD bufferSize, const EntryCallback& onEntry) {
    EnumResult r;
    QUERY_FILE_LAYOUT_INPUT input{};
    input.FilterType = QUERY_FILE_LAYOUT_FILTER_TYPE_NONE;
    for (bool first = true;; first = false) {
        input.Flags = flags | (first ? QUERY_FILE_LAYOUT_RESTART : 0);
        DWORD returned = 0;
        if (!DeviceIoControl(volume, FSCTL_QUERY_FILE_LAYOUT, &input, sizeof(input), buffer, bufferSize, &returned,
                             nullptr)) {
            r.endError = GetLastError();
            break;
        }
        ++r.calls;
        const BYTE* end = buffer + returned;
        const auto* header = reinterpret_cast<const QUERY_FILE_LAYOUT_OUTPUT*>(buffer);
        r.outputFlags |= header->Flags;
        if (header->FileEntryCount == 0) {
            break;
        }
        const BYTE* p = buffer + header->FirstFileOffset;
        for (DWORD i = 0; i < header->FileEntryCount && p + sizeof(FILE_LAYOUT_ENTRY) <= end; ++i) {
            const auto* entry = reinterpret_cast<const FILE_LAYOUT_ENTRY*>(p);
            ++r.entries;
            ++r.fileVersions[entry->Version];
            const BYTE* entryEnd = entry->NextFileOffset ? p + entry->NextFileOffset : end;
            onEntry(entry, std::min(entryEnd, end));
            if (entry->NextFileOffset == 0) break;
            p += entry->NextFileOffset;
        }
    }
    return r;
}

void PrintVersions(const wchar_t* label, const std::map<DWORD, std::uint64_t>& versions) {
    std::wstring text;
    for (const auto& [v, n] : versions) {
        wchar_t part[64];
        swprintf_s(part, L" v%lu=%llu", v, n);
        text += part;
    }
    Line(L"%s:%s", label, text.empty() ? L" (none)" : text.c_str());
}

// ---- oracle -----------------------------------------------------------------

struct OracleStream {
    std::wstring name;
    std::uint64_t size = 0;
    std::uint64_t allocation = 0;
};

bool QueryOracle(HANDLE hint, std::uint64_t id, std::vector<OracleStream>& out, DWORD& error) {
    FILE_ID_DESCRIPTOR descriptor{};
    descriptor.dwSize = sizeof(descriptor);
    descriptor.Type = FileIdType;
    descriptor.FileId.QuadPart = static_cast<LONGLONG>(id);
    HANDLE h = OpenFileById(hint, &descriptor, FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
    if (h == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    alignas(8) static BYTE info[64 * 1024];
    bool ok = GetFileInformationByHandleEx(h, FileStreamInfo, info, sizeof(info)) != FALSE;
    error = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok) {
        // ERROR_HANDLE_EOF here means the file has no streams.
        return error == ERROR_HANDLE_EOF;
    }
    const BYTE* p = info;
    for (;;) {
        const auto* s = reinterpret_cast<const FILE_STREAM_INFO*>(p);
        OracleStream o;
        o.name.assign(s->StreamName, s->StreamNameLength / sizeof(WCHAR));
        o.size = static_cast<std::uint64_t>(s->StreamSize.QuadPart);
        o.allocation = static_cast<std::uint64_t>(s->StreamAllocationSize.QuadPart);
        out.push_back(std::move(o));
        if (s->NextEntryOffset == 0) break;
        p += s->NextEntryOffset;
    }
    return true;
}

void HexDump(const std::vector<BYTE>& raw, size_t limit) {
    const size_t n = std::min(raw.size(), limit);
    for (size_t row = 0; row < n; row += 16) {
        std::wstring text;
        wchar_t part[32];
        swprintf_s(part, L"    %04zx:", row);
        text += part;
        for (size_t i = row; i < row + 16 && i < n; ++i) {
            swprintf_s(part, L" %02x", raw[i]);
            text += part;
        }
        Line(L"%s", text.c_str());
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || !iswalpha(argv[1][0])) {
        Line(L"usage: layout-probe <drive letter>");
        return 2;
    }
    const wchar_t drive = static_cast<wchar_t>(towupper(argv[1][0]));

    const bool elevated = IsElevated();
    const bool backup = EnableBackupPrivilege();
    Line(L"Drive            : %c:", drive);
    Line(L"Elevated         : %s", elevated ? L"yes" : L"no");
    Line(L"Backup privilege : %s", backup ? L"enabled" : L"not held");

    std::wstring rootPath = std::wstring(1, drive) + L":\\";
    HANDLE rootHandle = CreateFileW(rootPath.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (rootHandle == INVALID_HANDLE_VALUE) {
        Line(L"Cannot open %s: error %lu", rootPath.c_str(), GetLastError());
        return 1;
    }
    BY_HANDLE_FILE_INFORMATION rootInfo{};
    GetFileInformationByHandle(rootHandle, &rootInfo);
    const std::uint64_t rootId =
        (static_cast<std::uint64_t>(rootInfo.nFileIndexHigh) << 32) | rootInfo.nFileIndexLow;
    Line(L"Root file ID     : 0x%016llx", rootId);

    std::wstring volumePath = std::wstring(L"\\\\.\\") + drive + L":";
    HANDLE volume = CreateFileW(volumePath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0,
                                nullptr);
    if (volume == INVALID_HANDLE_VALUE) {
        Line(L"Volume open (GENERIC_READ) failed: error %lu", GetLastError());
        return 1;
    }

    constexpr DWORD kBufferSize = 4 * 1024 * 1024;
    auto* buffer = static_cast<BYTE*>(VirtualAlloc(nullptr, kBufferSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (buffer == nullptr) {
        Line(L"VirtualAlloc failed");
        return 1;
    }

    // ---- pass 1: names + streams, store everything -------------------------

    const DWORD pass1Flags = QUERY_FILE_LAYOUT_INCLUDE_NAMES | QUERY_FILE_LAYOUT_INCLUDE_STREAMS |
                             QUERY_FILE_LAYOUT_INCLUDE_STREAMS_WITH_NO_CLUSTERS_ALLOCATED;
    std::unordered_map<std::uint64_t, FileRecord> files;
    std::vector<Sample> samples;
    std::map<DWORD, std::uint64_t> streamVersions;
    std::uint64_t badBounds = 0, dosOnlyNames = 0, namelessFiles = 0, eligible = 0;
    int nFirst = 0, nSpread = 0, nMulti = 0, nCompressed = 0, nSparse = 0;

    const auto start = std::chrono::steady_clock::now();
    EnumResult pass1 = Enumerate(volume, pass1Flags, buffer, kBufferSize, [&](const FILE_LAYOUT_ENTRY* entry,
                                                                             const BYTE* entryEnd) {
        const BYTE* base = reinterpret_cast<const BYTE*>(entry);
        FileRecord& record = files[entry->FileReferenceNumber];
        record.attributes = entry->FileAttributes;

        if (entry->FirstNameOffset != 0) {
            const BYTE* p = base + entry->FirstNameOffset;
            for (int guard = 0; guard < 4096 && p + offsetof(FILE_LAYOUT_NAME_ENTRY, FileName) <= entryEnd;
                 ++guard) {
                const auto* name = reinterpret_cast<const FILE_LAYOUT_NAME_ENTRY*>(p);
                if (name->Flags == FILE_LAYOUT_NAME_ENTRY_DOS) {
                    ++dosOnlyNames;
                } else {
                    Link link;
                    link.parent = name->ParentFileReferenceNumber;
                    if (link.parent == rootId) {
                        link.name.assign(name->FileName, name->FileNameLength / sizeof(WCHAR));
                    }
                    record.links.push_back(std::move(link));
                }
                if (name->NextNameOffset == 0) break;
                p += name->NextNameOffset;
            }
        } else {
            ++namelessFiles;
        }

        if (entry->FirstStreamOffset != 0) {
            const BYTE* first = base + entry->FirstStreamOffset;
            record.streams = DecodeStreams(first, entryEnd, streamVersions, badBounds);

            const bool regular = !(entry->FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                                 !record.links.empty();
            if (regular) {
                ++eligible;
                const wchar_t* reason = nullptr;
                if (nFirst < 150) {
                    ++nFirst;
                    reason = L"first";
                } else if ((entry->FileAttributes & FILE_ATTRIBUTE_COMPRESSED) && nCompressed < 100) {
                    ++nCompressed;
                    reason = L"compressed";
                } else if ((entry->FileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) && nSparse < 30) {
                    ++nSparse;
                    reason = L"sparse";
                } else if (record.streams.size() >= 2 && nMulti < 100) {
                    ++nMulti;
                    reason = L"multi-stream";
                } else if (eligible % 4000 == 0 && nSpread < 150) {
                    ++nSpread;
                    reason = L"spread";
                }
                if (reason != nullptr && first < entryEnd) {
                    Sample s;
                    s.id = entry->FileReferenceNumber;
                    s.reason = reason;
                    s.raw.assign(first, entryEnd);
                    samples.push_back(std::move(s));
                }
            }
        }
    });
    const double enumSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const std::uint64_t peakAfterPass1 = PeakWorkingSetBytes();

    Line(L"");
    Line(L"== Pass 1 (NAMES | STREAMS | STREAMS_WITH_NO_CLUSTERS_ALLOCATED)");
    Line(L"Enumeration ended: error %lu%s", pass1.endError,
         pass1.endError == ERROR_HANDLE_EOF ? L" (ERROR_HANDLE_EOF)" : L"");
    Line(L"Output flags     : 0x%08lx (SINGLE_INSTANCED %s)", pass1.outputFlags,
         (pass1.outputFlags & QUERY_FILE_LAYOUT_SINGLE_INSTANCED) ? L"set" : L"not set");
    Line(L"Calls            : %llu", pass1.calls);
    Line(L"File entries     : %llu (unique IDs %zu)", pass1.entries, files.size());
    PrintVersions(L"FILE_LAYOUT_ENTRY versions  ", pass1.fileVersions);
    PrintVersions(L"STREAM_LAYOUT_ENTRY versions", streamVersions);
    Line(L"Stream bound errors (v2 offsets): %llu", badBounds);
    Line(L"DOS-only names   : %llu (skipped)", dosOnlyNames);
    Line(L"Files w/o names  : %llu", namelessFiles);
    Line(L"Enumerate time   : %.3f s", enumSeconds);
    Line(L"Peak working set after pass 1: %llu bytes", peakAfterPass1);
    const bool pass1Complete = pass1.endError == ERROR_HANDLE_EOF;
    if (!pass1Complete) {
        Line(L"Pass 1 did not reach ERROR_HANDLE_EOF; results are partial.");
    }

    // ---- pass 2: does STREAM_INFORMATION change the stream version? --------

    {
        const DWORD pass2Flags = pass1Flags | QUERY_FILE_LAYOUT_INCLUDE_STREAM_INFORMATION |
                                 QUERY_FILE_LAYOUT_INCLUDE_STREAM_INFORMATION_FOR_DATA_ATTRIBUTE;
        std::map<DWORD, std::uint64_t> versions2;
        std::uint64_t bad2 = 0;
        const auto t2 = std::chrono::steady_clock::now();
        EnumResult pass2 = Enumerate(volume, pass2Flags, buffer, kBufferSize,
                                     [&](const FILE_LAYOUT_ENTRY* entry, const BYTE* entryEnd) {
                                         if (entry->FirstStreamOffset != 0) {
                                             DecodeStreams(reinterpret_cast<const BYTE*>(entry) +
                                                               entry->FirstStreamOffset,
                                                           entryEnd, versions2, bad2);
                                         }
                                     });
        Line(L"");
        Line(L"== Pass 2 (pass 1 flags + STREAM_INFORMATION | ..._FOR_DATA_ATTRIBUTE), version check only");
        Line(L"Enumeration ended: error %lu, entries %llu, %.3f s", pass2.endError, pass2.entries,
             std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count());
        PrintVersions(L"STREAM_LAYOUT_ENTRY versions", versions2);
    }
    CloseHandle(volume);

    // ---- oracle check -----------------------------------------------------

    Line(L"");
    Line(L"== Oracle check (OpenFileById + FileStreamInfo) on %zu samples", samples.size());
    std::uint64_t oracleOk = 0, oracleFail = 0, matched = 0, mismatched = 0;
    std::map<std::pair<DWORD, std::uint32_t>, std::uint64_t> unnamedKeys, namedKeys;
    std::map<DWORD, std::uint64_t> openErrors;
    std::map<std::wstring, std::uint64_t> reasonMatched, reasonTotal;
    int mismatchShown = 0;
    for (const Sample& sample : samples) {
        std::vector<OracleStream> oracle;
        DWORD error = 0;
        if (!QueryOracle(rootHandle, sample.id, oracle, error)) {
            ++oracleFail;
            ++openErrors[error];
            continue;
        }
        ++oracleOk;
        ++reasonTotal[sample.reason];
        const std::vector<Stream>& decoded = files[sample.id].streams;
        std::vector<bool> used(decoded.size(), false);
        bool all = true;
        for (const OracleStream& o : oracle) {
            bool found = false;
            for (size_t i = 0; i < decoded.size(); ++i) {
                if (!used[i] && decoded[i].endOfFile == o.size && decoded[i].allocation == o.allocation) {
                    used[i] = true;
                    found = true;
                    auto key = std::make_pair(decoded[i].code, decoded[i].idHash);
                    if (o.name == L"::$DATA") {
                        ++unnamedKeys[key];
                    } else {
                        ++namedKeys[key];
                    }
                    break;
                }
            }
            all = all && found;
        }
        if (all) {
            ++matched;
            ++reasonMatched[sample.reason];
        } else {
            ++mismatched;
            if (mismatchShown++ < 15) {
                Line(L"  MISMATCH id 0x%016llx (%s)", sample.id, sample.reason);
                for (const OracleStream& o : oracle) {
                    Line(L"    oracle  [%s] size %llu alloc %llu", Printable(o.name).c_str(), o.size, o.allocation);
                }
                for (const Stream& d : decoded) {
                    Line(L"    decoded v%lu code 0x%04lx idLen %lu flags 0x%lx eof %llu alloc %llu", d.version, d.code,
                         d.idLength, d.flags, d.endOfFile, d.allocation);
                }
            }
        }
    }
    Line(L"Oracle ok %llu, oracle failed %llu, size-matched %llu, mismatched %llu", oracleOk, oracleFail, matched,
         mismatched);
    for (const auto& [error, n] : openErrors) Line(L"  oracle error %lu: %llu", error, n);
    for (const auto& [reason, n] : reasonTotal) {
        Line(L"  %-12s matched %llu / %llu", reason.c_str(), reasonMatched[reason], n);
    }
    Line(L"Decoded (code, idHash) matched to ::$DATA:");
    for (const auto& [key, n] : unnamedKeys) Line(L"  0x%04lx 0x%08x : %llu", key.first, key.second, n);
    Line(L"Decoded (code, idHash) matched to named streams:");
    for (const auto& [key, n] : namedKeys) Line(L"  0x%04lx 0x%08x : %llu", key.first, key.second, n);

    Line(L"");
    Line(L"Raw stream bytes for the first 8 samples (v2 offsets: Version@0 Next@4 Flags@8 ExtOff@12 "
         L"Alloc@16 EOF@24 InfoOff@32 Code@36 AttrFlags@40 IdLen@44 Id@48):");
    for (size_t i = 0; i < samples.size() && i < 8; ++i) {
        std::vector<OracleStream> oracle;
        DWORD error = 0;
        QueryOracle(rootHandle, samples[i].id, oracle, error);
        Line(L"  id 0x%016llx (%s)", samples[i].id, samples[i].reason);
        for (const OracleStream& o : oracle) {
            Line(L"    oracle [%s] size %llu alloc %llu", Printable(o.name).c_str(), o.size, o.allocation);
        }
        HexDump(samples[i].raw, 256);
    }
    CloseHandle(rootHandle);

    // ---- verdict ------------------------------------------------------------

    std::pair<DWORD, std::uint32_t> dataKey{};
    std::uint64_t dataKeyHits = 0, unnamedTotal = 0;
    for (const auto& [key, n] : unnamedKeys) {
        unnamedTotal += n;
        if (n > dataKeyHits) {
            dataKeyHits = n;
            dataKey = key;
        }
    }
    const bool verified = pass1Complete && oracleOk >= 100 && matched * 100 >= oracleOk * 99 &&
                          unnamedTotal > 0 && dataKeyHits * 100 >= unnamedTotal * 99;
    Line(L"");
    Line(L"Verdict: %s", verified ? L"LAYOUT VERIFIED (sizes match oracle on >= 99% of samples)"
                                  : L"LAYOUT UNVERIFIED - totals skipped");
    if (!verified) {
        Line(L"Peak working set : %llu bytes", PeakWorkingSetBytes());
        VirtualFree(buffer, 0, MEM_RELEASE);
        return 0;
    }
    Line(L"Unnamed data stream: code 0x%04lx, idHash 0x%08x", dataKey.first, dataKey.second);

    // ---- totals ------------------------------------------------------------

    enum Kind { kData = 0, kNamedData = 1, kOther = 2 };
    auto kindOf = [&](const Stream& s) {
        if (s.code != dataKey.first) return kOther;
        return s.idHash == dataKey.second ? kData : kNamedData;
    };

    std::unordered_map<std::uint64_t, std::uint64_t> topOf;
    topOf.reserve(files.size());
    std::uint64_t orphanLinks = 0;
    auto resolve = [&](std::uint64_t id) -> std::uint64_t {
        std::vector<std::uint64_t> chain;
        std::uint64_t current = id, result = 0;
        for (int depth = 0; depth < 4096; ++depth) {
            auto known = topOf.find(current);
            if (known != topOf.end()) {
                result = known->second;
                break;
            }
            auto it = files.find(current);
            if (it == files.end() || it->second.links.empty()) break;
            chain.push_back(current);
            const std::uint64_t parent = it->second.links.front().parent;
            if (parent == rootId) {
                result = current;
                break;
            }
            current = parent;
        }
        for (std::uint64_t c : chain) topOf[c] = result;
        return result;
    };

    using Totals = std::array<Sizes, 3>;
    std::unordered_map<std::uint64_t, Totals> perTop;
    Totals perLink{}, perFile{}, rootFilesT{}, orphanT{};
    std::uint64_t hardLinkedFiles = 0, extraLinks = 0, compressedLogical = 0, compressedAlloc = 0;
    for (const auto& [id, record] : files) {
        if (record.links.size() > 1) {
            ++hardLinkedFiles;
            extraLinks += record.links.size() - 1;
        }
        for (const Stream& s : record.streams) {
            perFile[kindOf(s)].Add(s.endOfFile, s.allocation);
            if ((record.attributes & FILE_ATTRIBUTE_COMPRESSED) && kindOf(s) == kData) {
                compressedLogical += s.endOfFile;
                compressedAlloc += s.allocation;
            }
        }
        for (const Link& link : record.links) {
            Sizes* bucket = nullptr;
            if (link.parent == rootId) {
                bucket = (record.attributes & FILE_ATTRIBUTE_DIRECTORY) && id != rootId ? perTop[id].data() : rootFilesT.data();
            } else {
                const std::uint64_t top = resolve(link.parent);
                if (top == 0) {
                    ++orphanLinks;
                    bucket = orphanT.data();
                } else {
                    bucket = perTop[top].data();
                }
            }
            for (const Stream& s : record.streams) {
                perLink[kindOf(s)].Add(s.endOfFile, s.allocation);
                bucket[kindOf(s)].Add(s.endOfFile, s.allocation);
            }
        }
    }

    auto row = [](const wchar_t* label, const Sizes* t) {
        Line(L"%-28s data %llu / %llu   named %llu (%llu B)   other %llu B", label, t[kData].endOfFile,
             t[kData].allocation, t[kNamedData].count, t[kNamedData].endOfFile, t[kOther].endOfFile);
    };
    Line(L"");
    Line(L"Columns: data EOF / alloc (unnamed data streams), named data streams count (bytes), other attributes bytes");
    Line(L"Hard-linked files: %llu (extra links %llu)", hardLinkedFiles, extraLinks);
    Line(L"Orphan links     : %llu", orphanLinks);
    Line(L"Compressed files : data EOF %llu, alloc %llu", compressedLogical, compressedAlloc);
    row(L"Per link (Phase 1 rule)", perLink.data());
    row(L"Per file (once)", perFile.data());
    row(L"Files directly in root", rootFilesT.data());
    row(L"Orphans", orphanT.data());

    struct TopRow {
        std::wstring name;
        const Sizes* t = nullptr;
    };
    std::vector<TopRow> rows;
    for (const auto& [id, t] : perTop) {
        TopRow r;
        for (const Link& link : files[id].links) {
            if (link.parent == rootId) {
                r.name = link.name;
                break;
            }
        }
        if (r.name.empty()) r.name = L"<unnamed>";
        r.t = t.data();
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(),
              [](const TopRow& a, const TopRow& b) { return a.t[kData].endOfFile > b.t[kData].endOfFile; });
    Line(L"");
    Line(L"Top-level folders (%zu), counted once per link:", rows.size());
    for (const TopRow& r : rows) {
        row((L"  " + Printable(r.name)).c_str(), r.t);
    }

    Line(L"");
    Line(L"Total time       : %.3f s",
         std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    Line(L"Peak working set : %llu bytes", PeakWorkingSetBytes());
    VirtualFree(buffer, 0, MEM_RELEASE);
    return 0;
}
