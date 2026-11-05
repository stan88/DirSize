#include "ui/strings.h"

namespace dirsize::ui {

std::wstring Text(UINT id) {
    // With a buffer size of 0, LoadStringW returns a read-only pointer to
    // the resource itself and its length (not null-terminated).
    const wchar_t* text = nullptr;
    int length = LoadStringW(GetModuleHandleW(nullptr), id, reinterpret_cast<LPWSTR>(&text), 0);
    if (length <= 0 || !text) return L"#" + std::to_wstring(id);
    return std::wstring(text, static_cast<size_t>(length));
}

}  // namespace dirsize::ui
