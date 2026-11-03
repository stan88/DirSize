#pragma once

#include <string>
#include <string_view>

namespace dirsize {

// Converts a user-supplied path into an absolute extended-length path
// ("\\?\C:\..." or "\\?\UNC\server\share\..."), so paths longer than
// MAX_PATH work. Returns an empty string on failure.
std::wstring ToExtendedPath(std::wstring_view path);

// Strips the extended-length prefix for display.
std::wstring ToDisplayPath(std::wstring_view extendedPath);

// Joins a directory path and a child name with exactly one backslash.
std::wstring JoinPath(std::wstring_view dir, std::wstring_view name);

}  // namespace dirsize
