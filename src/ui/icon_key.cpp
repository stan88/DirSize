#include "ui/icon_key.h"

#include <cwctype>

namespace dirsize::ui {

std::wstring IconKey(const std::wstring& name, bool isFolder) {
    if (isFolder) return L"<folder>";
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos || dot + 1 >= name.size()) return L"<file>";
    std::wstring ext = name.substr(dot);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

std::wstring DriveIconKey(const std::wstring& root) { return L"<drive>" + root; }

}  // namespace dirsize::ui
