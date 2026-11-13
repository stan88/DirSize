#pragma once
// Which system icon a row shows (shell_icons.h). Icons are looked up by type
// only, never by reading the item: a folder, a file extension, or a drive.
// No window or shell code here, so the keys are unit-tested.

#include <string>

namespace dirsize::ui {

// "<folder>" for a folder; for a file its extension in lower case with the
// dot (".txt", ".gitignore" for a name that is only an extension), or
// "<file>" when it has none ("README", "name.").
std::wstring IconKey(const std::wstring& name, bool isFolder);

// A drive root ("C:\") as a key: "<drive>C:\".
std::wstring DriveIconKey(const std::wstring& root);

}  // namespace dirsize::ui
