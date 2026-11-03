// dirsize-cli: command-line front end for the DirSize indexer.
//
// Phase 1: a baseline scanner that walks a folder tree with directory
// enumeration and reports totals, timing and memory use.

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

#include "core/dir_walker.h"
#include "core/path_util.h"

namespace {

using dirsize::ChildFolder;
using dirsize::WalkResult;

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

void WriteLine(std::wstring_view text = {}) {
    Write(text);
    Write(L"\n");
}

std::wstring Format(const wchar_t* format, ...) {
    wchar_t buffer[512];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

std::wstring FormatBytes(std::uint64_t bytes) {
    static const wchar_t* const kUnits[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? Format(L"%llu B", bytes) : Format(L"%.2f %s", value, kUnits[unit]);
}

std::wstring JsonEscape(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size() + 2);
    for (wchar_t c : text) {
        switch (c) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20) {
                    out += Format(L"\\u%04x", static_cast<unsigned>(c));
                } else {
                    out.push_back(c);
                }
        }
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

std::uint64_t PeakWorkingSetBytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return counters.PeakWorkingSetSize;
    }
    return 0;
}

// ---- commands ---------------------------------------------------------------

struct ScanOptions {
    std::wstring path;
    size_t top = 20;
    bool json = false;
};

void PrintUsage() {
    WriteLine(L"Usage:");
    WriteLine(L"  dirsize-cli scan <path> [--top N] [--json]");
    WriteLine();
    WriteLine(L"Walks the folder tree under <path> and reports total size, file and");
    WriteLine(L"folder counts, elapsed time and peak memory use.");
    WriteLine(L"  --top N   list the N largest immediate sub-folders (default 20, 0 = all)");
    WriteLine(L"  --json    machine-readable output");
}

void PrintText(const std::wstring& displayPath, const WalkResult& r, double seconds, bool elevated,
               std::uint64_t peakBytes, const std::vector<const ChildFolder*>& top) {
    WriteLine(Format(L"Path              %s", displayPath.c_str()));
    WriteLine(Format(L"Elevated          %s", elevated ? L"yes" : L"no"));
    WriteLine(Format(L"Elapsed           %.3f s", seconds));
    WriteLine(Format(L"Folders           %llu", r.totals.folders));
    WriteLine(Format(L"Files             %llu", r.totals.files));
    WriteLine(Format(L"Size              %s (%llu bytes)", FormatBytes(r.totals.logicalBytes).c_str(),
                     r.totals.logicalBytes));
    WriteLine(Format(L"Size on disk      %s (%llu bytes)", FormatBytes(r.totals.allocatedBytes).c_str(),
                     r.totals.allocatedBytes));
    WriteLine(Format(L"Access denied     %llu", r.accessDenied));
    WriteLine(Format(L"Other errors      %llu", r.otherErrors));
    WriteLine(Format(L"Links skipped     %llu", r.reparsePointsSkipped));
    WriteLine(Format(L"Peak memory       %s", FormatBytes(peakBytes).c_str()));
    if (seconds > 0) {
        WriteLine(Format(L"Throughput        %.0f entries/s",
                         static_cast<double>(r.totals.files + r.totals.folders) / seconds));
    }

    if (!top.empty()) {
        WriteLine();
        WriteLine(L"         Size    Size on disk       Files     Folders  Name");
        for (const ChildFolder* child : top) {
            WriteLine(Format(L"%13s %15s %11llu %11llu  %s", FormatBytes(child->totals.logicalBytes).c_str(),
                             FormatBytes(child->totals.allocatedBytes).c_str(), child->totals.files,
                             child->totals.folders, child->name.c_str()));
        }
    }
}

void PrintJson(const std::wstring& displayPath, const WalkResult& r, double seconds, bool elevated,
               std::uint64_t peakBytes, const std::vector<const ChildFolder*>& top) {
    WriteLine(L"{");
    WriteLine(Format(L"  \"path\": \"%s\",", JsonEscape(displayPath).c_str()));
    WriteLine(Format(L"  \"elevated\": %s,", elevated ? L"true" : L"false"));
    WriteLine(Format(L"  \"elapsedSeconds\": %.3f,", seconds));
    WriteLine(Format(L"  \"folders\": %llu,", r.totals.folders));
    WriteLine(Format(L"  \"files\": %llu,", r.totals.files));
    WriteLine(Format(L"  \"logicalBytes\": %llu,", r.totals.logicalBytes));
    WriteLine(Format(L"  \"allocatedBytes\": %llu,", r.totals.allocatedBytes));
    WriteLine(Format(L"  \"accessDenied\": %llu,", r.accessDenied));
    WriteLine(Format(L"  \"otherErrors\": %llu,", r.otherErrors));
    WriteLine(Format(L"  \"reparsePointsSkipped\": %llu,", r.reparsePointsSkipped));
    WriteLine(Format(L"  \"peakWorkingSetBytes\": %llu,", peakBytes));
    WriteLine(L"  \"children\": [");
    for (size_t i = 0; i < top.size(); ++i) {
        const ChildFolder* c = top[i];
        WriteLine(Format(L"    {\"name\": \"%s\", \"logicalBytes\": %llu, \"allocatedBytes\": %llu, "
                         L"\"files\": %llu, \"folders\": %llu}%s",
                         JsonEscape(c->name).c_str(), c->totals.logicalBytes, c->totals.allocatedBytes,
                         c->totals.files, c->totals.folders, i + 1 < top.size() ? L"," : L""));
    }
    WriteLine(L"  ]");
    WriteLine(L"}");
}

int RunScan(const ScanOptions& options) {
    const std::wstring extended = dirsize::ToExtendedPath(options.path);
    if (extended.empty()) {
        WriteLine(Format(L"error: invalid path: %s", options.path.c_str()));
        return 2;
    }
    const std::wstring displayPath = dirsize::ToDisplayPath(extended);

    const auto start = std::chrono::steady_clock::now();
    const WalkResult result = dirsize::WalkTree(extended);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    if (!result.rootOpened) {
        WriteLine(Format(L"error: cannot open %s (Win32 error %lu)", displayPath.c_str(), result.rootError));
        return 1;
    }

    std::vector<const ChildFolder*> top;
    top.reserve(result.children.size());
    for (const ChildFolder& child : result.children) {
        top.push_back(&child);
    }
    std::sort(top.begin(), top.end(), [](const ChildFolder* a, const ChildFolder* b) {
        return a->totals.logicalBytes > b->totals.logicalBytes;
    });
    if (options.top > 0 && top.size() > options.top) {
        top.resize(options.top);
    }

    const bool elevated = IsElevated();
    const std::uint64_t peakBytes = PeakWorkingSetBytes();
    if (options.json) {
        PrintJson(displayPath, result, seconds, elevated, peakBytes, top);
    } else {
        PrintText(displayPath, result, seconds, elevated, peakBytes, top);
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 3 || std::wstring_view(argv[1]) != L"scan") {
        PrintUsage();
        return 2;
    }

    ScanOptions options;
    options.path = argv[2];
    for (int i = 3; i < argc; ++i) {
        std::wstring_view arg = argv[i];
        if (arg == L"--json") {
            options.json = true;
        } else if (arg == L"--top" && i + 1 < argc) {
            options.top = static_cast<size_t>(std::wcstoull(argv[++i], nullptr, 10));
        } else {
            PrintUsage();
            return 2;
        }
    }
    return RunScan(options);
}
