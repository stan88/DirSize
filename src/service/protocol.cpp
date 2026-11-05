#include "service/protocol.h"

#include <cstring>

namespace dirsize::proto {
namespace {

class Writer {
public:
    explicit Writer(MessageType type) {
        U32(kMagic);
        U16(kVersion);
        U16(static_cast<std::uint16_t>(type));
        U32(0);  // payload length, set by Finish
    }
    void U8(std::uint8_t v) { Raw(&v, 1); }
    void U16(std::uint16_t v) { Raw(&v, 2); }
    void U32(std::uint32_t v) { Raw(&v, 4); }
    void U64(std::uint64_t v) { Raw(&v, 8); }
    void Units(const std::wstring& s) {
        static_assert(sizeof(wchar_t) == 2);
        Raw(s.data(), s.size() * 2);
    }
    void TotalsOf(const Totals& t) {
        U64(t.logicalBytes);
        U64(t.allocatedBytes);
        U64(t.files);
        U64(t.folders);
    }
    std::vector<std::byte> Finish() {
        const auto payload = static_cast<std::uint32_t>(out_.size() - kHeaderBytes);
        std::memcpy(out_.data() + 8, &payload, 4);
        return std::move(out_);
    }

private:
    void Raw(const void* p, size_t n) {
        const size_t at = out_.size();
        out_.resize(at + n);
        if (n != 0) std::memcpy(out_.data() + at, p, n);
    }
    std::vector<std::byte> out_;
};

class Reader {
public:
    Reader(const std::byte* p, size_t n) : p_(p), n_(n) {}
    bool U8(std::uint8_t& v) { return Raw(&v, 1); }
    bool U16(std::uint16_t& v) { return Raw(&v, 2); }
    bool U32(std::uint32_t& v) { return Raw(&v, 4); }
    bool U64(std::uint64_t& v) { return Raw(&v, 8); }
    bool Units(size_t count, std::wstring& s) {
        if (count > (n_ - at_) / 2) return false;
        s.resize(count);
        return Raw(s.data(), count * 2);
    }
    bool TotalsOf(Totals& t) { return U64(t.logicalBytes) && U64(t.allocatedBytes) && U64(t.files) && U64(t.folders); }
    bool AtEnd() const { return at_ == n_; }
    size_t Remaining() const { return n_ - at_; }

private:
    bool Raw(void* out, size_t n) {
        if (n > n_ - at_) return false;
        if (n != 0) std::memcpy(out, p_ + at_, n);
        at_ += n;
        return true;
    }
    const std::byte* p_;
    size_t n_;
    size_t at_ = 0;
};

bool ValidState(std::uint8_t s) { return s >= 1 && s <= 3; }

}  // namespace

std::vector<std::byte> EncodeStatusRequest() { return Writer(MessageType::StatusRequest).Finish(); }

std::vector<std::byte> EncodeQueryFolder(const QueryFolderRequest& request) {
    Writer w(MessageType::QueryFolderRequest);
    w.U32(static_cast<std::uint32_t>(request.path.size()));
    w.Units(request.path);
    w.U32(request.pageStart);
    w.U32(request.pageSize);
    return w.Finish();
}

std::vector<std::byte> EncodeStatusResponse(const StatusResponse& response) {
    Writer w(MessageType::StatusResponse);
    w.U32(static_cast<std::uint32_t>(response.volumes.size()));
    for (const VolumeStatus& v : response.volumes) {
        w.U16(static_cast<std::uint16_t>(v.drive));
        w.U8(static_cast<std::uint8_t>(v.state));
        w.U64(v.memoryBytes);
        w.U64(v.secondsSinceCatchUp);
    }
    return w.Finish();
}

std::vector<std::byte> EncodeFolderResponse(const FolderResponse& response) {
    Writer w(MessageType::FolderResponse);
    w.U8(response.updating ? 1 : 0);
    w.TotalsOf(response.totals);
    w.U32(response.childCount);
    w.U32(response.pageStart);
    w.U32(static_cast<std::uint32_t>(response.children.size()));
    for (const ChildFolder& c : response.children) {
        w.U16(static_cast<std::uint16_t>(c.name.size()));
        w.Units(c.name);
        w.TotalsOf(c.totals);
        w.U8(c.accessible ? 1 : 0);
    }
    return w.Finish();
}

std::vector<std::byte> EncodeError(ErrorCode code) {
    Writer w(MessageType::ErrorResponse);
    w.U32(static_cast<std::uint32_t>(code));
    return w.Finish();
}

DecodeStatus DecodeMessage(const std::byte* data, size_t size, size_t maxBytes, Message& message) {
    message = Message{};
    if (data == nullptr || size < kHeaderBytes) return DecodeStatus::Truncated;
    if (size > maxBytes) return DecodeStatus::TooLarge;
    Reader r(data, size);
    std::uint32_t magic = 0, length = 0;
    std::uint16_t version = 0, type = 0;
    r.U32(magic);
    r.U16(version);
    r.U16(type);
    r.U32(length);
    if (magic != kMagic) return DecodeStatus::BadMagic;
    if (version != kVersion) return DecodeStatus::BadVersion;
    if (length != size - kHeaderBytes) return DecodeStatus::LengthMismatch;
    message.type = static_cast<MessageType>(type);
    message.payload = data + kHeaderBytes;
    message.payloadBytes = length;
    return DecodeStatus::Ok;
}

DecodeStatus DecodeStatusRequest(const Message& message) {
    if (message.type != MessageType::StatusRequest || message.payloadBytes != 0) return DecodeStatus::Malformed;
    return DecodeStatus::Ok;
}

DecodeStatus DecodeQueryFolder(const Message& message, QueryFolderRequest& request) {
    request = QueryFolderRequest{};
    if (message.type != MessageType::QueryFolderRequest) return DecodeStatus::Malformed;
    Reader r(message.payload, message.payloadBytes);
    std::uint32_t units = 0;
    if (!r.U32(units) || units == 0 || units > kMaxPathUnits || !r.Units(units, request.path) ||
        !r.U32(request.pageStart) || !r.U32(request.pageSize) || !r.AtEnd()) {
        request = QueryFolderRequest{};
        return DecodeStatus::Malformed;
    }
    if (request.pageSize == 0 || request.pageSize > kMaxPageSize) {
        request = QueryFolderRequest{};
        return DecodeStatus::Malformed;
    }
    return DecodeStatus::Ok;
}

DecodeStatus DecodeStatusResponse(const Message& message, StatusResponse& response) {
    response = StatusResponse{};
    if (message.type != MessageType::StatusResponse) return DecodeStatus::Malformed;
    Reader r(message.payload, message.payloadBytes);
    std::uint32_t count = 0;
    constexpr size_t kEntryBytes = 2 + 1 + 8 + 8;
    if (!r.U32(count) || count > 26 || count * kEntryBytes != r.Remaining()) return DecodeStatus::Malformed;
    response.volumes.resize(count);
    for (VolumeStatus& v : response.volumes) {
        std::uint16_t drive = 0;
        std::uint8_t state = 0;
        r.U16(drive);
        r.U8(state);
        r.U64(v.memoryBytes);
        r.U64(v.secondsSinceCatchUp);
        if (drive < L'A' || drive > L'Z' || !ValidState(state)) {
            response = StatusResponse{};
            return DecodeStatus::Malformed;
        }
        v.drive = static_cast<wchar_t>(drive);
        v.state = static_cast<VolumeState>(state);
    }
    return DecodeStatus::Ok;
}

DecodeStatus DecodeFolderResponse(const Message& message, FolderResponse& response) {
    response = FolderResponse{};
    if (message.type != MessageType::FolderResponse) return DecodeStatus::Malformed;
    Reader r(message.payload, message.payloadBytes);
    std::uint8_t updating = 0;
    std::uint32_t count = 0;
    bool ok = r.U8(updating) && updating <= 1 && r.TotalsOf(response.totals) && r.U32(response.childCount) &&
              r.U32(response.pageStart) && r.U32(count) && count <= kMaxPageSize &&
              static_cast<std::uint64_t>(response.pageStart) + count <= response.childCount;
    response.updating = updating == 1;
    if (ok) response.children.reserve(count);
    for (std::uint32_t i = 0; ok && i < count; ++i) {
        ChildFolder c;
        std::uint16_t units = 0;
        std::uint8_t accessible = 0;
        ok = r.U16(units) && units != 0 && units <= kMaxNameUnits && r.Units(units, c.name) && r.TotalsOf(c.totals) &&
             r.U8(accessible) && accessible <= 1;
        c.accessible = accessible == 1;
        if (ok) response.children.push_back(std::move(c));
    }
    if (!ok || !r.AtEnd()) {
        response = FolderResponse{};
        return DecodeStatus::Malformed;
    }
    return DecodeStatus::Ok;
}

DecodeStatus DecodeError(const Message& message, ErrorResponse& response) {
    response = ErrorResponse{};
    if (message.type != MessageType::ErrorResponse) return DecodeStatus::Malformed;
    Reader r(message.payload, message.payloadBytes);
    std::uint32_t code = 0;
    if (!r.U32(code) || !r.AtEnd() || code < 1 || code > 8) return DecodeStatus::Malformed;
    response.code = static_cast<ErrorCode>(code);
    return DecodeStatus::Ok;
}

}  // namespace dirsize::proto
