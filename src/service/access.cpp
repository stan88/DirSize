#include "service/access.h"

#include <aclapi.h>

#include <cstdlib>
#include <vector>

#include "core/cache_store.h"

namespace dirsize::service {
namespace {

bool Forbidden(wchar_t c) {
    return c < 0x20 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'|' || c == L'?' ||
           c == L'*';
}

}  // namespace

PathVerdict CheckRequestPath(std::wstring_view path) {
    if (path.empty()) return PathVerdict::Empty;
    if (path.size() > 32767) return PathVerdict::TooLong;
    const wchar_t d = path[0];
    if (path.size() < 3 || !((d >= L'A' && d <= L'Z') || (d >= L'a' && d <= L'z')) || path[1] != L':' ||
        path[2] != L'\\') {
        return PathVerdict::NotDriveAbsolute;
    }
    std::wstring_view rest = path.substr(3);
    if (rest.size() > 1 && rest.back() == L'\\') rest.remove_suffix(1);  // one trailing backslash; "X:\\" stays bad
    if (rest.empty()) return PathVerdict::Ok;                          // the root
    size_t start = 0;
    for (;;) {
        const size_t end = rest.find(L'\\', start);
        const std::wstring_view segment = rest.substr(start, end == std::wstring_view::npos ? rest.size() - start
                                                                                              : end - start);
        if (segment.empty() || segment == L"." || segment == L".." || segment.back() == L' ' ||
            segment.back() == L'.') {
            return PathVerdict::BadSegment;
        }
        for (wchar_t c : segment) {
            if (Forbidden(c)) return PathVerdict::ForbiddenCharacter;
        }
        if (end == std::wstring_view::npos) return PathVerdict::Ok;
        start = end + 1;
    }
}

const wchar_t* PathVerdictName(PathVerdict verdict) {
    switch (verdict) {
        case PathVerdict::Ok: return L"ok";
        case PathVerdict::Empty: return L"empty";
        case PathVerdict::TooLong: return L"too long";
        case PathVerdict::NotDriveAbsolute: return L"not a drive-letter path";
        case PathVerdict::ForbiddenCharacter: return L"forbidden character";
        case PathVerdict::BadSegment: return L"bad path segment";
    }
    return L"?";
}

bool UsableClientToken(TOKEN_TYPE type, SECURITY_IMPERSONATION_LEVEL level) {
    return type == TokenImpersonation && level >= SecurityIdentification;
}

ClientToken::~ClientToken() {
    if (token_ != nullptr) CloseHandle(token_);
}

DWORD ClientToken::FromPipe(HANDLE pipe) {
    if (token_ != nullptr) {
        CloseHandle(token_);
        token_ = nullptr;
    }
    level_ = SecurityAnonymous;
    if (!ImpersonateNamedPipeClient(pipe)) return GetLastError();
    // Whatever happens below, the thread stops impersonating before we return.
    // Continuing as the client would be wrong; failing to revert is fatal.
    struct Revert {
        ~Revert() {
            if (!RevertToSelf()) std::abort();
        }
    } revert;
    HANDLE token = nullptr;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) return GetLastError();
    TOKEN_TYPE type{};
    SECURITY_IMPERSONATION_LEVEL level = SecurityAnonymous;
    DWORD size = 0;
    if (!GetTokenInformation(token, TokenType, &type, sizeof(type), &size) ||
        !GetTokenInformation(token, TokenImpersonationLevel, &level, sizeof(level), &size)) {
        const DWORD error = GetLastError();
        CloseHandle(token);
        return error;
    }
    if (!UsableClientToken(type, level)) {
        CloseHandle(token);
        return ERROR_BAD_IMPERSONATION_LEVEL;
    }
    token_ = token;
    level_ = level;
    return ERROR_SUCCESS;
}

HANDLE OpenFolderAsService(const std::wstring& path, DWORD& error) {
    HANDLE h = CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    error = h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (h != INVALID_HANDLE_VALUE) {
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag))) {
            error = GetLastError();
        } else if (!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            error = ERROR_DIRECTORY;
        }
        if (error != ERROR_SUCCESS) {
            CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
        }
    }
    return h;
}

bool ClientMayList(HANDLE clientToken, HANDLE folder, DWORD& error) {
    error = ERROR_SUCCESS;
    if (clientToken == nullptr) {
        error = ERROR_NO_TOKEN;
        return false;
    }
    PSECURITY_DESCRIPTOR sd = nullptr;
    error = GetSecurityInfo(folder, SE_FILE_OBJECT,
                            OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                            nullptr, nullptr, nullptr, nullptr, &sd);
    if (error != ERROR_SUCCESS) return false;
    GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    DWORD desired = FILE_LIST_DIRECTORY;
    MapGenericMask(&desired, &mapping);
    std::vector<std::uint8_t> privileges(1024);
    DWORD privilegesSize = static_cast<DWORD>(privileges.size());
    DWORD granted = 0;
    BOOL status = FALSE;
    const BOOL ok = AccessCheck(sd, clientToken, desired, &mapping,
                                reinterpret_cast<PPRIVILEGE_SET>(privileges.data()), &privilegesSize, &granted,
                                &status);
    if (!ok) error = GetLastError();
    LocalFree(sd);
    if (!ok) return false;
    if (!status || (granted & FILE_LIST_DIRECTORY) == 0) {
        error = ERROR_ACCESS_DENIED;
        return false;
    }
    return true;
}

bool VolumeGuidOfHandle(HANDLE handle, std::wstring& guidText) {
    std::wstring path(1024, L'\0');
    const DWORD n = GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()),
                                              FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
    if (n == 0 || n >= path.size()) return false;
    path.resize(n);
    // "\\?\Volume{GUID}\rest": keep "\\?\Volume{GUID}\" for the parser.
    const size_t close = path.find(L'}');
    if (close == std::wstring::npos || close + 1 >= path.size() || path[close + 1] != L'\\') return false;
    std::uint8_t guid[16];
    return ParseVolumeGuidPath(path.substr(0, close + 2), guid, guidText);
}

}  // namespace dirsize::service
