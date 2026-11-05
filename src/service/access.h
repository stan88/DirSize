#pragma once

// Deciding whether a pipe client may see a folder (docs/DESIGN-phase4.md
// section 6.1, decision 8.5).
//
// The client connects at SecurityIdentification level. The service reads the
// client's token (ClientToken::FromPipe), opens the folder as itself, and
// asks AccessCheck whether the token is allowed FILE_LIST_DIRECTORY on the
// folder's security descriptor. Every failure on the way denies access: the
// service runs as SYSTEM, so falling back to its own rights is never an
// option.

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace dirsize::service {

enum class PathVerdict {
    Ok,
    Empty,
    TooLong,
    NotDriveAbsolute,    // must start with "X:\" (no UNC, \\?\, \\.\, relative paths)
    ForbiddenCharacter,  // control characters, < > : " / | ? *
    BadSegment,          // empty, ".", "..", or ending in a space or a dot
};

// Checks a path from a request before it is opened. Accepts "X:\" and
// "X:\a\b" (one trailing backslash allowed). The access check on the opened
// folder is what protects; this keeps paths canonical and local.
PathVerdict CheckRequestPath(std::wstring_view path);
const wchar_t* PathVerdictName(PathVerdict verdict);

// The token ClientToken accepts: an impersonation token at Identification
// level or higher. (An anonymous token cannot even be opened, so the level
// check is a second line; exposed for tests.)
bool UsableClientToken(TOKEN_TYPE type, SECURITY_IMPERSONATION_LEVEL level);

class ClientToken {
public:
    ClientToken() = default;
    ~ClientToken();
    ClientToken(const ClientToken&) = delete;
    ClientToken& operator=(const ClientToken&) = delete;

    // After at least one message was read from `pipe`: impersonates the
    // client, opens the thread token (OpenAsSelf), reverts. The token must be
    // an impersonation token at Identification level or higher. Returns
    // ERROR_SUCCESS or the error that denied it.
    DWORD FromPipe(HANDLE pipe);

    HANDLE get() const { return token_; }
    SECURITY_IMPERSONATION_LEVEL level() const { return level_; }

private:
    HANDLE token_ = nullptr;
    SECURITY_IMPERSONATION_LEVEL level_ = SecurityAnonymous;
};

// Opens a folder as the service, to read its security descriptor, file ID
// and volume. Follows reparse points like an ordinary open.
HANDLE OpenFolderAsService(const std::wstring& path, DWORD& error);

// Whether `clientToken` may list `folder` (FILE_LIST_DIRECTORY). On any
// failure returns false with `error` set.
bool ClientMayList(HANDLE clientToken, HANDLE folder, DWORD& error);

// "01234567-89ab-..." of the volume an open handle is on
// (GetFinalPathNameByHandleW with VOLUME_NAME_GUID).
bool VolumeGuidOfHandle(HANDLE handle, std::wstring& guidText);

}  // namespace dirsize::service
