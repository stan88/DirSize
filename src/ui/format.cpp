#include "ui/format.h"

#include <cwchar>

namespace dirsize::ui {

std::wstring FormatBytes(std::uint64_t bytes) {
    static const wchar_t* const kUnits[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB", L"EB"};
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 6) {
        value /= 1024.0;
        ++unit;
    }
    // Rounding can reach 1024 (e.g. 1023.99 KB): show it in the next unit.
    if (value >= 999.5 && unit < 6) {
        value /= 1024.0;
        ++unit;
    }
    wchar_t buffer[32];
    const wchar_t* format = value < 9.995 ? L"%.2f %s" : value < 99.95 ? L"%.1f %s" : L"%.0f %s";
    std::swprintf(buffer, 32, format, value, kUnits[unit]);
    return buffer;
}

std::wstring FormatCount(std::uint64_t count) {
    std::wstring digits = std::to_wstring(count);
    std::wstring out;
    out.reserve(digits.size() + digits.size() / 3);
    for (size_t i = 0; i < digits.size(); ++i) {
        // A comma before every group of three counted from the right.
        const size_t remaining = digits.size() - i;
        if (i != 0 && remaining % 3 == 0) out += L',';
        out += digits[i];
    }
    return out;
}

std::wstring FormatPercent(std::uint64_t part, std::uint64_t whole) {
    double percent = whole == 0 ? 0.0 : static_cast<double>(part) * 100.0 / static_cast<double>(whole);
    wchar_t buffer[32];
    std::swprintf(buffer, 32, L"%.1f%%", percent);
    return buffer;
}

}  // namespace dirsize::ui
