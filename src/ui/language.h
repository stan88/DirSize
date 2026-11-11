#pragma once
// Which string-table language the front end uses (docs/DESIGN-phase6.md
// section 5). Windows does not map a neutral or other-region name to a table
// (zh-HK got English, en-GB got Chinese in the probe), so the program picks
// the table itself and sets it with SetProcessPreferredUILanguages.

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dirsize::ui {

// One lang/<locale>.json, as compiled into the executable.
struct LanguageEntry {
    const wchar_t* locale;           // "zh-TW"
    unsigned short langId;           // the table's LANGUAGE
    const wchar_t* const* matches;   // other names it serves, ends with nullptr
};

// The languages built from lang/*.json (generated languages.inc).
std::span<const LanguageEntry> Languages();

// True if tag is pattern, or starts with pattern followed by '-'
// (case-insensitive): "zh-Hant-TW" matches "zh-Hant", "en-GB" matches "en",
// "zh-CN" does not match "zh-TW".
bool TagMatches(std::wstring_view tag, std::wstring_view pattern);

// The entry for the first preferred name that an entry serves (its own locale
// or one of its matches); en-US when none does. Returns an index into table,
// or table.size() when the table has no en-US either.
size_t PickLanguage(const std::vector<std::wstring>& preferred, std::span<const LanguageEntry> table);

// The index of the entry whose locale is name (case-insensitive), or
// table.size().
size_t FindLanguage(std::wstring_view name, std::span<const LanguageEntry> table);

}  // namespace dirsize::ui
