# tilecopy

Windows-only terminal tool (C++23, MSVC) that copies a file, folder or whole
drive between **local drives** using a chunk-hash delta-copy algorithm: on the
first run it builds a database of SHA-256 chunk hashes (configurable chunk
size, default 1 MiB) of the source; on later runs only the chunks that changed
are rewritten on the destination.

## Build

Requires Visual Studio 2022 (MSVC, C++23) and CMake 3.25+.

```
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

## Usage

```
tilecopy --file   <source-file> <dest-file>   [options]
tilecopy --folder <source-dir>  <dest-dir>    [options]
tilecopy --drive  <X:>          <Y:|dest-dir> [options]
tilecopy --file/--folder/--drive <source> --make-db [options]
```

Common options:

| Option | Meaning |
|---|---|
| `--db <path>` | Chunk-database file to use. Defaults to the destination side: `<dest-file>.tcdb`, `<dest-dir>\tilecopy.tcdb`, `Y:\tilecopy.tcdb`. With `--make-db` and no destination it is derived from the source instead |
| `--make-db` | Only (re)generate the chunk database, copy nothing; destination may be omitted |
| `--chunk-size <size>` | Delta chunk size, `4K`–`64M` (`K`/`M` suffixes or plain bytes, default `1M`). A database built with a different chunk size is discarded and rebuilt |
| `--max-tries <n>` | Attempts per file before giving up (default **1**) |

Folder/drive options:

| Option | Meaning |
|---|---|
| `--mirror` | Delete destination entries that do not exist in the source |
| `--exclude-file <p>` | Exclude a file (repeatable; absolute or relative to the source root). Left untouched on the destination when mirroring |
| `--exclude-folder <p>` | Exclude a folder subtree (repeatable, same semantics) |
| `--mt` | Enable multithreaded copying (default: off) |
| `--threads <n>` | Max worker threads, 1–32 (default 8; only used with `--mt`) |

## Behavior

- **Delta copy**: a file is rewritten chunk-by-chunk only where the chunk hash
  differs from the database. The delta path is used only when the destination
  file's size matches what the database recorded from the previous run;
  otherwise a full copy is performed (and the database refreshed). Files whose
  size + last-write time match the database and whose destination looks intact
  are skipped entirely.
- **Drive copies** skip NTFS metadata files (`$MFT`, `$LogFile`, `$Extend`, …),
  `$Recycle.Bin`, `System Volume Information`, and the pagefile family at the
  drive root. The destination may be a drive or a folder; the source drive's
  contents are copied into it.
- A folder/drive **destination inside the source tree** (e.g.
  `--drive C: C:\backup`) is rejected — it would be copied into itself — unless
  it is covered by `--exclude-folder`.
- **Metadata** (attributes, creation/access/write times, owner/group/DACL, and
  SACL when running elevated) is always copied, best-effort, for files and
  folders. Directory timestamps are applied children-first so they survive.
- **Symbolic links, junctions and other reparse points** are copied as links,
  never followed — including during mirror deletion. Creating symlinks may
  require elevation or Windows Developer Mode.
- **Empty folders** are always created.
- Only local drives (fixed/removable) are supported; UNC paths and mapped
  network drives are rejected.
- Exit codes: `0` success, `1` bad arguments/setup, `2` completed with failures.

## Design decisions

1. **The DB file is automatically excluded** from copying, from landing on by
   a copied file, and from mirror deletion. Use `--db` to place it elsewhere
   (a drive root may need elevation to write to).
2. **`--mt`/`--threads` are folder/drive-only** (a single-file copy has nothing
   to parallelize at file granularity). Threading parallelizes across files,
   not within one file.
3. **Chunk size** is set with `--chunk-size` (default 1 MiB); using a different
   value than the existing database silently discards and rebuilds it.
4. **Trust model**: unchanged files are detected via size + last-write time,
   like robocopy/rsync defaults. If the destination was modified behind
   tilecopy's back at equal size, the delta pass corrects any chunk whose hash
   changed **on the source** but cannot see destination-only tampering. A
   `--verify` mode reading the destination could be added later.
5. **Retries** wait 250 ms between attempts; links get the same `--max-tries`
   as files.
6. Alternate NTFS data streams and hard-link topology are **not** preserved
   (only the default stream is copied).
