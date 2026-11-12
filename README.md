# DirSize

**English** | [繁體中文](README.zh-TW.md)

A fast, low-resource tool for Windows that shows the size of every folder.

> Status: 0.1.0, the first release. Download it from
> [Releases](https://github.com/stan88/DirSize/releases).

## What it does

- **Window**: a folder tree on the left; on the right, the selected folder's
  subfolders and files with their size, size on disk, share of the total and
  number of files. Sort by any column, resize the columns.
- **File Explorer panel**: select a folder in File Explorer and a narrow panel
  beside the window lists the sizes of its subfolders.
- **Context menu**: "Open with DirSize" on folders and drives.
- Follows the Windows theme (light, dark, high contrast) and display language
  (English, Traditional Chinese; others can be added, see
  [lang/README.md](lang/README.md)).

## How it works

- **Service** (`dirsize-service.exe`, runs as LocalSystem): indexes the fixed
  NTFS volumes from the Master File Table and keeps the index current with the
  USN change journal, so sizes are ready without walking every folder. The
  index is saved to `C:\ProgramData\DirSize` so a restart does not need a full
  rescan. It runs at low priority with a CPU cap.
- **Front end** (`dirsize.exe`, the window and the panel): asks the service.
  Where the service has no index (other file systems, network drives, or no
  service installed) it scans the folders itself, which is slower.
- **CLI** (`dirsize-cli.exe`): queries and measurements; run it without
  arguments for its commands.

## Requirements

Windows 10 version 1809 or later, x64.

## Installing

Download `DirSize-<version>-setup.exe` from
[Releases](https://github.com/stan88/DirSize/releases) and run it
(administrator rights are needed for the service). The installer offers, all checked by default:

- "Open with DirSize" in the File Explorer context menu (all users)
- Start the File Explorer panel when you sign in
- Show the Share column

These can be changed later in the program's "⋯" menu (the panel's right-click
menu has the Share column too).

The installer is **not code-signed** yet, so Windows shows "Unknown publisher"
and SmartScreen may warn when it is downloaded.

Uninstalling removes the service, the program files, the context menu and the
index cache, and the sign-in entry of the account that uninstalls. Other
per-user settings (`HKEY_CURRENT_USER\Software\DirSize`) are left in place.

A portable zip with only `dirsize.exe` is also built; without the service it
always scans folders itself.

## Building

Requires Visual Studio 2022 with the **Desktop development with C++** workload
(MSVC, Windows SDK, CMake). From Git Bash:

```bash
CMAKE="/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CMAKE" -S . -B build -G "Visual Studio 17 2022" -A x64
"$CMAKE" --build build --config Release
```

Tests: `ctest --test-dir build -C Release` (the `ctest.exe` next to `cmake.exe`).

Installer: install [Inno Setup](https://jrsoftware.org/isinfo.php) 6.5 or
later, then run `scripts\package-installer.ps1` in PowerShell. It builds
Release and writes the installer and the portable zip to `dist\`.

GitHub Actions (`.github/workflows/build.yml`) does the same on every push
and pull request. To release: set `project(VERSION)` in `CMakeLists.txt`
(the only place the version is set), commit, and push the tag `v<version>`.
The build stops if the tag and `CMakeLists.txt` disagree; otherwise it
creates a draft release with the two files, published by hand.

## Provenance

DirSize is an independent implementation written from public Microsoft documentation.
It is inspired by the idea of the discontinued *Folder Size for Windows* (SourceForge),
but contains none of its code. See [docs/DEVLOG.md](docs/DEVLOG.md).

The installer's Traditional Chinese messages are the official Inno Setup
translation, included unchanged; see [installer/lang/README.md](installer/lang/README.md).

## Code signing policy

Releases are not code-signed for now. The SignPath Foundation's free program
for open-source projects requires a project that is already widely known, so
DirSize will apply again once it has more users. If accepted, releases will be
signed with: Free code signing provided by [SignPath.io](https://signpath.io),
certificate by [SignPath Foundation](https://signpath.org), under these rules:

- Only files built by GitHub Actions from this repository's source are
  signed, and each release is approved by hand before signing.
- Committers and reviewers: [stan88](https://github.com/stan88)
- Approvers: [stan88](https://github.com/stan88)

## Privacy

This program will not transfer any information to other networked systems
unless specifically requested by the user or the person installing or
operating it. DirSize contains no networking code; the service answers only
programs on the same computer (its named pipe rejects remote clients).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the clean-room and privacy rules.
To translate the interface, see [lang/README.md](lang/README.md).

## License

[MIT](LICENSE)
