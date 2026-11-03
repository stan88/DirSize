#include "core/path_util.h"

#include <windows.h>

namespace dirsize {
namespace {

constexpr std::wstring_view kExtendedPrefix = L"\\\\?\\";
constexpr std::wstring_view kExtendedUncPrefix = L"\\\\?\\UNC\\";

}  // namespace

std::wstring ToExtendedPath(std::wstring_view path) {
    std::wstring input(path);
    if (input.starts_with(kExtendedPrefix)) {
        return input;
    }

    DWORD needed = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return {};
    }
    std::wstring full(needed, L'\0');
    DWORD written = GetFullPathNameW(input.c_str(), needed, full.data(), nullptr);
    if (written == 0 || written >= needed) {
        return {};
    }
    full.resize(written);

    // Drop a trailing backslash, except on a drive root such as "C:\".
    if (full.size() > 3 && full.back() == L'\\') {
        full.pop_back();
    }

    if (full.starts_with(L"\\\\")) {
        // UNC path: \\server\share -> \\?\UNC\server\share
        return std::wstring(kExtendedUncPrefix) + full.substr(2);
    }
    return std::wstring(kExtendedPrefix) + full;
}

std::wstring ToDisplayPath(std::wstring_view extendedPath) {
    if (extendedPath.starts_with(kExtendedUncPrefix)) {
        return L"\\\\" + std::wstring(extendedPath.substr(kExtendedUncPrefix.size()));
    }
    if (extendedPath.starts_with(kExtendedPrefix)) {
        return std::wstring(extendedPath.substr(kExtendedPrefix.size()));
    }
    return std::wstring(extendedPath);
}

std::wstring JoinPath(std::wstring_view dir, std::wstring_view name) {
    std::wstring result;
    result.reserve(dir.size() + 1 + name.size());
    result.append(dir);
    if (!result.empty() && result.back() != L'\\') {
        result.push_back(L'\\');
    }
    result.append(name);
    return result;
}

}  // namespace dirsize
