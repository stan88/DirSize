#pragma once
// Text formatting for sizes and counts shown in the front end.

#include <cstdint>
#include <initializer_list>
#include <string>

namespace dirsize::ui {

// 1024-based, like the CLI: "0 B", "512 B", "1.00 KB", "12.3 MB", "1.23 GB".
// Three significant digits from KB up.
std::wstring FormatBytes(std::uint64_t bytes);

// Thousands separators with ',' : "1,234,567".
std::wstring FormatCount(std::uint64_t count);

// part / whole as a percentage with one decimal: "12.3%". "0.0%" when whole is 0.
std::wstring FormatPercent(std::uint64_t part, std::uint64_t whole);

}  // namespace dirsize::ui

namespace dirsize::ui {

// Replaces %1..%9 in a pattern with args[0]..args[8], in one pass (an
// argument that contains "%2" is not replaced again). A placeholder without
// an argument, and any other '%', is kept as is. Used for string-table text,
// so a translation can put the values in its own order.
std::wstring FormatPattern(const std::wstring& pattern, std::initializer_list<std::wstring> args);

}  // namespace dirsize::ui
