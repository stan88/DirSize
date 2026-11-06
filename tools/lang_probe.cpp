// Phase 6 feasibility probe (measurement only, not shipped). See
// docs/DESIGN-phase6.md section 5, decision 7.5 and section 8 step 1.
//
// The executable carries two STRINGTABLE languages (lang_probe.rc):
//   id 1: zh-TW "ZH 1" and en-US "EN 1"
//   id 2: en-US only
//   id 3: zh-TW only
// LoadString's choice among languages is not documented, so this probe
// records what it returns with no setting, with SetThreadPreferredUILanguages
// for several lists, on a new thread, and with SetProcessPreferredUILanguages.
//
//   lang-probe [result-file]     (default: lang-probe-results.txt here)

#include <windows.h>

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

std::wstring g_out;

void Line(const std::wstring& s) {
    g_out += s;
    g_out += L"\r\n";
    fwprintf(stdout, L"%s\n", s.c_str());
}

// A MULTI_SZ list as "a;b;c".
std::wstring Joined(const std::vector<wchar_t>& buf) {
    std::wstring r;
    for (const wchar_t* p = buf.data(); p && *p; p += wcslen(p) + 1) {
        if (!r.empty()) r += L";";
        r += p;
    }
    return r.empty() ? L"(empty)" : r;
}

template <typename F>
std::wstring List(F get, DWORD flags) {
    ULONG count = 0, size = 0;
    if (!get(flags, &count, nullptr, &size)) return L"(error " + std::to_wstring(GetLastError()) + L")";
    std::vector<wchar_t> buf(size + 1, 0);
    if (!get(flags, &count, buf.data(), &size)) return L"(error " + std::to_wstring(GetLastError()) + L")";
    return Joined(buf);
}

std::wstring ThreadList() {
    return List([](DWORD f, PULONG c, PZZWSTR b, PULONG s) { return GetThreadPreferredUILanguages(f, c, b, s); },
                MUI_LANGUAGE_NAME | MUI_MERGE_USER_FALLBACK | MUI_MERGE_SYSTEM_FALLBACK);
}

std::wstring Load(UINT id) {
    wchar_t buf[128] = {};
    SetLastError(0);
    int n = LoadStringW(GetModuleHandleW(nullptr), id, buf, 128);
    if (n == 0) return L"(none, error " + std::to_wstring(GetLastError()) + L")";
    return buf;
}

void Report(const std::wstring& label) {
    Line(L"[" + label + L"]");
    Line(L"  thread list (merged): " + ThreadList());
    Line(L"  id1=" + Load(1) + L" | id2=" + Load(2) + L" | id3=" + Load(3));
}

// Sets the thread list from "a;b" (empty clears it).
bool SetThread(const std::wstring& names) {
    if (names.empty()) return SetThreadPreferredUILanguages(0, nullptr, nullptr) != FALSE;
    std::vector<wchar_t> buf;
    for (wchar_t ch : names) buf.push_back(ch == L';' ? L'\0' : ch);
    buf.push_back(L'\0');
    buf.push_back(L'\0');
    ULONG count = 0;
    BOOL ok = SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, buf.data(), &count);
    if (!ok) Line(L"  SetThreadPreferredUILanguages(" + names + L") failed, error " + std::to_wstring(GetLastError()));
    return ok != FALSE;
}

void ThreadCase(const std::wstring& names) {
    if (SetThread(names)) Report(L"thread = " + names);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring outPath = argc > 1 ? argv[1] : L"lang-probe-results.txt";

    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
    GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
    Line(L"lang-probe");
    Line(L"  user default UI language: 0x" + [] {
        wchar_t b[16];
        swprintf_s(b, L"%04X", GetUserDefaultUILanguage());
        return std::wstring(b);
    }());
    Line(L"  system default UI language: 0x" + [] {
        wchar_t b[16];
        swprintf_s(b, L"%04X", GetSystemDefaultUILanguage());
        return std::wstring(b);
    }());
    Line(L"  user default locale: " + std::wstring(locale));
    Line(L"  user preferred UI languages: " +
         List([](DWORD f, PULONG c, PZZWSTR b, PULONG s) { return GetUserPreferredUILanguages(f, c, b, s); },
              MUI_LANGUAGE_NAME));
    Line(L"  system preferred UI languages: " +
         List([](DWORD f, PULONG c, PZZWSTR b, PULONG s) { return GetSystemPreferredUILanguages(f, c, b, s); },
              MUI_LANGUAGE_NAME));

    Report(L"default, no setting");
    ThreadCase(L"en-US");
    ThreadCase(L"zh-TW");
    ThreadCase(L"en-GB");        // sub-language not in the exe
    ThreadCase(L"zh-HK");        // sub-language not in the exe
    ThreadCase(L"ja-JP");        // language not in the exe
    ThreadCase(L"ja-JP;en-US");  // first missing, second present
    ThreadCase(L"en-US;zh-TW");
    ThreadCase(L"zh-TW;en-US");

    // Does a new thread inherit the setting? (Worker threads load strings too.)
    SetThread(L"en-US");
    std::thread([] { Report(L"new thread after main thread = en-US"); }).join();
    SetThread(L"zh-TW");
    std::thread([] { Report(L"new thread after main thread = zh-TW"); }).join();

    if (SetThread(L"")) Report(L"thread cleared");

    // Process-wide setting, then a new thread and the (cleared) main thread.
    {
        wchar_t en[] = L"en-US\0";
        ULONG count = 0;
        if (SetProcessPreferredUILanguages(MUI_LANGUAGE_NAME, en, &count)) {
            Report(L"process = en-US (main thread)");
            std::thread([] { Report(L"process = en-US (new thread)"); }).join();
            SetThread(L"zh-TW");
            Report(L"process = en-US, thread = zh-TW");
        } else {
            Line(L"SetProcessPreferredUILanguages(en-US) failed, error " + std::to_wstring(GetLastError()));
        }
    }

    // UTF-8 with BOM.
    std::string bytes = "\xEF\xBB\xBF";
    int n = WideCharToMultiByte(CP_UTF8, 0, g_out.c_str(), static_cast<int>(g_out.size()), nullptr, 0, nullptr, nullptr);
    std::string body(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, g_out.c_str(), static_cast<int>(g_out.size()), body.data(), n, nullptr, nullptr);
    bytes += body;
    FILE* f = nullptr;
    if (_wfopen_s(&f, outPath.c_str(), L"wb") == 0 && f) {
        fwrite(bytes.data(), 1, bytes.size(), f);
        fclose(f);
        fwprintf(stdout, L"\nwrote %s\n", outPath.c_str());
    } else {
        fwprintf(stderr, L"cannot write %s\n", outPath.c_str());
        return 1;
    }
    return 0;
}
