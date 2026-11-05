#pragma once

// CRC-32 as described in the W3C PNG specification, section 5.5 (ISO 3309 /
// ITU-T V.42): polynomial x^32 + x^26 + x^23 + x^22 + x^16 + x^12 + x^11 +
// x^10 + x^8 + x^7 + x^5 + x^4 + x^2 + x + 1, register initialised to all
// ones, bits of each byte processed least significant first, result inverted.
// Implemented from that description; used only to detect damaged cache
// files, not for security.

#include <cstddef>
#include <cstdint>

namespace dirsize {

class Crc32 {
public:
    void Update(const void* data, size_t size);
    std::uint32_t Value() const { return ~state_; }

private:
    std::uint32_t state_ = 0xFFFFFFFFu;
};

std::uint32_t ComputeCrc32(const void* data, size_t size);

}  // namespace dirsize
