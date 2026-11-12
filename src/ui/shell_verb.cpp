#include "ui/shell_verb.h"

#include <shlobj.h>

#include <string>
#include <vector>

#include "ui/strings.h"
#include "ui/verb_command.h"

namespace dirsize::ui {

namespace {

const wchar_t* const kVerbKeys[] = {
    L"Software\\Classes\\Directory\\shell\\DirSize",
    L"Software\\Classes\\Drive\\shell\\DirSize",
};

const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kRunValue[] = L"DirSizePanel";

DWORD SetDefault(const std::wstring& key, const std::wstring& value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, REG_SZ, value.c_str(),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

std::wstring ExePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) return L"";
        if (n < buffer.size()) return std::wstring(buffer.data(), n);
        buffer.resize(buffer.size() * 2);  // truncated: try a larger buffer
    }
}

}  // namespace

bool IsShellVerbInstalled() {
    HKEY h = nullptr;
    std::wstring key = std::wstring(kVerbKeys[0]) + L"\\command";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

DWORD InstallShellVerb() {
    const std::wstring exe = ExePath();
    if (exe.empty()) return GetLastError();
    DWORD error = ERROR_SUCCESS;
    for (const wchar_t* key : kVerbKeys) {
        error = SetDefault(key, Text(IDS_VERB_TEXT));  // the menu text
        if (error == ERROR_SUCCESS) error = SetDefault(std::wstring(key) + L"\\command", VerbCommandLine(exe));
        if (error != ERROR_SUCCESS) break;
    }
    if (error != ERROR_SUCCESS) RemoveShellVerb();  // no half-registered verb
    else SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return error;
}

DWORD RemoveShellVerb() {
    DWORD result = ERROR_SUCCESS;
    for (const wchar_t* key : kVerbKeys) {
        LSTATUS s = RegDeleteTreeW(HKEY_CURRENT_USER, key);
        if (s != ERROR_SUCCESS && s != ERROR_FILE_NOT_FOUND && result == ERROR_SUCCESS) result = s;
        // The key itself, in case the tree call left it (as the probe did).
        s = RegDeleteKeyW(HKEY_CURRENT_USER, key);
        if (s != ERROR_SUCCESS && s != ERROR_FILE_NOT_FOUND && result == ERROR_SUCCESS) result = s;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return result;
}

bool IsShellVerbSetByInstaller() {
    HKEY h = nullptr;
    std::wstring key = std::wstring(kVerbKeys[0]) + L"\\command";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

bool IsPanelAutostart() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) ==
           ERROR_SUCCESS;
}

DWORD SetPanelAutostart(bool on) {
    if (!on) {
        LSTATUS s = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        return s == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : static_cast<DWORD>(s);
    }
    const std::wstring exe = ExePath();
    if (exe.empty()) return GetLastError();
    const std::wstring command = PanelCommandLine(exe);
    // The documentation limits a Run command line to 260 characters.
    if (command.size() > 260) return ERROR_FILENAME_EXCED_RANGE;
    return static_cast<DWORD>(RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, command.c_str(),
                                              static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))));
}

}  // namespace dirsize::ui
