#include "core/crc32.h"

#include <array>

namespace dirsize {
namespace {

// Bits are processed least significant first, so the register holds the
// polynomial with its coefficients reversed: bit 31 - k is the x^k term.
constexpr std::uint32_t ReversedPolynomial() {
    constexpr int kTerms[] = {26, 23, 22, 16, 12, 11, 10, 8, 7, 5, 4, 2, 1, 0};  // x^32 is implicit
    std::uint32_t p = 0;
    for (int k : kTerms) p |= 1u << (31 - k);
    return p;
}

constexpr std::array<std::uint32_t, 256> MakeTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int bit = 0; bit < 8; ++bit) c = (c & 1) ? ReversedPolynomial() ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kTable = MakeTable();

}  // namespace

void Crc32::Update(const void* data, size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint32_t c = state_;
    for (size_t i = 0; i < size; ++i) c = kTable[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    state_ = c;
}

std::uint32_t ComputeCrc32(const void* data, size_t size) {
    Crc32 crc;
    crc.Update(data, size);
    return crc.Value();
}

}  // namespace dirsize
