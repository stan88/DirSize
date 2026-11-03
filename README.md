# DirSize

**English** | [繁體中文](README.zh-TW.md)

A fast, low-resource tool for Windows that shows the size of every folder.

> Status: early development — nothing usable yet.

## Goals

- **Fast**: on NTFS volumes, build the index from the Master File Table and keep it
  current with the USN change journal, instead of walking every directory.
- **Light**: compact in-memory folder tree, background (low-priority) I/O, a persisted
  cache so a reboot does not trigger a full rescan.
- **Works everywhere**: other file systems (exFAT, FAT32, removable, network drives)
  fall back to directory enumeration plus change notifications. Which drive types are
  scanned is configurable.

## Planned components

| Component | Role |
|---|---|
| Indexer core | Volume scanning, folder tree, change tracking, cache persistence |
| Service | Runs the indexer in the background, answers queries over a named pipe |
| CLI | Command-line queries and benchmarks |
| Front-end | Shows folder sizes next to File Explorer |

## Building

Requires Visual Studio 2022 with the **Desktop development with C++** workload
(MSVC, Windows SDK, CMake). Build instructions will follow once the first component lands.

## Provenance

DirSize is an independent implementation written from public Microsoft documentation.
It is inspired by the idea of the discontinued *Folder Size for Windows* (SourceForge),
but contains none of its code. See [docs/DEVLOG.md](docs/DEVLOG.md).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the clean-room and privacy rules.

## License

[MIT](LICENSE)
