#pragma once

// Named pipe protocol between the DirSize service and its clients
// (docs/DESIGN-phase4.md section 6). Pure byte-buffer functions.
//
// Every message: magic "DSZP" (u32), version (u16), type (u16), payload
// length (u32), then the payload. Little-endian. Strings are a u32 or u16
// count of UTF-16 units followed by the units. A message is one pipe message
// (message mode), so the payload length must match the bytes received.
//
// Everything a peer sends is untrusted: decoders check every length against
// the bytes present and the limits below, and reject trailing bytes.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/index.h"

namespace dirsize::proto {

constexpr std::uint32_t kMagic = 0x505A5344u;  // "DSZP"
constexpr std::uint16_t kVersion = 1;
constexpr size_t kHeaderBytes = 12;
constexpr std::uint32_t kMaxPathUnits = 32767;
constexpr size_t kMaxRequestBytes = kHeaderBytes + 4 + kMaxPathUnits * 2 + 8;
constexpr size_t kMaxResponseBytes = 1u << 20;
constexpr std::uint32_t kMaxPageSize = 1000;
constexpr std::uint32_t kMaxNameUnits = 1024;  // NTFS names are at most 255

enum class MessageType : std::uint16_t {
    StatusRequest = 1,
    QueryFolderRequest = 2,
    StatusResponse = 101,
    FolderResponse = 102,
    ErrorResponse = 199,
};

enum class ErrorCode : std::uint32_t {
    BadRequest = 1,
    UnsupportedVersion = 2,
    PathRejected = 3,   // not a local drive-letter path
    NotFound = 4,
    AccessDenied = 5,   // the caller may not list the folder
    NotIndexed = 6,     // the volume has no index (front end walks it itself)
    Busy = 7,
    Internal = 8,
};

enum class VolumeState : std::uint8_t {
    Ready = 1,
    Updating = 2,  // a rescan is running; answers come from the previous index
    NoIndex = 3,
};

struct QueryFolderRequest {
    std::wstring path;
    std::uint32_t pageStart = 0;
    std::uint32_t pageSize = 0;  // 1..kMaxPageSize
};

struct VolumeStatus {
    wchar_t drive = 0;
    VolumeState state = VolumeState::NoIndex;
    std::uint64_t memoryBytes = 0;
    std::uint64_t secondsSinceCatchUp = 0;
};

struct StatusResponse {
    std::vector<VolumeStatus> volumes;
};

struct ChildFolder {
    std::wstring name;
    Totals totals;
    bool accessible = true;  // false: the caller may not list it (shown as "no access")
};

struct FolderResponse {
    bool updating = false;
    Totals totals;
    std::uint32_t childCount = 0;  // all child folders, not only this page
    std::uint32_t pageStart = 0;
    std::vector<ChildFolder> children;
};

struct ErrorResponse {
    ErrorCode code = ErrorCode::Internal;
};

std::vector<std::byte> EncodeStatusRequest();
std::vector<std::byte> EncodeQueryFolder(const QueryFolderRequest& request);
std::vector<std::byte> EncodeStatusResponse(const StatusResponse& response);
std::vector<std::byte> EncodeFolderResponse(const FolderResponse& response);
std::vector<std::byte> EncodeError(ErrorCode code);

enum class DecodeStatus {
    Ok,
    Truncated,      // shorter than the header
    BadMagic,
    BadVersion,
    TooLarge,       // over the limit for this direction
    LengthMismatch, // payload length differs from the bytes received
    Malformed,      // payload does not parse, or values out of range
};

struct Message {
    MessageType type = MessageType::ErrorResponse;
    const std::byte* payload = nullptr;
    size_t payloadBytes = 0;
};

// Checks the header of one received message. `maxBytes` is kMaxRequestBytes
// on the server and kMaxResponseBytes on the client.
DecodeStatus DecodeMessage(const std::byte* data, size_t size, size_t maxBytes, Message& message);

// Payload decoders; each needs the matching message type.
DecodeStatus DecodeQueryFolder(const Message& message, QueryFolderRequest& request);
DecodeStatus DecodeStatusRequest(const Message& message);
DecodeStatus DecodeStatusResponse(const Message& message, StatusResponse& response);
DecodeStatus DecodeFolderResponse(const Message& message, FolderResponse& response);
DecodeStatus DecodeError(const Message& message, ErrorResponse& response);

}  // namespace dirsize::proto
