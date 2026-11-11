#pragma once
// User-visible text from the executable's string table (string_ids.h,
// lang/*.json), and the choice of its language.

#include <windows.h>

#include <initializer_list>
#include <string>

#include "ui/string_ids.h"

namespace dirsize::ui {

// The string with this ID, or "#<id>" if it is missing (so a missing entry
// is visible instead of blank).
std::wstring Text(UINT id);

// Text(id) with %1..%9 replaced by args (FormatPattern).
std::wstring Format(UINT id, std::initializer_list<std::wstring> args);

// The language the user chose in the "..." menu (HKCU\Software\DirSize,
// value Language), or "" to follow Windows.
std::wstring SavedLanguage();

// Saves the choice; "" removes it (follow Windows). ApplyUiLanguage then
// switches the running process (ui-tests checks a second call takes effect).
DWORD SaveLanguage(const std::wstring& locale);

// Picks the string-table language (the saved choice, else the user's Windows
// display languages through PickLanguage) and sets it for the whole process,
// so worker threads load the same language. Call first in wWinMain, before
// any window or thread, and again after the user picks another language.
// Returns the locale that was set.
std::wstring ApplyUiLanguage();

// The locale ApplyUiLanguage set ("" before it runs).
const std::wstring& CurrentLanguage();

// The language's own name for itself ("English (United States)"), or the
// locale name if Windows has none.
std::wstring LanguageDisplayName(const std::wstring& locale);

}  // namespace dirsize::ui
