#pragma once
// "Check for updates" in the "..." menu: what the answer of GitHub's "Get the
// latest release" (GET /repos/{owner}/{repo}/releases/latest) means. Only the
// release's tag_name is read. No network code here (that is in
// src/app/update_client.cpp), so the rules are unit-tested.

#include <cstdint>
#include <string>
#include <string_view>

namespace dirsize::ui {

struct Version {
    std::uint32_t major = 0, minor = 0, patch = 0;
};

// "0.2.1" or "v0.2.1": exactly three numbers of at most 5 digits each.
// Anything else (a prerelease suffix, four parts, ...) is not accepted.
bool ParseVersion(std::wstring_view text, Version& out);

// < 0, 0 or > 0.
int CompareVersions(const Version& a, const Version& b);

// The value of a string member of the top-level JSON object, such as
// "tag_name". Members nested in other objects or arrays, and text inside
// strings (release notes that mention "tag_name"), are skipped. Only ASCII
// values are returned; false when the member is missing, is not a string,
// or the text is not a JSON object.
bool TopLevelJsonString(std::string_view json, std::string_view key, std::string& value);

enum class UpdateState {
    Newer,         // latest is newer than this program
    UpToDate,      // the same, or this program is newer (a build after the release)
    RateLimited,   // HTTP 403 or 429: GitHub's hourly limit for unauthenticated requests
    BadAnswer,     // another HTTP status, no release yet (404), or no usable tag_name
    NetworkError,  // the request did not complete (set by the caller, not by ClassifyUpdate)
};

// The state for an HTTP status and body. `latest` gets the release's
// version as "0.2.1" for Newer and UpToDate.
UpdateState ClassifyUpdate(unsigned long httpStatus, std::string_view body, std::wstring_view current,
                           std::wstring& latest);

}  // namespace dirsize::ui
