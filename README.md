# tilecopy

Windows-only terminal tool (C++23, MSVC) that copies a file, folder or whole
drive between **local drives** using a chunk-hash delta-copy algorithm: on the
first run it builds a database of SHA-256 chunk hashes (configurable chunk
size, default 1 MiB) of the source; on later runs only the chunks that changed
are rewritten on the destination.

It can also take **raw sector images** of a whole physical disk (`--drive` to
a `.vhdx`) or of a single partition (`--partition`), producing a dynamic VHDX
that mounts natively in Windows (double-click, Disk Management, or
`Mount-DiskImage`), with the same chunk database making later runs read only
the source and rewrite only what changed. Those images go back onto real
hardware with `--restore`.

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
tilecopy --drive  <X:|N|\\.\PhysicalDriveN>   <image.vhdx> [options]
tilecopy --partition <X:|\\?\Volume{GUID}\|\\.\HarddiskVolumeN> <image.vhdx> [options]
tilecopy --restore <image.vhdx> <N|\\.\PhysicalDriveN|X:> [options]
tilecopy --restore <image.vhdx> <X:|\\?\Volume{GUID}\|\\.\HarddiskVolumeN> [options]
tilecopy --file/--folder/--drive/--partition <source> --make-db [options]
```

Common options:

| Option | Meaning |
|---|---|
| `--db <path>` | Chunk-database file to use. Defaults to the destination side: `<dest-file>.tcdb`, `<dest-dir>\tilecopy.tcdb`, `Y:\tilecopy.tcdb`. With `--make-db` and no destination it is derived from the source instead; with `--restore` it defaults to `<image.vhdx>.tcdb` and is only read |
| `--make-db` | Only (re)generate the chunk database, copy nothing; destination may be omitted |
| `--chunk-size <size>` | Delta chunk size, `4K`–`64M` (`K`/`M` suffixes or plain bytes, default `1M`). A database built with a different chunk size is discarded and rebuilt |
| `--always-read-source` | Do not trust size + last-write time to decide a file is unchanged: read and hash every source file, then write only the chunks that really differ. Needed for sources whose write time is not updated when they are written — a virtual disk file modified while mounted is the usual case. Costs a full read of the source on every run. Not valid with `--ntfs-map-origin` or raw image copies |
| `--max-tries <n>` | Attempts per file, or per chunk for raw images, before giving up (default **3**, 250 ms between attempts) |
| `--no-file-logs` | Do not print a line per file copied/moved; only the initial and final messages (and errors) are printed |

Folder/drive options:

| Option | Meaning |
|---|---|
| `--mirror` | Delete destination entries that do not exist in the source |
| `--no-move-detection` | Do not detect moved/renamed source files (see below) |
| `--move-detection-check-date` | Only consider move candidates whose last-write time also matches the record. Cuts down how many new files must be hashed, but misses moves done by tools that rewrite write times (e.g. Explorer copies) |
| `--exclude-file <p>` | Exclude a file (repeatable; absolute or relative to the source root). Left untouched on the destination when mirroring |
| `--exclude-folder <p>` | Exclude a folder subtree (repeatable, same semantics) |
| `--mt` | Enable multithreaded copying (default: off) |
| `--threads <n>` | Max worker threads, 1–32 (default 8; only used with `--mt`) |
| `--folder-logs` | Instead of a line per file, print one line per folder checked with the number of files copied from it (implies `--no-file-logs`) |
| `--ntfs-map-origin` | Read the source volume's NTFS USN change journal to visit only entries changed since the last run instead of walking the whole tree. The journal position is stored in the database; needs administrator rights. Falls back to a full scan whenever the journal cannot be trusted (see below) |

## Raw image copies (`--drive` to a `.vhdx`, and `--partition`)

A `.vhdx` destination switches `--drive` to a raw sector copy, and
`--partition` always is one. Both need an **elevated console**.

- **Source forms** — `--drive C:` images the whole physical disk that
  contains volume `C:` (rejected if the volume spans disks); `--drive 0` and
  `--drive \\.\PhysicalDrive0` name the disk directly. `--partition` takes a
  drive letter, a volume GUID path (`\\?\Volume{...}\`, as listed by
  `mountvol` — works for volumes without a letter, e.g. locked/encrypted
  ones), or `\\.\HarddiskVolumeN`.
- **Destination** — a dynamic VHDX created and attached through the Virtual
  Disk API (1 MiB block size, sector size taken from the source disk). The
  disk is attached outside the PnP stack (non-PnP) where Windows supports
  it, so the image's file system can never be mounted mid-write and
  detaching is instant; older systems fall back to a PnP attach with no
  drive letters, kept offline — there, system services racing into the
  image's volume can make the final detach take a while. A
  whole-disk image carries the source's own partition table; a `--partition`
  image wraps the volume in a generated GPT with a single basic-data
  partition at a 1 MiB offset, so mounting it surfaces the volume with a
  drive letter.
- **Consistency** — every volume on the source with a mounted file system is
  put into one VSS snapshot set (writer-involved for application
  consistency, falling back to a writerless snapshot, then to live reads,
  with a log line whenever it degrades). Volume data is read from the shadow
  devices; regions without a snapshottable volume (partition gaps, boot
  areas, unformatted or locked partitions) are read live from the raw
  device. Removable drives cannot be snapshotted at all and are read live —
  incremental runs still work there; a file being written during the copy is
  simply caught by its changed hashes on the next run. An **unlocked BitLocker volume is imaged decrypted** (the copy
  mounts as plain NTFS); a locked one is copied as raw ciphertext in full.
- **Skipped data** — unallocated clusters (from the volume bitmap; works on
  NTFS, FAT32 and exFAT) are neither read nor written; on the first run they
  stay unallocated in the VHDX, keeping it small. The contents of
  `pagefile.sys`, `hiberfil.sys` and `swapfile.sys` are stored as zeros.
  File systems without a queryable bitmap (e.g. FAT12/16) are read in full,
  though later runs still write only changed chunks.
- **Incremental runs** — the database keeps one SHA-256 per chunk of the
  source address space plus the VHDX's size and write time as recorded after
  the previous run. When they still match, only the source is read; chunks
  whose hash matches the record are skipped, everything else is rewritten in
  place. **The destination is never read.** If the VHDX was modified by
  anything else in between — including being mounted read-write — every
  chunk is rewritten, so **mount images read-only**
  (`Mount-DiskImage -Access ReadOnly`, or attach read-only in Disk
  Management).
- **Database size** — one 32-byte hash per chunk: ~32 MiB per TiB of source
  at the default 1 MiB chunk size. Use a larger `--chunk-size` (up to 64M,
  multiple of 4K) for very large disks.
- `--mt`/`--threads` parallelize chunk reads/hashes/writes; `--max-tries`
  retries failed chunk reads/writes. `--make-db` hashes the source without
  touching any destination (needs `--db` when no destination is given). Not
  valid: `--mirror`, move detection options, `--exclude-*`, `--folder-logs`,
  `--ntfs-map-origin`.
- A failed chunk records a *failed* marker in place of its hash, so the next
  run redoes that chunk alone and the rest of the image stays a valid
  increment. Only a failed final flush — where which writes reached the file
  is unknowable — clears the recorded destination state and forces a full
  rewrite. Copying a system disk to a file on that same disk is allowed (the
  snapshot keeps the copy consistent) but noisy: the image's own blocks
  change every run.
- The destination's recorded write time is one tilecopy **sets** on the
  `.vhdx` after detaching it, not one it reads back. NTFS does not stamp a
  write time per write for the paging I/O the virtual disk driver performs,
  and the stamp that lands at cleanup can arrive after `DetachVirtualDisk`
  returns — reading it back risked recording a value the file no longer had,
  which turned every later run into a full copy. Setting it makes the value
  tilecopy's own; it then only changes when something else writes to the
  image, which is what the check is for.

## Restoring an image (`--restore`)

Writes a `.vhdx` made by `--drive` or `--partition` back onto real hardware,
**overwriting everything on the target**. Needs an **elevated console**.

```
tilecopy --restore E:\backups\disk0.vhdx 2          # onto \\.\PhysicalDrive2
tilecopy --restore E:\backups\data.vhdx  D:         # a volume image onto D:
```

| Option | Meaning |
|---|---|
| `--yes` | Do not ask for confirmation before overwriting the target. Required when stdin is not a console |
| `--restore-write-all` | Write every chunk without reading the target first. Skips the compare pass entirely |

- **The image is attached read-only** (read-only open *and* read-only attach),
  so a restore can never modify the backup it reads.
- **What the image is decides what the target must be.** A whole-disk image
  goes onto a physical disk (`2`, `\\.\PhysicalDrive2`, or a drive letter
  naming the disk that holds that volume); a `--partition` image goes onto a
  single volume (drive letter, `\\?\Volume{...}\`, `\\.\HarddiskVolumeN`).
  Naming the wrong kind is an error, not a guess. The two are told apart by
  the GPT wrapper `--partition` writes (one basic-data entry named `tilecopy`
  at a 1 MiB offset); a chunk database that describes the image overrides that
  probe, and one that does not is ignored.
- **Delta restore by default** — each chunk is read from the image, compared
  against the same chunk on the target, and written only if it differs. So
  re-restoring onto a drive that is already close to the image moves very
  little data. `--restore-write-all` writes everything instead.
- **Verification** — if a chunk database for the image is found
  (`<image.vhdx>.tcdb`, or `--db`, written with the same `--chunk-size`), every
  chunk read out of the image is hashed and checked against the hash recorded
  when the backup was taken. Mismatches are reported and still written (the
  image is all there is), and the run exits with code 2 so a damaged image
  cannot pass silently. The database is only ever read here, never written.
- **The target must be at least as large** as the device the image was made
  from. Extra space past the image is left untouched; for a GPT whole-disk
  image on a larger disk that means the backup GPT header lands where the
  original disk ended, so Windows will offer to repair the partition table
  afterwards. A sector-size difference between the image's device and the
  target is reported too — partition tables and file systems inside the image
  assume the original size.
- **Refusals** — a target holding the running Windows installation, or the
  image file itself, is rejected outright. If the chunk database happens to
  live on the target, that is reported as a warning (it is not needed to
  restore).
- **Confirmation** — the image, the target device (model, size, current
  partition table and volumes with labels) and every warning are printed
  first, then the disk number (or drive letter) has to be typed back. `--yes`
  skips the prompt.
- **Taking the target** — volumes on a target disk are locked and dismounted
  and the disk is taken offline for the run (non-persistently), then brought
  back online with a properties refresh so Windows re-reads the restored
  layout. A volume target must be lockable: writing a file system's own
  metadata under a live mount corrupts it, so an unlockable volume stops the
  restore before anything is written.
- `--mt`/`--threads` parallelize image reads, hashing and comparing;
  `--max-tries` retries a failed chunk read or write. A chunk that still fails
  leaves that region of the target holding whatever it had — the run reports
  it and exits with code 2; run the restore again.

## Behavior

- **Delta copy**: a file is rewritten chunk-by-chunk only where the chunk hash
  differs from the database. The delta path is used only when the destination
  file's size matches what the database recorded from the previous run;
  otherwise a full copy is performed (and the database refreshed). Files whose
  size + last-write time match the database and whose destination looks intact
  are skipped entirely — unread, so a source that changed without its write
  time changing is missed; `--always-read-source` drops that shortcut and
  hashes every source file instead.
- **Move detection** (folder/drive, on by default): a database record whose
  path truly vanished from the source paired with a new source file of the
  same size whose content matches (the new file is hashed and compared to the
  recorded chunk hashes) is renamed at the destination instead of re-copied,
  and the record is adopted under the new path. Write times are ignored by
  default because tools like Explorer rewrite them when copying;
  `--move-detection-check-date` requires them to match too. Disable with
  `--no-move-detection`.
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
- **USN incremental scans** (`--ntfs-map-origin`, folder/drive only): the
  database additionally stores the source volume's USN journal identity and
  position. Later runs read only the journal records since that position and
  visit just the entries reported created/changed/renamed/deleted; every other
  file — including its destination copy — is trusted as unchanged. This means
  destination-side damage to untouched files goes unseen until the source
  changes; run once without the flag (or delete the database) to force a full
  verification pass. Reading the journal needs administrator rights. Any
  condition that makes the journal untrustworthy — first run, non-NTFS source,
  journal wrapped or recreated, different volume, changed exclude list, a
  failed previous run, or a `--make-db`-only refresh followed by a copy —
  automatically falls back to a full scan. Changes made through hard-link
  aliases outside the source tree may be missed (hard-link topology is not
  preserved anyway), and `--folder-logs` prints no per-folder lines on
  incremental runs.
- Only local drives (fixed/removable) are supported; UNC paths and mapped
  network drives are rejected.
- Exit codes: `0` success, `1` bad arguments/setup (or a declined `--restore`
  confirmation), `2` completed with failures (or the database could not be
  saved; for `--restore`, also a chunk that did not match its recorded hash).

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
   The write time is only as good as the writer: NTFS does not stamp it for
   paging writes, so a virtual disk file written through a mounting driver can
   change content while keeping its write time (and size) exactly as recorded,
   and is then skipped. `--always-read-source` exists for those sources; it is
   opt-in because the alternative is re-reading the whole source every run.
   It is rejected together with `--ntfs-map-origin`, which decides what is
   visited at all from the same kind of metadata, and with raw image copies,
   which never take the shortcut in the first place.
5. **Retries** wait 250 ms between attempts; links get the same `--max-tries`
   as files. The default is 3 rather than 1 because one give-up is expensive:
   a raw image gives up on a whole chunk, and on a 4 TiB source there are four
   million of them, so a single transient read error would otherwise be
   near-certain on every run.
6. Alternate NTFS data streams and hard-link topology are **not** preserved
   (only the default stream is copied).
7. **`--ntfs-map-origin` databases** use a version-2 layout carrying the
   journal position; databases written without the flag keep the version-1
   layout unchanged. Any failed entry invalidates the stored position, so the
   next run walks everything instead of trusting a checkpoint that skipped a
   broken file.
8. **Raw image databases** use a version-3 layout (source identity, the
   VHDX's recorded size/write time, and one hash per chunk; an all-zero hash
   marks a chunk left as a hole). Image and file databases never mix: a
   database of the wrong flavor is discarded and rebuilt.
9. **`--restore` is a mode, not a direction flag**: it takes the image and the
   target positionally like every other mode, and refuses to guess what the
   image is. Restoring compares the target before writing because the delta
   idea holds in that direction too — a spare drive kept in sync with an image
   is mostly identical to it already — and because reading a target that is
   about to be overwritten costs nothing but time. The database is optional
   there: it is what makes a corrupted image detectable during the restore,
   but nothing about restoring depends on having it.

## Donations are welcome

If you liked this software and would like to support its development, you can buy me a coffee. Understand that any value is fine and appreciated, and that your support means a lot to me. Thank you!

[Paypal donation](https://www.paypal.com/donate/?business=NUHKNZCBCPCLQ&no_recurring=0&currency_code=USD)

![Paypal qrcode](donations/paypal_qrcode.png)

## Check out my other projects

- [qt6appskeleton](https://github.com/riozebratubo/qt6appskeleton): a cross-platform Qt6 app skeleton with sqlite persistence and settings
- [Winzoo](https://github.com/riozebratubo/winzoo): a lightweight taskbar replacement for Windows 10/11
