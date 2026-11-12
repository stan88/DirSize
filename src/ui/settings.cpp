#include "ui/settings.h"

#include <string>

namespace dirsize::ui {

namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\DirSize";
constexpr wchar_t kShowShareValue[] = L"ShowShare";

}  // namespace

bool ShowShareColumn() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kShowShareValue, RRF_RT_REG_DWORD, nullptr, &value, &size) !=
        ERROR_SUCCESS) {
        return true;
    }
    return value != 0;
}

DWORD SetShowShareColumn(bool show) {
    const DWORD value = show ? 1 : 0;
    return static_cast<DWORD>(
        RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, kShowShareValue, REG_DWORD, &value, sizeof(value)));
}

std::wstring ColumnWidthsSetting(bool panel) {
    wchar_t buf[256] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, panel ? L"PanelColumns" : L"WindowColumns", RRF_RT_REG_SZ,
                     nullptr, buf, &size) != ERROR_SUCCESS) {
        return L"";  // missing, or longer than any value this program writes
    }
    return buf;
}

DWORD SetColumnWidthsSetting(bool panel, const std::wstring& text) {
    return static_cast<DWORD>(RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey,
                                              panel ? L"PanelColumns" : L"WindowColumns", REG_SZ, text.c_str(),
                                              static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t))));
}

UINT SettingsChangedMessage() {
    static const UINT message = RegisterWindowMessageW(L"DirSize.SettingsChanged");
    return message;
}

void NotifySettingsChanged() {
    const UINT message = SettingsChangedMessage();
    if (message) PostMessageW(HWND_BROADCAST, message, 0, 0);
}

}  // namespace dirsize::ui
