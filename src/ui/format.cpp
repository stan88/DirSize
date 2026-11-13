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

std::wstring FormatSize(std::uint64_t bytes, SizeUnit unit) {
    switch (unit) {
        case SizeUnit::KB:
            // Not (bytes + 1023) / 1024: that overflows near the top.
            return FormatCount(bytes / 1024 + (bytes % 1024 != 0 ? 1 : 0)) + L" KB";
        case SizeUnit::GB: {
            wchar_t buffer[48];
            std::swprintf(buffer, 48, L"%.2f", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
            // Separators in the printed integer part, so a value rounded up
            // to the next thousand (999.999 -> "1000.00") gets one too.
            std::wstring text = buffer;
            const size_t dot = text.find(L'.');
            const std::wstring whole = text.substr(0, dot), rest = dot == std::wstring::npos ? L"" : text.substr(dot);
            std::wstring out;
            for (size_t i = 0; i < whole.size(); ++i) {
                if (i != 0 && (whole.size() - i) % 3 == 0) out += L',';
                out += whole[i];
            }
            return out + rest + L" GB";
        }
        case SizeUnit::Auto: break;
    }
    return FormatBytes(bytes);
}

std::wstring FormatPercent(std::uint64_t part, std::uint64_t whole) {
    double percent = whole == 0 ? 0.0 : static_cast<double>(part) * 100.0 / static_cast<double>(whole);
    wchar_t buffer[32];
    std::swprintf(buffer, 32, L"%.1f%%", percent);
    return buffer;
}

}  // namespace dirsize::ui

namespace dirsize::ui {

std::wstring FormatPattern(const std::wstring& pattern, std::initializer_list<std::wstring> args) {
    std::wstring out;
    out.reserve(pattern.size());
    for (size_t i = 0; i < pattern.size(); ++i) {
        wchar_t c = pattern[i];
        if (c == L'%' && i + 1 < pattern.size() && pattern[i + 1] >= L'1' && pattern[i + 1] <= L'9') {
            size_t n = static_cast<size_t>(pattern[i + 1] - L'1');
            if (n < args.size()) {
                out += *(args.begin() + n);
                ++i;
                continue;
            }
        }
        out += c;
    }
    return out;
}

}  // namespace dirsize::ui
