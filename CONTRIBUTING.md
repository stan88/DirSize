# Contributing to DirSize

**English** | [繁體中文](CONTRIBUTING.zh-TW.md)

Thanks for your interest. DirSize is an MIT-licensed, independent implementation. These rules
keep it that way.

## Clean-room rules

- **Do not read or copy** the source code of the discontinued *Folder Size for Windows*
  (SourceForge, GPLv2). This includes its SourceForge code browser, its CVS repository, and any
  mirror or copy.
- Do not use NTFS / MFT parsing code that is GPL-licensed or of unclear origin (for example,
  derivatives of ntfs-3g, or unattributed headers found online).
- Base the implementation on public documentation only: Microsoft Learn, WDK documentation, the
  Microsoft-published Windows SDK / WDK headers, and the C++ standard.
- Record every document you rely on in the "參考資料" (references) table of
  [docs/DEVLOG.md](docs/DEVLOG.md) as you work, not afterwards.
- Before adding third-party code, confirm its license is compatible with MIT and note it in the
  DEVLOG.

## Privacy

Do not put personal information in the repository: local folder names or paths, user names,
hardware models, installed software, or e-mail addresses. When you record measurements, use
placeholders or numbers for personal folders. Raw benchmark output belongs in `bench-results/`,
which git ignores.

## Building

Requires Visual Studio 2022 with the **Desktop development with C++** workload.

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Code

- C++20, Win32, MSVC. Target Windows 10 or later (`_WIN32_WINNT=0x0A00`). Static CRT (`/MT`).
- Parsing of raw buffers returned by Windows must be bounds-checked.
