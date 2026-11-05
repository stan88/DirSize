#include "core/cache_store.h"

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <winioctl.h>

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <cstring>

namespace dirsize {
namespace {

StoreResult Fail(const wchar_t* reason, unsigned long error = 0) {
    StoreResult r;
    r.reason = reason;
    r.error = error;
    return r;
}

StoreResult Ok() {
    StoreResult r;
    r.ok = true;
    return r;
}

StoreResult Missing(const wchar_t* reason) {
    StoreResult r;
    r.missing = true;
    r.reason = reason;
    return r;
}

class Handle {
public:
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return h_; }
    bool valid() const { return h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_;
};

bool IsSystemOrAdministrators(PSID sid) {
    return sid != nullptr && IsValidSid(sid) &&
           (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid));
}

int HexValue(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

// Only SYSTEM and Administrators, full access, inherited by files and
// subfolders; "P" blocks inheritance from ProgramData. Owner Administrators.
constexpr wchar_t kFolderSddl[] = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";

StoreResult EnsureFolder(const std::wstring& path, bool create) {
    if (create) {
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kFolderSddl, SDDL_REVISION_1, &sd, nullptr)) {
            return Fail(L"cannot build the folder security descriptor", GetLastError());
        }
        SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
        const BOOL created = CreateDirectoryW(path.c_str(), &sa);
        const DWORD error = GetLastError();
        LocalFree(sd);
        if (!created && error != ERROR_ALREADY_EXISTS) return Fail(L"cannot create the cache folder", error);
    }
    // Checked whether just created or already there: never trust the create.
    return CheckFolderIsPrivate(path);
}

}  // namespace

bool ParseVolumeGuidPath(const std::wstring& path, std::uint8_t guid[16], std::wstring& text) {
    // \\?\Volume{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}\  (GetVolumeNameForVolumeMountPointW)
    constexpr wchar_t kPrefix[] = L"\\\\?\\Volume{";
    constexpr size_t kPrefixLength = sizeof(kPrefix) / sizeof(wchar_t) - 1;
    if (path.size() != kPrefixLength + 36 + 2 || path.compare(0, kPrefixLength, kPrefix) != 0 ||
        path[kPrefixLength + 36] != L'}' || path.back() != L'\\') {
        return false;
    }
    const std::wstring body = path.substr(kPrefixLength, 36);
    // Field lengths 8-4-4-4-12; bytes stored as in the GUID structure
    // (Data1, Data2, Data3 little-endian; Data4 in order).
    std::uint8_t digits[16];
    size_t n = 0;
    for (size_t i = 0; i < body.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (body[i] != L'-') return false;
            continue;
        }
        const int hi = HexValue(body[i]);
        const int lo = i + 1 < body.size() ? HexValue(body[i + 1]) : -1;
        if (hi < 0 || lo < 0) return false;
        digits[n++] = static_cast<std::uint8_t>(hi * 16 + lo);
        ++i;
    }
    if (n != 16) return false;
    const std::uint8_t order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 16; ++i) guid[i] = digits[order[i]];
    text.clear();
    for (wchar_t c : body) text.push_back(static_cast<wchar_t>(std::towlower(c)));
    return true;
}

StoreResult GetVolumeIdentity(wchar_t drive, VolumeIdentity& identity) {
    const std::wstring mountPoint = std::wstring(1, drive) + L":\\";  // must end with a backslash
    wchar_t name[64] = {};  // the documentation suggests 50 characters
    if (!GetVolumeNameForVolumeMountPointW(mountPoint.c_str(), name, static_cast<DWORD>(std::size(name)))) {
        return Fail(L"cannot get the volume GUID path", GetLastError());
    }
    if (!ParseVolumeGuidPath(name, identity.guid, identity.guidText)) return Fail(L"unexpected volume GUID path");
    const std::wstring volumePath = std::wstring(L"\\\\.\\") + drive + L":";
    Handle volume(CreateFileW(volumePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr));
    if (!volume.valid()) return Fail(L"cannot open the volume", GetLastError());
    NTFS_VOLUME_DATA_BUFFER data{};
    DWORD returned = 0;
    if (!DeviceIoControl(volume.get(), FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &data, sizeof(data), &returned,
                         nullptr)) {
        return Fail(L"cannot get NTFS volume data", GetLastError());
    }
    identity.serial = static_cast<std::uint64_t>(data.VolumeSerialNumber.QuadPart);
    return Ok();
}

StoreResult CheckFolderIsPrivate(const std::wstring& path) {
    Handle h(CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!h.valid()) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return Missing(L"no cache folder");
        return Fail(L"cannot open the cache folder", error);
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(h.get(), FileAttributeTagInfo, &tag, sizeof(tag))) {
        return Fail(L"cannot read the cache folder attributes", GetLastError());
    }
    if (!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return Fail(L"cache folder path is not a folder");
    if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return Fail(L"cache folder is a reparse point");

    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const DWORD error = GetSecurityInfo(h.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                        &owner, nullptr, &dacl, nullptr, &sd);
    if (error != ERROR_SUCCESS) return Fail(L"cannot read the cache folder security", error);
    StoreResult result = Ok();
    if (!IsSystemOrAdministrators(owner)) {
        result = Fail(L"cache folder owner is not SYSTEM or Administrators");
    } else if (dacl == nullptr) {
        result = Fail(L"cache folder has no DACL (everyone has access)");
    } else {
        ACL_SIZE_INFORMATION info{};
        if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) {
            result = Fail(L"cannot read the cache folder DACL", GetLastError());
        } else {
            for (DWORD i = 0; i < info.AceCount && result.ok; ++i) {
                void* ace = nullptr;
                if (!GetAce(dacl, i, &ace)) {
                    result = Fail(L"cannot read a cache folder ACE", GetLastError());
                    break;
                }
                const auto* header = static_cast<const ACE_HEADER*>(ace);
                if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;  // denying never widens access
                if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
                    result = Fail(L"cache folder DACL has an unexpected entry type");
                } else if (!IsSystemOrAdministrators(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart)) {
                    result = Fail(L"cache folder DACL allows someone other than SYSTEM or Administrators");
                }
            }
        }
    }
    LocalFree(sd);
    return result;
}

StoreResult PrepareCacheFolder(bool create, std::wstring& folder) {
    PWSTR programData = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData);
    std::wstring base = SUCCEEDED(hr) && programData != nullptr ? std::wstring(programData) : std::wstring();
    CoTaskMemFree(programData);  // required whether or not the call succeeded
    if (base.empty()) return Fail(L"cannot find the ProgramData folder", static_cast<unsigned long>(hr));
    const std::wstring top = base + L"\\DirSize";
    StoreResult r = EnsureFolder(top, create);
    if (!r.ok) return r;
    folder = top + L"\\cache";
    return EnsureFolder(folder, create);
}

std::wstring CacheFilePath(const std::wstring& folder, const VolumeIdentity& identity) {
    return folder + L"\\" + identity.guidText + L".cache";
}

StoreResult WriteFileReplacing(const std::wstring& path, const std::vector<std::byte>& bytes) {
    const std::wstring temp = path + L".tmp";
    DeleteFileW(temp.c_str());  // left over from an interrupted save; the folder is private
    {
        Handle h(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!h.valid()) return Fail(L"cannot create the temporary cache file", GetLastError());
        size_t at = 0;
        while (at < bytes.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - at, 16u << 20));
            DWORD written = 0;
            if (!WriteFile(h.get(), bytes.data() + at, chunk, &written, nullptr) || written != chunk) {
                const DWORD error = GetLastError();
                return Fail(L"cannot write the temporary cache file", error);
            }
            at += chunk;
        }
        if (!FlushFileBuffers(h.get())) return Fail(L"cannot flush the temporary cache file", GetLastError());
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        DeleteFileW(temp.c_str());
        return Fail(L"cannot replace the cache file", error);
    }
    return Ok();
}

StoreResult ReadWholeFile(const std::wstring& path, std::uint64_t maxBytes, std::vector<std::byte>& bytes) {
    bytes.clear();
    Handle h(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!h.valid()) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return Missing(L"no cache file");
        return Fail(L"cannot open the cache file", error);
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(h.get(), FileAttributeTagInfo, &tag, sizeof(tag))) {
        return Fail(L"cannot read the cache file attributes", GetLastError());
    }
    if (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        return Fail(L"cache file is not a regular file");
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h.get(), &size)) return Fail(L"cannot get the cache file size", GetLastError());
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > maxBytes) {
        return Fail(L"cache file too large");
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t at = 0;
    while (at < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - at, 16u << 20));
        DWORD read = 0;
        if (!ReadFile(h.get(), bytes.data() + at, chunk, &read, nullptr)) {
            const DWORD error = GetLastError();
            bytes.clear();
            return Fail(L"cannot read the cache file", error);
        }
        if (read == 0) {
            bytes.clear();
            return Fail(L"cache file shorter than its size");
        }
        at += read;
    }
    return Ok();
}

}  // namespace dirsize
