#include "ui/strings.h"

#include <vector>

#include "ui/format.h"
#include "ui/language.h"

namespace dirsize::ui {

namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\DirSize";
constexpr wchar_t kLanguageValue[] = L"Language";

std::wstring g_current;

// A MULTI_SZ buffer as a list.
std::vector<std::wstring> SplitMultiSz(const std::vector<wchar_t>& buf) {
    std::vector<std::wstring> out;
    for (const wchar_t* p = buf.data(); p < buf.data() + buf.size() && *p; p += wcslen(p) + 1) out.emplace_back(p);
    return out;
}

std::vector<std::wstring> UserLanguages() {
    ULONG count = 0, size = 0;
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) || size == 0) return {};
    std::vector<wchar_t> buf(size + 1, L'\0');
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buf.data(), &size)) return {};
    return SplitMultiSz(buf);
}

}  // namespace

std::wstring Text(UINT id) {
    // With a buffer size of 0, LoadStringW returns a read-only pointer to
    // the resource itself and its length (not null-terminated).
    const wchar_t* text = nullptr;
    int length = LoadStringW(GetModuleHandleW(nullptr), id, reinterpret_cast<LPWSTR>(&text), 0);
    if (length <= 0 || !text) return L"#" + std::to_wstring(id);
    return std::wstring(text, static_cast<size_t>(length));
}

std::wstring Format(UINT id, std::initializer_list<std::wstring> args) { return FormatPattern(Text(id), args); }

std::wstring SavedLanguage() {
    wchar_t buf[LOCALE_NAME_MAX_LENGTH] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kLanguageValue, RRF_RT_REG_SZ, nullptr, buf, &size) !=
        ERROR_SUCCESS) {
        return L"";
    }
    return buf;
}

DWORD SaveLanguage(const std::wstring& locale) {
    if (locale.empty()) {
        HKEY key = nullptr;
        LSTATUS s = RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_SET_VALUE, &key);
        if (s == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
        if (s != ERROR_SUCCESS) return static_cast<DWORD>(s);
        s = RegDeleteValueW(key, kLanguageValue);
        RegCloseKey(key);
        return s == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : static_cast<DWORD>(s);
    }
    return static_cast<DWORD>(RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, kLanguageValue, REG_SZ, locale.c_str(),
                                              static_cast<DWORD>((locale.size() + 1) * sizeof(wchar_t))));
}

std::wstring ApplyUiLanguage() {
    auto table = Languages();
    size_t pick = FindLanguage(SavedLanguage(), table);
    if (pick >= table.size()) pick = PickLanguage(UserLanguages(), table);
    if (pick >= table.size()) return L"";
    std::wstring list = table[pick].locale;
    list.push_back(L'\0');
    list.push_back(L'\0');
    ULONG count = 0;
    if (SetProcessPreferredUILanguages(MUI_LANGUAGE_NAME, list.c_str(), &count) && count > 0) {
        g_current = table[pick].locale;
    }
    return g_current;
}

const std::wstring& CurrentLanguage() { return g_current; }

std::wstring LanguageDisplayName(const std::wstring& locale) {
    wchar_t buf[128] = {};
    if (GetLocaleInfoEx(locale.c_str(), LOCALE_SNATIVEDISPLAYNAME, buf, 128) > 0) return buf;
    return locale;
}

}  // namespace dirsize::ui
