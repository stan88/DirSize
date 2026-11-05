#pragma once
// User-visible text from the executable's string table (string_ids.h).

#include <windows.h>

#include <string>

#include "ui/string_ids.h"

namespace dirsize::ui {

// The string with this ID, or "#<id>" if it is missing (so a missing entry
// is visible instead of blank).
std::wstring Text(UINT id);

}  // namespace dirsize::ui
