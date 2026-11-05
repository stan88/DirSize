// Unit tests for the service's pipe protocol and access checks. No
// elevation needed.
//
// - Protocol: round trips; every truncation and an extra byte are rejected;
//   each length and range check is hit by a hand-made message.
// - Paths: accepted and rejected request paths.
// - Access: a named pipe inside this process, the client at Identification,
//   Impersonation and Anonymous level; AccessCheck on a folder the user may
//   list and on one whose DACL denies listing.

#include <windows.h>
#include <sddl.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "client/pipe_client.h"
#include "core/ntfs_live.h"
#include "service/access.h"
#include "service/volume_worker.h"
#include "service/protocol.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

using namespace dirsize;
using namespace dirsize::proto;
using namespace dirsize::service;

Message MessageOf(const std::vector<std::byte>& b, size_t max = kMaxResponseBytes) {
    Message m;
    CHECK(DecodeMessage(b.data(), b.size(), max, m) == DecodeStatus::Ok);
    return m;
}

template <typename T>
void PutAt(std::vector<std::byte>& b, size_t at, T v) {
    std::memcpy(b.data() + at, &v, sizeof(T));
}

// A message with a raw payload, the length field set to match.
std::vector<std::byte> Raw(MessageType type, const std::vector<std::byte>& payload) {
    std::vector<std::byte> b(kHeaderBytes);
    PutAt<std::uint32_t>(b, 0, kMagic);
    PutAt<std::uint16_t>(b, 4, kVersion);
    PutAt<std::uint16_t>(b, 6, static_cast<std::uint16_t>(type));
    PutAt<std::uint32_t>(b, 8, static_cast<std::uint32_t>(payload.size()));
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

struct Bytes {
    std::vector<std::byte> v;
    template <typename T>
    Bytes& Add(T x) {
        const size_t at = v.size();
        v.resize(at + sizeof(T));
        std::memcpy(v.data() + at, &x, sizeof(T));
        return *this;
    }
    Bytes& Units(const std::wstring& s) {
        for (wchar_t c : s) Add<std::uint16_t>(static_cast<std::uint16_t>(c));
        return *this;
    }
    Bytes& TotalsOf(std::uint64_t n) { return Add(n).Add(n).Add(n).Add(n); }
};

void TestProtocolRoundTrips() {
    {
        const auto b = EncodeStatusRequest();
        CHECK(DecodeStatusRequest(MessageOf(b, kMaxRequestBytes)) == DecodeStatus::Ok);
    }
    {
        QueryFolderRequest q{L"C:\\Users\\x", 40, 100};
        const auto b = EncodeQueryFolder(q);
        QueryFolderRequest back;
        CHECK(DecodeQueryFolder(MessageOf(b, kMaxRequestBytes), back) == DecodeStatus::Ok);
        CHECK(back.path == q.path && back.pageStart == 40 && back.pageSize == 100);
    }
    {
        StatusResponse s;
        s.volumes = {{L'C', VolumeState::Ready, 50u << 20, 3}, {L'D', VolumeState::Updating, 30u << 20, 1}};
        StatusResponse back;
        CHECK(DecodeStatusResponse(MessageOf(EncodeStatusResponse(s)), back) == DecodeStatus::Ok);
        CHECK(back.volumes.size() == 2 && back.volumes[1].drive == L'D' &&
              back.volumes[1].state == VolumeState::Updating && back.volumes[0].memoryBytes == (50u << 20));
    }
    {
        FolderResponse f;
        f.updating = true;
        f.totals = {100, 200, 3, 4};
        f.childCount = 5;
        f.pageStart = 2;
        f.children = {{L"a", {1, 2, 3, 4}, true}, {L"\u6587\u4EF6", {5, 6, 7, 8}, false}};
        FolderResponse back;
        CHECK(DecodeFolderResponse(MessageOf(EncodeFolderResponse(f)), back) == DecodeStatus::Ok);
        CHECK(back.updating && back.totals == f.totals && back.childCount == 5 && back.pageStart == 2 &&
              back.children.size() == 2 && back.children[1].name == L"\u6587\u4EF6" && !back.children[1].accessible &&
              back.children[0].totals == f.children[0].totals);
    }
    {
        ErrorResponse e;
        CHECK(DecodeError(MessageOf(EncodeError(ErrorCode::AccessDenied)), e) == DecodeStatus::Ok);
        CHECK(e.code == ErrorCode::AccessDenied);
    }
}

// Every proper prefix and one extra byte must fail, with the decoder that
// matches the type.
template <typename Decode>
bool AllCutsRejected(const std::vector<std::byte>& good, size_t max, Decode decode) {
    for (size_t n = 0; n < good.size(); ++n) {
        Message m;
        const DecodeStatus s = DecodeMessage(good.data(), n, max, m);
        if (s == DecodeStatus::Ok) return false;  // the length field no longer matches
    }
    // Truncated payloads with a matching length field.
    for (size_t n = kHeaderBytes; n < good.size(); ++n) {
        std::vector<std::byte> b(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
        PutAt<std::uint32_t>(b, 8, static_cast<std::uint32_t>(n - kHeaderBytes));
        Message m;
        if (DecodeMessage(b.data(), b.size(), max, m) != DecodeStatus::Ok) return false;
        if (decode(m) == DecodeStatus::Ok) return false;
    }
    std::vector<std::byte> b = good;
    b.push_back(std::byte{0});
    PutAt<std::uint32_t>(b, 8, static_cast<std::uint32_t>(b.size() - kHeaderBytes));
    Message m;
    return DecodeMessage(b.data(), b.size(), max, m) == DecodeStatus::Ok && decode(m) != DecodeStatus::Ok;
}

void TestProtocolRejects() {
    // Header checks.
    const auto status = EncodeStatusRequest();
    Message m;
    CHECK(DecodeMessage(status.data(), status.size() - 1, kMaxRequestBytes, m) == DecodeStatus::Truncated);
    {
        auto b = status;
        PutAt<std::uint32_t>(b, 0, 0);
        CHECK(DecodeMessage(b.data(), b.size(), kMaxRequestBytes, m) == DecodeStatus::BadMagic);
        b = status;
        PutAt<std::uint16_t>(b, 4, kVersion + 1);
        CHECK(DecodeMessage(b.data(), b.size(), kMaxRequestBytes, m) == DecodeStatus::BadVersion);
        b = status;
        PutAt<std::uint32_t>(b, 8, 1);
        CHECK(DecodeMessage(b.data(), b.size(), kMaxRequestBytes, m) == DecodeStatus::LengthMismatch);
        std::vector<std::byte> big(kMaxRequestBytes + 1);
        CHECK(DecodeMessage(big.data(), big.size(), kMaxRequestBytes, m) == DecodeStatus::TooLarge);
    }
    // A request decoded as the wrong type.
    CHECK(DecodeStatusRequest(MessageOf(EncodeError(ErrorCode::Busy))) == DecodeStatus::Malformed);
    {
        QueryFolderRequest q;
        CHECK(DecodeQueryFolder(MessageOf(status, kMaxRequestBytes), q) == DecodeStatus::Malformed);
        CHECK(DecodeStatusRequest(MessageOf(Raw(MessageType::StatusRequest, Bytes().Add<std::uint8_t>(0).v),
                                            kMaxRequestBytes)) == DecodeStatus::Malformed);
    }

    // Truncations and extra bytes for every type.
    CHECK(AllCutsRejected(EncodeQueryFolder({L"C:\\a", 0, 10}), kMaxRequestBytes, [](const Message& x) {
        QueryFolderRequest q;
        return DecodeQueryFolder(x, q);
    }));
    StatusResponse s;
    s.volumes = {{L'C', VolumeState::Ready, 1, 2}};
    CHECK(AllCutsRejected(EncodeStatusResponse(s), kMaxResponseBytes, [](const Message& x) {
        StatusResponse r;
        return DecodeStatusResponse(x, r);
    }));
    FolderResponse f;
    f.childCount = 2;
    f.children = {{L"ab", {}, true}, {L"c", {}, false}};
    CHECK(AllCutsRejected(EncodeFolderResponse(f), kMaxResponseBytes, [](const Message& x) {
        FolderResponse r;
        return DecodeFolderResponse(x, r);
    }));
    CHECK(AllCutsRejected(EncodeError(ErrorCode::NotFound), kMaxResponseBytes, [](const Message& x) {
        ErrorResponse r;
        return DecodeError(x, r);
    }));

    // Range checks in QueryFolder.
    auto query = [](std::uint32_t units, const std::wstring& path, std::uint32_t start, std::uint32_t size) {
        QueryFolderRequest q;
        return DecodeQueryFolder(MessageOf(Raw(MessageType::QueryFolderRequest,
                                               Bytes().Add(units).Units(path).Add(start).Add(size).v),
                                           kMaxRequestBytes),
                                 q);
    };
    CHECK(query(1, L"C", 0, 1) == DecodeStatus::Ok);
    CHECK(query(0, L"", 0, 1) == DecodeStatus::Malformed);  // empty path
    CHECK(query(1, L"C", 0, 0) == DecodeStatus::Malformed);  // page size 0
    CHECK(query(1, L"C", 0, kMaxPageSize + 1) == DecodeStatus::Malformed);
    CHECK(query(2, L"C", 0, 1) == DecodeStatus::Malformed);  // count larger than the units present
    {
        QueryFolderRequest q;
        const auto b = Raw(MessageType::QueryFolderRequest, Bytes().Add<std::uint32_t>(kMaxPathUnits + 1).v);
        CHECK(DecodeQueryFolder(MessageOf(b, kMaxRequestBytes), q) == DecodeStatus::Malformed);
        // One unit over the limit with every unit present (decoded with the
        // larger response limit so the size check does not stop it first).
        const auto c = Raw(MessageType::QueryFolderRequest, Bytes()
                                                                .Add<std::uint32_t>(kMaxPathUnits + 1)
                                                                .Units(std::wstring(kMaxPathUnits + 1, L'a'))
                                                                .Add<std::uint32_t>(0)
                                                                .Add<std::uint32_t>(1)
                                                                .v);
        CHECK(DecodeQueryFolder(MessageOf(c, kMaxResponseBytes), q) == DecodeStatus::Malformed);
        Message tooBig;
        CHECK(DecodeMessage(c.data(), c.size(), kMaxRequestBytes, tooBig) == DecodeStatus::TooLarge);
    }
    // Range checks in StatusResponse.
    auto statusOf = [](std::uint32_t count, std::uint16_t drive, std::uint8_t state) {
        Bytes p;
        p.Add(count);
        for (std::uint32_t i = 0; i < count && i < 30; ++i) p.Add(drive).Add(state).Add<std::uint64_t>(0).Add<std::uint64_t>(0);
        StatusResponse r;
        return DecodeStatusResponse(MessageOf(Raw(MessageType::StatusResponse, p.v)), r);
    };
    CHECK(statusOf(1, L'C', 1) == DecodeStatus::Ok);
    CHECK(statusOf(1, L'c', 1) == DecodeStatus::Malformed);
    CHECK(statusOf(1, L'C', 0) == DecodeStatus::Malformed);
    CHECK(statusOf(1, L'C', 4) == DecodeStatus::Malformed);
    CHECK(statusOf(27, L'C', 1) == DecodeStatus::Malformed);
    // Range checks in FolderResponse.
    auto folderOf = [](std::uint8_t updating, std::uint32_t childCount, std::uint32_t pageStart, std::uint32_t count,
                       std::uint16_t units, std::uint8_t accessible) {
        Bytes p;
        p.Add(updating).TotalsOf(1).Add(childCount).Add(pageStart).Add(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            p.Add(units).Units(std::wstring(units, L'x')).TotalsOf(2).Add(accessible);
        }
        FolderResponse r;
        return DecodeFolderResponse(MessageOf(Raw(MessageType::FolderResponse, p.v)), r);
    };
    CHECK(folderOf(0, 1, 0, 1, 3, 1) == DecodeStatus::Ok);
    CHECK(folderOf(2, 1, 0, 1, 3, 1) == DecodeStatus::Malformed);  // updating flag
    CHECK(folderOf(0, 1, 1, 1, 3, 1) == DecodeStatus::Malformed);  // page beyond childCount
    CHECK(folderOf(0, 1, 0, 1, 0, 1) == DecodeStatus::Malformed);  // empty name
    CHECK(folderOf(0, 1, 0, 1, static_cast<std::uint16_t>(kMaxNameUnits + 1), 1) == DecodeStatus::Malformed);
    CHECK(folderOf(0, 1, 0, 1, 3, 2) == DecodeStatus::Malformed);  // accessible flag
    CHECK(folderOf(0, kMaxPageSize + 1, 0, kMaxPageSize + 1, 1, 1) == DecodeStatus::Malformed);
    // Error codes.
    auto errorOf = [](std::uint32_t code) {
        ErrorResponse r;
        return DecodeError(MessageOf(Raw(MessageType::ErrorResponse, Bytes().Add(code).v)), r);
    };
    CHECK(errorOf(1) == DecodeStatus::Ok && errorOf(8) == DecodeStatus::Ok);
    CHECK(errorOf(0) == DecodeStatus::Malformed && errorOf(9) == DecodeStatus::Malformed);
}

void TestPaths() {
    for (const wchar_t* ok : {L"C:\\", L"c:\\", L"C:\\Users", L"C:\\Users\\", L"D:\\a b\\c.txt", L"C:\\\u6587\u4EF6"}) {
        CHECK(CheckRequestPath(ok) == PathVerdict::Ok);
    }
    CHECK(CheckRequestPath(L"") == PathVerdict::Empty);
    CHECK(CheckRequestPath(std::wstring(32768, L'a')) == PathVerdict::TooLong);
    for (const wchar_t* bad : {L"C:", L"C:a", L"C", L"1:\\", L"\\\\server\\share", L"\\\\?\\C:\\", L"\\\\.\\C:",
                               L"\\\\?\\GLOBALROOT\\Device", L"\\Users", L"Users\\x", L"/C/x"}) {
        CHECK(CheckRequestPath(bad) == PathVerdict::NotDriveAbsolute);
    }
    for (const wchar_t* bad : {L"C:\\a\\\\b", L"C:\\a\\.\\b", L"C:\\a\\..", L"C:\\a\\..\\b", L"C:\\a.", L"C:\\a ",
                               L"C:\\\\"}) {
        CHECK(CheckRequestPath(bad) == PathVerdict::BadSegment);
    }
    for (const wchar_t* bad : {L"C:\\a:b", L"C:\\a*", L"C:\\a?", L"C:\\a/b", L"C:\\a|b", L"C:\\a\"", L"C:\\a<",
                               L"C:\\a>"}) {
        CHECK(CheckRequestPath(bad) == PathVerdict::ForbiddenCharacter);
    }
    CHECK(CheckRequestPath(std::wstring(L"C:\\a") + L'\x01') == PathVerdict::ForbiddenCharacter);
    CHECK(CheckRequestPath(std::wstring(L"C:\\a\0b", 6)) == PathVerdict::ForbiddenCharacter);
}

// ---- access through a real pipe ------------------------------------------------------

std::wstring CurrentUserSid() {
    HANDLE token = nullptr;
    std::wstring out;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return out;
    std::vector<std::uint8_t> buffer(256);
    DWORD size = 0;
    if (GetTokenInformation(token, TokenUser, buffer.data(), static_cast<DWORD>(buffer.size()), &size)) {
        wchar_t* text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
            out = text;
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return out;
}

struct PipeOutcome {
    DWORD tokenError = 0xFFFFFFFF;
    SECURITY_IMPERSONATION_LEVEL level = SecurityAnonymous;
    bool mayListAllowed = false, mayListDenied = true;
    DWORD deniedError = 0;
    bool stillSelf = false;  // the server thread is not impersonating afterwards
};

// The client connects with `sqos`; if `readFirst` is false the server asks for
// the token before reading any message.
PipeOutcome RunPipe(DWORD sqos, bool readFirst, const std::wstring& allowed, const std::wstring& denied) {
    static int counter = 0;
    const std::wstring name =
        L"\\\\.\\pipe\\dirsize-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++counter);
    HANDLE server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                     PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                     1, 4096, 4096, 0, nullptr);
    PipeOutcome out;
    CHECK(server != INVALID_HANDLE_VALUE);
    if (server == INVALID_HANDLE_VALUE) return out;
    std::thread t([&] {
        if (!ConnectNamedPipe(server, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return;
        char message[16];
        DWORD n = 0;
        if (readFirst) ReadFile(server, message, sizeof(message), &n, nullptr);
        ClientToken token;
        out.tokenError = token.FromPipe(server);
        out.level = token.level();
        HANDLE self = nullptr;
        out.stillSelf = !OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &self) &&
                        GetLastError() == ERROR_NO_TOKEN;
        if (self) CloseHandle(self);
        DWORD error = 0;
        for (const auto& [path, result] : {std::pair{allowed, &out.mayListAllowed}, std::pair{denied, &out.mayListDenied}}) {
            HANDLE folder = OpenFolderAsService(path, error);
            CHECK(folder != INVALID_HANDLE_VALUE);
            if (folder == INVALID_HANDLE_VALUE) continue;
            *result = ClientMayList(token.get(), folder, error);
            if (result == &out.mayListDenied) out.deniedError = error;
            CloseHandle(folder);
        }
        if (!readFirst) ReadFile(server, message, sizeof(message), &n, nullptr);
        WriteFile(server, "ok", 2, &n, nullptr);
        FlushFileBuffers(server);
        DisconnectNamedPipe(server);
    });
    HANDLE client = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 200 && client == INVALID_HANDLE_VALUE; ++i) {
        client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                             SECURITY_SQOS_PRESENT | sqos, nullptr);
        if (client == INVALID_HANDLE_VALUE) Sleep(5);
    }
    CHECK(client != INVALID_HANDLE_VALUE);
    if (client != INVALID_HANDLE_VALUE) {
        DWORD n = 0;
        WriteFile(client, "hi", 2, &n, nullptr);
        char reply[8];
        ReadFile(client, reply, sizeof(reply), &n, nullptr);
        CloseHandle(client);
    }
    t.join();
    CloseHandle(server);
    return out;
}

void TestAccessThroughPipe() {
    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, temp);
    const std::wstring base = std::wstring(temp) + L"dirsize-access-test-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring allowed = base + L"-allowed";
    const std::wstring denied = base + L"-denied";
    CHECK(CreateDirectoryW(allowed.c_str(), nullptr));
    // Everything but listing for this user; READ_CONTROL stays (owner), so the
    // service side can read the descriptor.
    const std::wstring sid = CurrentUserSid();
    const std::wstring sddl = L"D:P(D;;0x1;;;" + sid + L")(A;;FA;;;" + sid + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr));
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    CHECK(CreateDirectoryW(denied.c_str(), &sa));
    LocalFree(sd);

    for (DWORD sqos : {SECURITY_IDENTIFICATION, SECURITY_IMPERSONATION}) {
        const PipeOutcome o = RunPipe(sqos, true, allowed, denied);
        CHECK(o.tokenError == ERROR_SUCCESS);
        CHECK(o.level == (sqos == SECURITY_IDENTIFICATION ? SecurityIdentification : SecurityImpersonation));
        CHECK(o.stillSelf);
        CHECK(o.mayListAllowed);
        CHECK(!o.mayListDenied && o.deniedError == ERROR_ACCESS_DENIED);
    }
    {  // Anonymous: no usable token, so nothing is allowed.
        const PipeOutcome o = RunPipe(SECURITY_ANONYMOUS, true, allowed, denied);
        CHECK(o.tokenError != ERROR_SUCCESS);
        CHECK(o.stillSelf);
        CHECK(!o.mayListAllowed && !o.mayListDenied);
    }
    {  // Measured: the client's token is available right after the connect,
       // before any message is read (the documentation only says it is that
       // of "the last message read"). It is the connected client either way.
        const PipeOutcome o = RunPipe(SECURITY_IDENTIFICATION, false, allowed, denied);
        CHECK(o.tokenError == ERROR_SUCCESS && o.level == SecurityIdentification);
        CHECK(o.stillSelf);
        CHECK(o.mayListAllowed && !o.mayListDenied);
    }
    CHECK(UsableClientToken(TokenImpersonation, SecurityIdentification));
    CHECK(UsableClientToken(TokenImpersonation, SecurityImpersonation));
    CHECK(!UsableClientToken(TokenImpersonation, SecurityAnonymous));
    CHECK(!UsableClientToken(TokenPrimary, SecurityImpersonation));
    {  // No token at all.
        DWORD error = 0;
        HANDLE folder = OpenFolderAsService(allowed, error);
        CHECK(folder != INVALID_HANDLE_VALUE);
        CHECK(!ClientMayList(nullptr, folder, error) && error == ERROR_NO_TOKEN);
        if (folder != INVALID_HANDLE_VALUE) CloseHandle(folder);
    }
    {  // A file is not a folder.
        const std::wstring file = allowed + L"\\f.txt";
        HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        DWORD error = 0;
        CHECK(OpenFolderAsService(file, error) == INVALID_HANDLE_VALUE && error == ERROR_DIRECTORY);
        DeleteFileW(file.c_str());
    }
    {  // The volume GUID of a folder handle.
        DWORD error = 0;
        HANDLE folder = OpenFolderAsService(allowed, error);
        std::wstring guid;
        CHECK(folder != INVALID_HANDLE_VALUE && VolumeGuidOfHandle(folder, guid) && guid.size() == 36);
        if (folder != INVALID_HANDLE_VALUE) CloseHandle(folder);
    }
    RemoveDirectoryW(allowed.c_str());
    RemoveDirectoryW(denied.c_str());
}

// An ordinary user gets only the Authenticated Users entry of the service's
// pipe DACL: read and write without FILE_CREATE_PIPE_INSTANCE (0x0012019B).
// The client must be able to connect and talk with exactly that.
void TestPipeAccessOfOrdinaryUsers() {
    const std::wstring sid = CurrentUserSid();
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;0x0012019B;;;" + sid + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr));
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    const std::wstring name = L"\\\\.\\pipe\\dirsize-test-acl-" + std::to_wstring(GetCurrentProcessId());
    HANDLE server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                     PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                     1, 65536, 65536, 0, &sa);
    LocalFree(sd);
    CHECK(server != INVALID_HANDLE_VALUE);
    if (server == INVALID_HANDLE_VALUE) return;
    std::atomic<bool> done{false};
    std::thread t([&] {
        while (!done) {
            if (!ConnectNamedPipe(server, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
                DisconnectNamedPipe(server);  // e.g. the client closed before the connect: wait again
                continue;
            }
            std::vector<std::byte> request(kMaxRequestBytes);
            DWORD n = 0;
            if (ReadFile(server, request.data(), static_cast<DWORD>(request.size()), &n, nullptr)) {
                const auto response = EncodeStatusResponse({});
                WriteFile(server, response.data(), static_cast<DWORD>(response.size()), &n, nullptr);
                FlushFileBuffers(server);
            }
            DisconnectNamedPipe(server);
        }
    });

    // Measured: GENERIC_READ | GENERIC_WRITE also connects with this entry
    // (GENERIC_WRITE contains FILE_APPEND_DATA, the same bit as
    // FILE_CREATE_PIPE_INSTANCE, so this was checked rather than assumed).
    // The client still asks only for the specific rights it uses.
    for (DWORD access : {static_cast<DWORD>(GENERIC_READ | GENERIC_WRITE), dirsize::client::kClientPipeAccess}) {
        HANDLE h = INVALID_HANDLE_VALUE;
        for (int i = 0; i < 100 && h == INVALID_HANDLE_VALUE; ++i) {
            h = CreateFileW(name.c_str(), access, 0, nullptr, OPEN_EXISTING,
                            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            if (h == INVALID_HANDLE_VALUE && GetLastError() != ERROR_PIPE_BUSY) break;
            if (h == INVALID_HANDLE_VALUE) Sleep(10);
        }
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    {
        dirsize::client::PipeClient client;
        dirsize::client::ClientError e;
        CHECK(client.Connect(false, e, name.c_str()));
        if (!e.what.empty()) std::printf("  PipeClient: %ls (error %lu)\n", e.what.c_str(), e.win32);
        StatusResponse status;
        ErrorResponse serverError;
        CHECK(client.Status(status, serverError, e));
    }
    done = true;
    // Unblock ConnectNamedPipe with one more connection.
    HANDLE last = CreateFileW(name.c_str(), dirsize::client::kClientPipeAccess, 0, nullptr, OPEN_EXISTING,
                              SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (last != INVALID_HANDLE_VALUE) CloseHandle(last);
    t.join();
    CloseHandle(server);
}

void TestRescanBackoffAndTrackerCap() {
    using std::chrono::seconds;
    CHECK(RescanBackoff(0) == seconds(0));
    CHECK(RescanBackoff(1) == seconds(60));
    CHECK(RescanBackoff(2) == seconds(120));
    CHECK(RescanBackoff(3) == seconds(240));
    CHECK(RescanBackoff(4) == seconds(480));
    CHECK(RescanBackoff(5) == seconds(600));
    CHECK(RescanBackoff(50) == seconds(600));
    for (int n = 1; n < 40; ++n) CHECK(RescanBackoff(n + 1) >= RescanBackoff(n));

    dirsize::OpenFileTracker tracker;
    for (std::uint64_t id = 1; id <= 2500; ++id) tracker.Track(id);
    CHECK(tracker.TrimTo(dirsize::kMaxTrackedOpenFiles) == 2500 - dirsize::kMaxTrackedOpenFiles);
    CHECK(tracker.Size() == dirsize::kMaxTrackedOpenFiles);
    CHECK(tracker.TrimTo(dirsize::kMaxTrackedOpenFiles) == 0);
}

}  // namespace

int main() {
    TestProtocolRoundTrips();
    TestProtocolRejects();
    TestPaths();
    TestAccessThroughPipe();
    TestPipeAccessOfOrdinaryUsers();
    TestRescanBackoffAndTrackerCap();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
