#include "ui/update_check.h"

namespace dirsize::ui {
namespace {

bool ParseNumber(std::wstring_view& text, std::uint32_t& out) {
    size_t n = 0;
    std::uint32_t value = 0;
    while (n < text.size() && text[n] >= L'0' && text[n] <= L'9') {
        if (n == 5) return false;
        value = value * 10 + static_cast<std::uint32_t>(text[n] - L'0');
        ++n;
    }
    if (n == 0) return false;
    out = value;
    text.remove_prefix(n);
    return true;
}

// A small scanner over JSON text (RFC 8259), enough to walk the members of
// one object. Positions only move forward; every step checks the end.
class Scanner {
public:
    explicit Scanner(std::string_view text) : s_(text) {}

    void SkipSpace() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool Peek(char c) {
        SkipSpace();
        return i_ < s_.size() && s_[i_] == c;
    }
    bool Take(char c) {
        if (!Peek(c)) return false;
        ++i_;
        return true;
    }

    // A string at the current position. `out` gets the text with escapes
    // decoded; a \u escape outside ASCII becomes '?' (only ASCII values are
    // used by the caller).
    bool String(std::string& out) {
        out.clear();
        if (!Take('"')) return false;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i_ >= s_.size()) return false;
            const char e = s_[i_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (s_.size() - i_ < 4) return false;
                    unsigned code = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = s_[i_++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else return false;
                    }
                    out.push_back(code < 0x80 ? static_cast<char>(code) : '?');
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    // Skips one value of any kind. Nested objects and arrays are skipped by
    // counting brackets, with strings skipped whole so brackets and quotes
    // inside them do not count.
    bool SkipValue() {
        SkipSpace();
        if (i_ >= s_.size()) return false;
        std::string ignored;
        if (s_[i_] == '"') return String(ignored);
        if (s_[i_] == '{' || s_[i_] == '[') {
            int depth = 0;
            while (i_ < s_.size()) {
                const char c = s_[i_];
                if (c == '"') {
                    if (!String(ignored)) return false;
                    continue;
                }
                ++i_;
                if (c == '{' || c == '[') {
                    ++depth;
                } else if (c == '}' || c == ']') {
                    if (--depth == 0) return true;
                }
            }
            return false;
        }
        // A number, true, false or null: up to the next delimiter.
        const size_t start = i_;
        while (i_ < s_.size() && s_[i_] != ',' && s_[i_] != '}' && s_[i_] != ']' && s_[i_] != ' ' &&
               s_[i_] != '\t' && s_[i_] != '\n' && s_[i_] != '\r') {
            ++i_;
        }
        return i_ > start;
    }

private:
    std::string_view s_;
    size_t i_ = 0;
};

}  // namespace

bool ParseVersion(std::wstring_view text, Version& out) {
    if (!text.empty() && (text[0] == L'v' || text[0] == L'V')) text.remove_prefix(1);
    Version v;
    if (!ParseNumber(text, v.major) || text.empty() || text[0] != L'.') return false;
    text.remove_prefix(1);
    if (!ParseNumber(text, v.minor) || text.empty() || text[0] != L'.') return false;
    text.remove_prefix(1);
    if (!ParseNumber(text, v.patch) || !text.empty()) return false;
    out = v;
    return true;
}

int CompareVersions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

bool TopLevelJsonString(std::string_view json, std::string_view key, std::string& value) {
    Scanner sc(json);
    if (!sc.Take('{')) return false;
    if (sc.Take('}')) return false;
    std::string name;
    for (;;) {
        if (!sc.String(name) || !sc.Take(':')) return false;
        if (name == key) {
            if (!sc.Peek('"')) return false;
            if (!sc.String(value)) return false;
            for (char c : value) {
                if (static_cast<unsigned char>(c) >= 0x80) return false;
            }
            return true;
        }
        if (!sc.SkipValue()) return false;
        if (sc.Take(',')) continue;
        return false;  // '}' (not found) or malformed
    }
}

UpdateState ClassifyUpdate(unsigned long httpStatus, std::string_view body, std::wstring_view current,
                           std::wstring& latest) {
    latest.clear();
    if (httpStatus == 403 || httpStatus == 429) return UpdateState::RateLimited;
    if (httpStatus != 200) return UpdateState::BadAnswer;
    std::string tag;
    if (!TopLevelJsonString(body, "tag_name", tag)) return UpdateState::BadAnswer;
    const std::wstring wideTag(tag.begin(), tag.end());  // ASCII only (checked above)
    Version theirs, ours;
    if (!ParseVersion(wideTag, theirs) || !ParseVersion(current, ours)) return UpdateState::BadAnswer;
    latest = std::to_wstring(theirs.major) + L"." + std::to_wstring(theirs.minor) + L"." +
             std::to_wstring(theirs.patch);
    return CompareVersions(theirs, ours) > 0 ? UpdateState::Newer : UpdateState::UpToDate;
}

}  // namespace dirsize::ui
