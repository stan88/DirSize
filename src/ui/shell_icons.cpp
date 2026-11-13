#include "ui/shell_icons.h"

#include <shellapi.h>

#include <map>
#include <mutex>

namespace dirsize::ui {

namespace {

struct Icons {
    HICON small = nullptr;
    HICON large = nullptr;
    int smallPx = 0;
    int largePx = 0;
};

std::mutex& Mutex() {
    static std::mutex m;
    return m;
}

// Never destroyed: the icons are shared by every window until the process
// ends.
std::map<std::wstring, Icons>& Cache() {
    static auto* cache = new std::map<std::wstring, Icons>();
    return *cache;
}

int IconPixels(HICON icon) {
    ICONINFO ii{};
    if (!icon || !GetIconInfo(icon, &ii)) return 0;
    BITMAP bm{};
    GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return bm.bmWidth;
}

HICON Ask(const std::wstring& key, UINT sizeFlag) {
    std::wstring name;
    DWORD attributes = FILE_ATTRIBUTE_NORMAL;
    UINT flags = SHGFI_ICON | sizeFlag | SHGFI_USEFILEATTRIBUTES;
    static const std::wstring kDrive = L"<drive>";
    if (key == L"<folder>") {
        name = L"folder";
        attributes = FILE_ATTRIBUTE_DIRECTORY;
    } else if (key == L"<file>") {
        name = L"file";
    } else if (key.compare(0, kDrive.size(), kDrive) == 0) {
        name = key.substr(kDrive.size());
        attributes = FILE_ATTRIBUTE_DIRECTORY;
        // A local drive's own icon (system drive, removable, optical...);
        // a network drive is not asked.
        if (GetDriveTypeW(name.c_str()) != DRIVE_REMOTE) flags &= ~SHGFI_USEFILEATTRIBUTES;
    } else {
        name = L"file" + key;  // ".txt" -> "file.txt"
    }
    SHFILEINFOW info{};
    if (!SHGetFileInfoW(name.c_str(), attributes, &info, sizeof(info), flags)) return nullptr;
    return info.hIcon;
}

}  // namespace

void LoadShellIcons(const std::vector<std::wstring>& keys) {
    for (const std::wstring& key : keys) {
        {
            std::lock_guard lock(Mutex());
            if (Cache().count(key)) continue;
        }
        Icons icons;
        icons.small = Ask(key, SHGFI_SMALLICON);
        icons.large = Ask(key, SHGFI_LARGEICON);
        icons.smallPx = IconPixels(icons.small);
        icons.largePx = IconPixels(icons.large);
        std::lock_guard lock(Mutex());
        auto [it, added] = Cache().emplace(key, icons);
        if (!added) {  // another worker loaded it meanwhile
            if (icons.small) DestroyIcon(icons.small);
            if (icons.large) DestroyIcon(icons.large);
        }
    }
}

bool DrawShellIcon(HDC dc, const std::wstring& key, int x, int y, int px) {
    HICON icon = nullptr;
    {
        std::lock_guard lock(Mutex());
        auto it = Cache().find(key);
        if (it == Cache().end()) return false;
        const Icons& i = it->second;
        // The smaller one when it is big enough (less scaling down).
        icon = i.small && i.smallPx >= px ? i.small : i.large ? i.large : i.small;
    }
    if (!icon) return false;
    return DrawIconEx(dc, x, y, icon, px, px, 0, nullptr, DI_NORMAL) != FALSE;
}

}  // namespace dirsize::ui
