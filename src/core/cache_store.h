#pragma once

// File-system side of the index cache (docs/DESIGN-phase3.md section 6).
//
// Location: %ProgramData%\DirSize\cache\<volume GUID>.cache, from
// SHGetKnownFolderPath(FOLDERID_ProgramData) (not the environment variable,
// which the user controls). The folder holds every folder name of the
// volume, so only SYSTEM and Administrators may access it. Users can create
// folders in ProgramData (its default ACL), so an existing DirSize folder is
// not trusted: before every use, both DirSize\ and cache\ must be real
// folders (not reparse points), owned by SYSTEM or Administrators, with a
// DACL that allows no one else. Otherwise the cache is refused (no repair)
// and the caller does a full scan.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dirsize {

struct StoreResult {
    bool ok = false;
    bool missing = false;  // read: the folder or the file does not exist
    const wchar_t* reason = L"";
    unsigned long error = 0;  // Win32 error behind the reason, if any
};

struct VolumeIdentity {
    std::uint8_t guid[16] = {};
    std::wstring guidText;    // e.g. 01234567-89ab-cdef-0123-456789abcdef
    std::uint64_t serial = 0;  // NTFS_VOLUME_DATA_BUFFER::VolumeSerialNumber
};

// GetVolumeNameForVolumeMountPointW and FSCTL_GET_NTFS_VOLUME_DATA (the
// latter needs an elevated process).
StoreResult GetVolumeIdentity(wchar_t drive, VolumeIdentity& identity);

// Finds (and with `create`, creates) the cache folder and checks that it is
// safe to use. On success `folder` is its path.
StoreResult PrepareCacheFolder(bool create, std::wstring& folder);

// Checks one existing folder: a real folder, owner SYSTEM or Administrators,
// a DACL whose allow entries name only those two. Exposed for tests.
StoreResult CheckFolderIsPrivate(const std::wstring& path);

std::wstring CacheFilePath(const std::wstring& folder, const VolumeIdentity& identity);

// Writes to "<path>.tmp" with CREATE_NEW, flushes it, then replaces `path`
// with MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH). A
// crash leaves either the old or the new file.
StoreResult WriteFileReplacing(const std::wstring& path, const std::vector<std::byte>& bytes);

// Reads a whole regular file of at most `maxBytes` (reparse points refused).
StoreResult ReadWholeFile(const std::wstring& path, std::uint64_t maxBytes, std::vector<std::byte>& bytes);

// Parses "\\?\Volume{GUID}\" (exposed for tests).
bool ParseVolumeGuidPath(const std::wstring& path, std::uint8_t guid[16], std::wstring& text);

}  // namespace dirsize
