<div align="center">

<img src="assets/ext4-hero.svg" width="860" alt="Ext4 for Windows — native ext2, ext3 and ext4 file-system driver">

[![Ext4 for Windows — driver demonstration](images/ext4.gif)](https://youtu.be/TO6ssLvekVA)

[![Latest Release](https://img.shields.io/github/v/release/wesmar/Ext4?label=Latest%20Release&style=for-the-badge)](https://github.com/wesmar/Ext4/releases/latest)

**[⬇ Download ext4.7z](https://github.com/wesmar/Ext4/releases/download/latest/ext4.7z)**
&nbsp;·&nbsp;
**[Source Code](https://github.com/wesmar/Ext4/archive/refs/heads/main.zip)**
&nbsp;·&nbsp;
**[All Releases](https://github.com/wesmar/Ext4/releases)**

> No archive password. Extract `ext4.sys` and `ext4ctl.exe`.

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](LICENSE.md)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011%20x64-2563eb.svg)]()
[![Language](https://img.shields.io/badge/Language-C23-22c55e.svg)]()
[![Build](https://img.shields.io/badge/Build-MSVC%20%2B%20WDK-lightgrey.svg)]()

</div>
> **Engineering status**
>
> `ext4.sys` provides native ext2/ext3/ext4 read/write access on bare-metal Windows systems and in virtual machines. The current build passed the documented Windows/Linux validation, including Driver Verifier and repeated unload testing. Engineering refinements continue. Microsoft production signing is pending; loading uses KVC or DrvLoader on systems you administer. Keep a backup of important data.

> **Repository policy:** the release archive is not password-protected. Git tracks the active source, project files, tests and documentation. Build outputs, symbols, local tooling, virtual disks and test logs stay outside the repository. Test passphrase defaults in two PowerShell scripts are represented by `<TEST_LUKS_PASSPHRASE>`.

# Ext4 for Windows — Native ext2/ext3/ext4 Read/Write Driver

<br>

<div align="center">

**A drive letter for your Linux partition • journaled writes • controlled unload**

*jbd2 journal, extents with unwritten preallocation, htree directories, metadata checksums, 64-bit group descriptors*

*Windows writes checked from Linux: metadata consistency and byte-exact contents*

</div>

<!-- SCREENSHOTS / VIDEO — drop files into images/ and uncomment:
![An ext4 partition in Explorer](images/explorer.png)
![Test run summary](images/tests.png)
-->

> **`ext4.sys`** is a native Windows kernel-mode file system driver for ext2, ext3 and ext4 volumes. It registers with the I/O manager, cache manager and mount manager. An ext4 partition receives a drive letter, appears in Explorer and Disk Management, and supports normal Windows file operations: create, write, rename, map and delete. Metadata updates pass through a **jbd2 journal** that Linux can replay. Writes use **unwritten extents** with Linux-compatible on-disk semantics. The validation pipeline checks Windows-written files from Linux using **`e2fsck` and SHA-256**.
>
> The codebase retains parts of **Ext2Fsd** by Matt Wu, **Ext4Fsd** by Bo Branten and Linux ext4/jbd2. Their code, algorithms and licensing scope are identified in [Code Lineage and Redesign](#code-lineage-and-redesign). The active Windows paths were then reorganized, rewritten and tested from Windows through to Linux media verification.

---

## Table of Contents

- [Overview](#overview)
- [Code Lineage and Redesign](#code-lineage-and-redesign)
- [Architecture](#architecture)
- [Write Path and the Journal](#write-path-and-the-journal)
- [Unwritten Extents](#unwritten-extents)
- [Performance Engineering](#performance-engineering)
- [Stopping on Demand — `sc stop`](#stopping-on-demand--sc-stop)
- [Drive Letters for Linux Partitions](#drive-letters-for-linux-partitions)
- [Encrypted Volumes — LUKS](#encrypted-volumes--luks)
- [Security Boundaries](#security-boundaries)
- [Source Layout](#source-layout)
- [Testing and Validation](#testing-and-validation)
- [Supported Features](#supported-features)
- [Known Limitations](#known-limitations)
- [Installation](#installation)
- [Building from Source](#building-from-source)
- [License](#license)

---

## Overview

Windows has no native ext4 file-system driver. On dual-boot and larger multiboot systems, ext4 volumes used by Ubuntu, Qubes OS and other Linux installations appear without a drive letter or `\\?\Volume{...}` link. The same happens when an ext4 disk is attached over USB. Disk Management reports these volumes as unknown.

The ways of reaching an ext4 partition from Windows, and what each one costs:

| Way in | What it costs | Drive letter, every program works? |
|---|---|---|
| `wsl --mount` | a Linux VM, administrator rights, the whole disk handed to WSL | No — files are reached through `\\wsl.localhost` |
| Copy tools (read-only viewers) | one application | No — copy out only, no writes |
| Commercial drivers | a paid licence, closed source | Yes |
| Ext2Fsd / Ext4Fsd | one driver, open source | Yes — the base this driver grew from |
| **`ext4.sys`** | **one kernel driver, open source** | **Yes, journaled, `e2fsck`-clean, stops on demand** |

Design rules the code follows:

- **Independent media verification.** The full matrix detaches the test volume from Windows, checks it with `e2fsck` and uses the Linux kernel to verify the files listed in the test manifests with SHA-256.
- **Event-driven background work.** Reapers use deadlines and notifications; volume teardown is woken when references are released. Protocol timing, such as multi-mount protection, follows the on-disk protocol.
- **Never write what Linux would not.** A journal that cannot be opened mounts the volume read-only instead of writing unjournaled; a feature the driver does not implement refuses the mount or forces read-only.
- **Warning-free is the minimum.** `/W4 /WX /std:clatest` in both configurations: a warning is a build error.

---

## Code Lineage and Redesign

Parts of the codebase and several core algorithms come from four open-source projects. They provide the ext file-system model, Linux on-disk semantics, Windows file-system scaffolding and the original extended-attribute implementation. Because these components remain in the combined work, the complete source is distributed under the GNU General Public License version 2 (`GPL-2.0-only`).

| Project | Authors | Retained foundation |
|---|---|---|
| **Ext2Fsd** | Matt Wu, with KaHo Ng | IRP dispatch scaffolding, FCB/MCB model, cache manager integration and ext2/ext3 handling |
| **Ext4Fsd** | Bo Branten | ext4 structures, metadata checksums, jbd2 support with 64-bit block numbers and Visual Studio project lineage |
| **Linux kernel** — `fs/ext4`, `fs/jbd2`, `lib/rbtree.c`, `fs/nls` | Linus Torvalds, Stephen Tweedie, Red Hat, Cluster File Systems and the ext4 / jbd2 developers | Extent-tree, htree, checksum and journal algorithms; character set tables |
| **lwext4** | Grzegorz Kostka, KaHo Ng | The original extended-attribute implementation; its BSD notice remains in `ext4_xattr.h` |

My standard for the redesign was mathematical precision: explicit invariants, bounded state transitions, clear ownership, measured hot paths and reproducible failures. The resulting code replaces or substantially rewrites the active paths for writes, allocation, journaling, mount and unload, security checks and hot-path lookup.

What changed on the way to `ext4.sys`, in short:

- **Structure.** The monolithic source tree is divided by responsibility into `driver`, `fsd`, `volume`, `core`, `ext4`, `journal`, `linux`, `nls` and `support`. Each area has its own header; the former umbrella header had 3 400 lines. Before behavior changed, preprocessing proved the split token-identical across 1 615 functions.
- **Journal commits and recovery.** A Windows-native jbd2 commit engine logs every metadata change and commits it before the home write. Linux `e2fsck` or the driver replays an interrupted transaction at the next mount.
- **Stopping and unloading.** `sc stop` drains mounted volumes and outstanding references before releasing the driver.
- **Drive letters** for partitions the mount manager treats as hidden (MBR type `0x83`, GPT "Linux filesystem").
- **Dozens of fixes** found by the test suite: data exposure past a write into preallocated space, extents created one block at a time, a block allocator that started every file in group 0, registry buffer overruns, uninitialised return values, unload hangs, reapers polling on timers, extended attribute blocks written without their checksum, a Windows EA write that erased the Linux ACLs of the file, xattr blocks leaked on delete, lazily initialised block bitmaps that never reached the disk, and reference leaks - a refused open, a close cut short on a demoted symlink, a delete taken over by the wrong handle - that kept `sc stop` pending for ever.

---

## Architecture

```mermaid
flowchart TB
    subgraph APPS["Windows"]
        EXPL["Explorer, cmd, PowerShell,<br/>any Win32 program"]
        MM["Mount manager<br/>drive letters, Volume{GUID}"]
        CC["Cache manager / memory manager"]
    end

    subgraph DRV["ext4.sys"]
        DRIVER["driver/<br/>entry, dispatch, registry, unload drain thread"]
        FSD["fsd/<br/>create, read, write, fileinfo, dirctl, fsctl, pnp"]
        VOL["volume/<br/>mount, VCB, dismount, drive letters"]
        CORE["core/<br/>FCB, MCB, ICB caches, extent lists, reapers"]
        EXT4["ext4/<br/>extents, htree, allocators, inodes, xattr, csum"]
        JNL["journal/<br/>jbd2 commit, checkpoint, replay"]
        LNX["linux/<br/>buffer heads over the cache manager"]
    end

    subgraph DISK["Storage"]
        VOLDEV["Volume device (volmgr)"]
        MEDIA["ext2/3/4 partition on SATA, NVMe, USB, VHD/VHDX"]
    end

    EXPL --> FSD
    MM <--> VOL
    FSD --> CORE
    FSD --> EXT4
    FSD <--> CC
    VOL --> JNL
    EXT4 --> LNX
    JNL --> LNX
    LNX --> CC
    CC --> VOLDEV
    DRIVER --> FSD
    VOLDEV --> MEDIA
```

File data goes through the cache manager like any Windows file system: cached reads and writes are served from the system cache, paging I/O comes back to the driver, which maps file blocks to disk blocks through the extent tree (or the indirect block map on ext2/ext3). Metadata goes through Linux-style buffer heads, which are pinned views of the volume stream in the same cache, so every inode, bitmap, directory block and extent node has exactly one copy in memory.

---

## Write Path and the Journal

Every metadata change passes through one hook, `mark_buffer_dirty`, which attaches the block to the running jbd2 transaction of the calling thread. The volume stream itself will only write byte ranges the driver has registered as dirty, so the cache manager cannot flush a metadata page behind the journal's back: write-ahead ordering is enforced, not hoped for.

```mermaid
sequenceDiagram
    autonumber
    participant App as Windows program
    participant Fsd as fsd/ (IRP)
    participant Ext as ext4/ (extents, bitmaps, inode)
    participant Jnl as journal/ (jbd2)
    participant Disk as volume

    App->>Fsd: WriteFile / SetEndOfFile / CreateFile
    Fsd->>Ext: allocate blocks, update extent tree and inode
    Ext->>Jnl: mark_buffer_dirty(bh) — block joins the running transaction
    Fsd-->>App: STATUS_SUCCESS
    Note over Jnl: commit: descriptor + block copies + revoke, FLUSH, commit block, FLUSH
    Jnl->>Disk: log blocks
    Note over Jnl: checkpoint: committed copies written home, log tail advanced
    Jnl->>Disk: metadata in place
```

Rules the engine keeps, each one learned from a failure it caused:

1. **One hook for all metadata.** Directory blocks that were marked dirty directly used to bypass the journal; after a crash the inodes came back and the directory entries did not. Every site now goes through `mark_buffer_dirty`.
2. **A handle joins a transaction only if it still fits the log**, the rule jbd2's `start_this_handle` enforces; otherwise it asks for a commit and waits. Without it a fast writer filled the log and the commit aborted the journal.
3. **Superseded records retire without I/O.** A block logged again by a newer committed transaction is not written home from the older one, so a hot working set cannot pin the log.
4. **The journal superblock is written with its checksum**, through `jbd2_write_superblock` — otherwise `e2fsck` calls it corrupt and the next mount refuses it.
5. **No journal, no writes.** An external, corrupt or unreplayable journal mounts the volume read-only, as Linux does.

Verified by crashing the VM in the middle of heavy metadata churn: Linux `e2fsck` replays the log and finds nothing to fix; the driver's own replay at the next mount leaves the volume just as clean.

---

## Unwritten Extents

Windows programs usually set the file size first and write the data afterwards — `CopyFile` does exactly that. On ext4 the size change allocates **unwritten extents**: the blocks are reserved and contiguous, but they read as zeros until data is written into them. The write then converts exactly the blocks it covers.

The conversion is where the old code went wrong, twice:

| Problem | Effect | Fix |
|---|---|---|
| A write into the middle of an unwritten extent splits it in three. The first split let its two unwritten halves merge straight back into one, and the second split then marked everything right of the write as initialized | Blocks never written became readable: **stale disk contents visible from Linux** past the end of a large write | The first split passes `EXT4_GET_BLOCKS_PRE_IO`, as Linux `ext4_split_extent` does; the left extent is looked up again before the second split; a split outside its extent now fails with `-EIO` instead of wrapping `ee_len` |
| A block-cache miss left the run length at one block | Every write converted one block at a time and every block became its own extent: **a 1 MB file had 258 extents, an 8 MB file 1 955 and a two-level tree** | The block mapper is asked for the whole rest of the I/O at once and stops at the extent boundary by itself; a converted run is merged with its initialized neighbours |
| `SetEndOfFile` wrote zeros over the whole extension of every file | A 256 MB `SetLength` wrote 256 MB of zeros before the real data | Unwritten extents already read as zeros; only the tail of the old last block is cleared, as Linux `ext4_block_truncate_page` does. ext2/ext3 files, which have no unwritten state, still get the full range zeroed |
| Shrink skipped block truncation after an earlier allocating write | Regrowth exposed initialized data beyond the new end of file | Size changes truncate under the exclusive file resources; cached and uncached regrowth preserve the prefix and zero the newly exposed tail |
| Extent insertion rollback used the encoded unwritten length | ENOSPC could free blocks outside the allocation | Rollback uses the allocator's exact block count; regressions check free space, file contents and Linux metadata consistency |

Checked from Linux with `debugfs`: a 20 MB file written at an unaligned offset into a preallocated range is now **one initialized extent** followed by the untouched unwritten tail, and every byte outside the written range reads as zero.

---

## Performance Engineering

The design reduces work per I/O and contention between independent files.

| Mechanism | Engineering effect |
|---|---|
| Extent conversion per I/O run, followed by merging | Keeps sequential data contiguous and bounds extent-tree growth |
| Unwritten preallocation | Growing an extent-mapped file reserves blocks; only the initialized tail needs explicit zeroing |
| Allocation goals based on the inode's block group | Preserves locality and spreads allocations across the volume |
| CPU-scaled lock stripes for names, blocks and inodes | Independent groups and directories can progress concurrently |
| Shared resources for ordinary opens and lookups | Exclusive ownership is reserved for namespace changes |
| Interlocked free-space totals, folded into the superblock at commit and flush | Removes a shared superblock update from each allocation |
| Reference-counted buffer heads with serialized last-reference handling | Frequent releases avoid a global exclusive lock while preserving lifetime |
| Bounded cache reaping, deadlines and release notifications | Reclamation follows pressure and actual teardown progress |
| Directory name sets alongside htree lookup | Avoids a full directory scan for each case-insensitive create |
| Hardware CRC32C selected by CPUID and a check vector | Accelerates metadata checksums with a table-based fallback |
| Journal-aware flush ordering | Preserves durability while avoiding redundant completed device flushes |

Lock stripes use four stripes per logical processor across all processor groups, rounded up to a power of two and capped by the volume's group count. Stripes occupy separate cache lines. Workloads concentrated on one directory or block group remain a separate contention case.

For *T* concurrent holders choosing independently and uniformly among *S* stripes, the collision union bound is min(1, (*T* − 1) / *S*). This states the assumptions explicitly; benchmark workloads determine the actual contention. LUKS workers are bounded by CPU count and a fixed buffer-memory budget.

`tests/copybench.ps1` measures preallocated writes, streaming writes, copies and cached reads. Completed writes and copies are checked with SHA-256 outside the timed interval. `tests/fsbench.ps1` covers small-file operations, append-and-flush, random I/O and sequential I/O; uncached reads use aligned native buffers.

Comparisons use the same guest and storage backend, alternate the volume order and run with Driver Verifier disabled. Exact timings depend on the storage, cache state, CPU allocation and antivirus policy. Performance results are recorded alongside the tested build.

The latest same-guest repetitions show strong bulk-copy and file-creation performance, with ext4 ahead of NTFS in copying, creation, rename and deletion. NTFS leads in enumeration and append-and-flush workloads. Random and sequential I/O remain close in this setup.

---

## Stopping on Demand — `sc stop`

A file system driver on Windows is not meant to be unloaded: `IoRegisterFileSystem` marks the driver object as a base file system driver, and the I/O manager refuses `DriverUnload` for such a driver as long as it owns any device object — reference counts of zero are not enough. `ext4.sys` stops anyway, on a bare `sc stop ext4` or `net stop ext4`, with volumes mounted and handles open:

1. A no-wake kernel timer probes the unload-pending flag of the control device; nothing else in NT signals that a stop was requested.
2. A **drain thread** takes over: it asks the driver to prepare for unload, which requires empty volume lists, completed VCB teardown and reclaimed replacement VPBs. It waits for release notifications. A bounded short recheck covers references still held by the I/O manager after close.
3. Mounted volumes are dismounted as their handles go: the journal is committed and stopped, caches purged, data sections force-closed, drive letters withdrawn at once so nothing shows the volume as RAW.
4. Replacement VPBs are reclaimed only after their reference and device state permit it. Original VPBs are restored with their original persistent state. The control devices are then deleted and the last reference is dropped on a system worker thread, which runs `DriverUnload`.

Lifecycle tests cover idle stops, open files and directory watchers, repeated `sc stop` / `net stop`, and drive-letter restoration after restart. A busy handle delays unload until its reference is released.

While a volume is still busy the drain thread says why, in the kernel debug output: the counts that gate the teardown, and every file and file object that has not been closed yet - which is how the last reference leaks were found. Without a debugger, each step of the stop leaves its number in the service's registry key (`UnloadStep`, and `UnloadWaitStatus` for the last wait); the test runner prints both when a stop does not finish.

To replace the driver, run `sc stop ext4`, wait for `STOPPED`, copy the new `ext4.sys`, then load it through KVC or DrvLoader. `sc start` also works when the image is accepted by the machine's Code Integrity policy.

---

## Drive Letters for Linux Partitions

The volume manager creates a volume device for every partition, but the mount manager hands out drive letters only to partition types it knows. An MBR type `0x83` or a GPT "Linux filesystem" partition is treated as hidden: no letter, no `\\?\Volume{...}` link, and nobody opens it, so no file system is ever asked to mount it.

`volume\letter.c` listens for both hidden-volume and mounted-device arrivals, including devices already present when the driver loads. It opens each device once so the I/O manager asks the registered file systems to mount it. During reload, Mount Manager may remember `D:` after the live DOS link has gone. The worker restores that link before the open, breaking the wait between a missing path and an unmounted file system. After a successful mount, the driver registers the mounted-device interface on the volume PDO. Mount Manager then creates the `Volume{GUID}` link, restores its recorded letter and removes both when the device leaves. A new volume receives the first free letter from `D:`. `mountvol` and Disk Management see the same assignments.

Hot unplug with a handle open withdraws the letters in under half a second; replugging brings them back just as fast.

---

## Encrypted Volumes — LUKS

Most Linux installations encrypt their partitions with LUKS (dm-crypt): Fedora, Ubuntu and Debian with full-disk encryption, Qubes OS always. `ext4.sys` opens them the way Linux does, split the way Linux splits it:

```mermaid
flowchart TB
    CTL["ext4ctl unlock (user mode)<br/>LUKS1 / LUKS2 header, keyslot KDF<br/>PBKDF2 · Argon2i · Argon2id, AF-merge,<br/>digest check"]
    FS["ext4 file system<br/>(ext4.sys, unchanged)"]
    CRYPT["\Device\Ext4Crypt&lt;n&gt;<br/>AES-XTS per sector (CNG, AES-NI)<br/>ext4.sys"]
    PART["\Device\HarddiskVolumeN<br/>the LUKS partition"]
    CTL -- "volume key only<br/>IOCTL, administrators" --> CRYPT
    FS --> CRYPT --> PART
```

- **The passphrase never enters the kernel.** `ext4ctl` derives the keyslot key in user mode — Argon2 may ask for a gigabyte of memory, which has no place in a driver — decrypts and merges the key material and checks it against the header's digest. Only the verified volume key goes to the driver, which keeps nothing but the key schedule; `ext4ctl lock`, `sc stop` and shutdown destroy it.
- **The driver adds a disk device of its own** over the encrypted segment. Every sector is AES-XTS with the plain64 tweak, exactly as dm-crypt; the file system mounts on that device like on any partition and knows nothing about encryption. The mount manager is told about the device directly — it has no PnP node — and gives it a drive letter and a `\\?\Volume{...}` name; the LUKS UUID is its unique id.
- **Writes** are never encrypted in place — the caller's pages are the cache. An aligned write is encrypted in the caller's own thread straight into a buffer of its own (the first XTS whitening pass is the copy) and sent on without waiting; the disk's completion finishes it, with no thread switch on the way. **Reads** go to worker threads that read into the caller's pages and decrypt them there; long requests are cut into pieces the threads take in parallel. The workers' buffers are allocated up front, so a paging write always makes progress. LUKS2 sectors of 4 KiB are presented as 512e: a part of a sector is read, merged and rewritten under a lock.
- **Formats:** LUKS1 and LUKS2 (both header copies and their checksums), `aes-xts-plain64` with 256- or 512-bit keys, PBKDF2 (SHA-1 … SHA-512), Argon2i and Argon2id computed with one thread per lane. The KDF, AES-XTS and PBKDF2 code is checked against the RFC 9106, IEEE 1619 and RFC 6070 vectors (`ext4ctl selftest`).

```cmd
ext4ctl list                                    :: partitions: LUKS headers and ext file systems
ext4ctl unlock UUID=bfba416e-... --letter G     :: asks for the passphrase, mounts as G:
ext4ctl unlock 7 --key-file C:\keys\data.key    :: the whole file is the passphrase, as in cryptsetup
ext4ctl status                                  :: what is unlocked, where
ext4ctl lock G:                                 :: dismount, drop the key (--force with open files)
```

`ext4ctl` is a single executable with no runtime dependencies; it works the same over ssh and on Windows Server Core. A partition is named by its number, its NT device name or the UUID of its LUKS header, which unlike the volume number does not change when disks come and go.

The LUKS tests use cryptsetup-created containers, including Argon2id parameters used by Qubes OS and 4 KiB encryption sectors. Windows writes are checked by Linux after locking the containers: file hashes must match and `e2fsck` must report clean metadata.

### LVM inside LUKS — read-only

Qubes OS, and many a Fedora install, put an LVM volume group inside the LUKS container: in Qubes a thin pool holding dom0's root and the private volume of every qube. `ext4ctl lv` maps the logical volumes of an unlocked container as disk devices of their own, **read-only**:

- `ext4ctl` reads the PV label and the current copy of the volume group's text metadata, and turns a volume into a table of runs over the LUKS device. A linear volume is its segments; a thin volume is looked up in dm-thin's own metadata — the pool's superblock, the btree of devices, the btree of block mappings — with contiguous chunks merged and chunks never written left as holes that read as zeros. Thin snapshots are thin volumes of their own and open the same way.
- The driver checks the table (it starts at 0, has no gaps, is sector-aligned and stays inside the LUKS device) and serves reads straight from it, split along the runs and sent on to the LUKS device without waiting. Writes fail and the device reports itself write-protected, so ext4 mounts read-only.
- **Why read-only:** a write to a thin volume may need a new chunk from the pool, which means changing the pool's metadata — the space maps and btrees that Linux's dm-thin owns and checks at the next boot. Reading changes nothing in LVM or dm-thin; the Linux side stays exactly as it was.

```cmd
ext4ctl unlock UUID=0061a882-... --read-only    :: the container itself: no file system on it
ext4ctl lv list 0                               :: the volume group inside \Device\Ext4Crypt0
ext4ctl lv open 0 qubes_dom0/vm-work-private --letter W
ext4ctl lv close W:                             :: or: ext4ctl lock 0 --force closes both
```

Tested on the LVM partition of the test disk, made by LVM in Linux (a linear volume, and a thin volume in a pool with 64 KiB chunks): every file byte-exact against Linux's manifest, writes refused, the container refuses to lock while a volume is open (`--force` closes the volumes first), `sc stop` with the volumes mounted, and `e2fsck -f` in Linux clean afterwards.

---

## Security Boundaries

A file system driver runs every request of every process in kernel mode, so each input a caller controls is checked before it is used:

| Entry point | Rule |
|---|---|
| Control device `\\.\ext4` | Created with a SYSTEM + Administrators ACL; a standard user cannot open it |
| Private IOCTLs (volume settings, mount points, statistics) | Kernel callers or administrators only; buffers shorter than their header are refused before a byte is read; strings from the caller are bounded and terminated by the driver |
| Unload request | Kernel mode only — the service control manager's stop path, never a user program |
| `FSCTL_GET_RETRIEVAL_POINTERS` | Raw user pointers (METHOD_NEITHER): probed, and every store into them guarded, so a buffer unmapped by another thread ends the request, not the system |
| `FSCTL_SET_REPARSE_POINT` | Lengths and offsets checked against the buffer; creating a symlink needs `SeCreateSymbolicLinkPrivilege`, as on NTFS |
| Extended attribute queries | The name list is validated entry by entry before it is walked |
| LUKS unlock and lock | Control device only, administrators only; the request is checked field by field, the key in the request buffer is wiped before the answer is copied back, the plaintext of written data is wiped from the bounce buffers. The decrypted disk device is open to SYSTEM and administrators only; files on it are the file system's to check |

`tests\security.ps1` proves each rule as an administrator and as a standard user (a restricted token of the test thread).

---

## Source Layout

One responsibility per folder, one header per folder, `/W4 /WX` clean.

```
Ext4/
├── src/
│   ├── driver/        # DriverEntry, IRP dispatch and work queue, registry, unload drain thread
│   ├── fsd/           # One file per IRP family: create, read, write, cleanup, close,
│   │                  # fileinfo, dirctl, fsctl, lock, ea, reparse, pnp, fast I/O ...
│   ├── volume/        # Mount and verify, VCB, dismount, volume lock, drive letters,
│   │                  # LUKS disk devices (crypt.c), LVM volumes inside them (lvm.c)
│   ├── crypto/        # AES-XTS over CNG, shared by the driver and ext4ctl
│   ├── unicode/       # Casefolded directories: Linux's UTF-8 NFD / case folding tables
│   ├── core/          # FCB/CCB, MCB name cache, ICB inode table, extent lists, reapers
│   ├── ext4/          # Extent tree, htree, directory entries, block and inode allocators,
│   │                  # group descriptors, superblock, xattr, metadata checksums,
│   │                  # ext2/ext3 indirect block maps
│   ├── journal/       # jbd2: commit, checkpoint, log space, per-thread handles, replay
│   ├── linux/         # Buffer heads over the cache manager, kernel services, rbtree
│   ├── nls/           # Character set tables (cp437 ... cp1255, ISO-8859-x, KOI8, UTF-8)
│   ├── support/       # Disk I/O, pool, name conversion, errno ↔ NTSTATUS, time, debug
│   ├── include/       # ext4fs.h and one ext4fs_<area>.h per folder; linux/ compatibility headers
│   ├── ext4.vcxproj   # WDK kernel-mode driver project
│   └── ext4.rc        # Version resource, generated from include/version.h
├── tools/ext4ctl/     # LUKS unlock and driver control: LUKS1/2, Argon2, BLAKE2b, JSON,
│                      # LVM metadata and dm-thin mappings
├── tests/             # Guest and host test scripts (see Testing and Validation)
├── build.ps1          # Finds VS and the WDK by evidence, builds bin\ext4.sys and bin\ext4ctl.exe
└── ext4.slnx
```

The driver and control tool are split by responsibility. LUKS header formats, keyslots, metadata parsing, LVM mappings, passphrase handling and driver control have separate modules; on-disk layouts have compile-time offset checks.

---

## Testing and Validation

The current Release x64 build is warning-free under `/W4 /WX`. With Driver Verifier enabled for `ext4.sys`, it passed **80 consecutive stop/start cycles** and **2,400 shrink/regrow cases** across plain and LUKS2 volumes. File and directory handles were open during selected stops; drive letters returned and written data survived each restart. The guest stayed on the same boot throughout the series.

The final full Windows/Linux matrix also passed: functional operations, security boundaries, parallel I/O, namespace races, hot-plug, LUKS1/LUKS2, read-only LVM and Linux feature interoperability. Linux reported clean metadata and matching test manifests after Windows writes.

### Regression invariants

- **Allocation rollback:** extent insertion forced to fail after data allocation must return exactly the allocated block range, including an early-group reuse case. Free space, file length, contents and `e2fsck` are checked.
- **Object lifetime:** buffer-head release reads reference-protected state before dropping ownership. A model compiles the production VPB reclaimer and exercises busy references, nested swaps, persistent flags and allocation failure.
- **Resize correctness:** cached and uncached handles shrink and regrow files across sector and block boundaries. Every retained byte must match; every newly exposed byte must be zero.
- **Unload:** teardown includes outstanding VCB destruction and replacement VPBs. A busy object remains owned until it can be reclaimed.
- **Tester correctness:** child exit codes and completion summaries are both required. An unhandled exception counts as failure; an empty or interrupted run is rejected.

```mermaid
flowchart LR
    BUILD["Build<br/>/W4 /WX"] --> WIN["Windows guest<br/>functional, resize, security,<br/>LUKS/LVM, races, lifecycle"]
    WIN --> LINUX["Detach test media<br/>e2fsck + Linux read-back"]
    LINUX --> HASH["Manifest hashes<br/>contents, flags, links and xattrs"]
```

```powershell
# Full matrix on the configured Hyper-V guest, including Linux media checks
pwsh tests\run-ext4test.ps1 -Interop -Stress -System -Luks -Features

# Functional checks and Linux media verification
pwsh tests\run-ext4test.ps1 -Quick

# Malformed images from the e2fsprogs corpus
pwsh tests\robust.ps1
```

Configure the disposable guest and test-image paths in `tests/testenv.local.psd1`; that file stays local. The host runner transfers guest scripts over SSH and checks native process results. Formatting and write tests use dedicated fixtures.

| Area | Coverage |
|---|---|
| File operations | Cached, uncached and write-through I/O; append, overwrite, truncation, regrowth, mappings and ENOSPC |
| Namespace | Long and international names, directory indexes, rename/replace, hard links, symlinks, share modes, locks and delete-pending |
| Parallel access | Concurrent create/delete churn, write/resize/read-back and opens racing namespace changes |
| Security | Short and invalid IOCTL buffers, retrieval pointers, restricted-user access and EA namespace preservation |
| Linux interoperability | Ownership, modes, nanosecond timestamps, links, xattrs, SELinux labels and inherited inode flags |
| Encryption and LVM | LUKS1/LUKS2 key derivation and digest checks, sector-boundary I/O, lock/unlock, read-only linear and thin logical volumes |
| Linux features | Casefolded and large indexed directories, immutable/append-only flags, MMP, shared xattrs, `ea_inode` and `inline_data` |
| Media integrity | Linux `e2fsck` and manifest-based content verification after Windows writes |
| Malformed media | Bounded traversal and read-back, attempted writes, dismount and Verifier checks; mount refusal and read-only fallback are recorded separately |

Driver Verifier runs are correctness checks. Throughput measurements use a separate run with Verifier disabled.

---

## Supported Features

| ext4 feature | Support |
|---|---|
| `extent`, `huge_file`, `large_file`, `sparse_super`, `flex_bg`, `meta_bg`, `64bit` group descriptors | Read / write |
| `has_journal` (internal jbd2) | Read / write, journaled; replay at mount |
| `dir_index` (htree), `filetype`, `dir_nlink`, `extra_isize` | Read / write |
| `large_dir` (three-level htree, directories past 2 GiB) | Read / write: index blocks split at every level and a level is added under the root when all are full, as in Linux; tested with a three-level index the kernel built and one this driver grew from empty (45 000 names), both checked by `e2fsck` and by the kernel looking every name up through the index |
| `metadata_csum`, `gdt_csum`, `csum_seed` | Read / write, checksums verified and maintained |
| `orphan_file` (the `mkfs.ext4` default since e2fsprogs 1.47) | Read / write |
| `casefold` (`mkfs -O casefold`, `chattr +F`; SteamOS formats its microSD cards so) | Read / write. Names in a casefolded directory are folded as Linux folds them (NFD and full case folding, Unicode 12.1, with the kernel's own tables): the index is hashed on the folded names, `ŁÓDŹ.TXT` finds `Łódź.txt` and `STRASSE.TXT` finds `straße.txt`, a name that folds to an existing one is refused, strict mode refuses names that are not UTF-8, and directories made inside inherit the flag |
| Unwritten (preallocated) extents | Read / write |
| Extended attributes | Read / write, exposed as Windows EAs in the `user.` namespace, names matched without regard to case as Windows expects; other namespaces (POSIX ACLs, security labels, `trusted.`) are neither shown nor touched - replacing a file's EA set replaces its `user.` attributes only |
| SELinux labels (`security.selinux`) | A file or directory made from Windows takes the label of its directory, as the kernel labels when the policy names no transition, so Fedora sees a labelled file, not `unlabeled_t` |
| `chattr +i`, `+a` and the inherited flags | Honoured as on Linux: an immutable file is not written, truncated, renamed, deleted or linked, and shows as read-only; an append-only file only grows at its end; an immutable directory takes and loses no entries, an append-only one only takes them. New inodes inherit their directory's flags (`EXT4_FL_INHERITED`) |
| Symbolic links, hard links | Read / write; symlinks appear as reparse points |
| Nanosecond timestamps and the epoch bits past 2038 | Read / write |
| ext2, ext3 (indirect block maps) | Read / write |
| `bigalloc`, `quota`, `project`, `verity`, `orphan_present` | Mounted read-only |
| `mmp` (multi-mount protection) | Read / write, by the Linux protocol: a volume `e2fsck` is checking mounts read-only, one left in use waits out its owner's check interval and is taken over only if the owner stays silent, a volume another node writes to meanwhile turns read-only with nothing more written; while mounted the sequence is updated on the Linux schedule, and an orderly dismount leaves the volume released. The node name `debugfs dump_mmp` shows is the computer name, the device `ext4.sys` |
| `ea_inode` (xattr values in inodes of their own, up to 64 KiB from Windows) | Read / write, as Linux keeps them: a value too large for the xattr block goes to a value inode (crc32c of the value in its `i_atime`, reference count in `i_ctime` : `i_version`), charged to the owner's `i_blocks`; values two files share (Linux deduplicates them) are counted, and a value inode goes with its last reference - when the attribute is replaced or removed, or the file deleted. Checked by `e2fsck` and by the kernel, which verifies every value against its hash as it reads it |
| Shared xattr blocks (`h_refcount` > 1) | Copy on write: an inode whose attributes change takes a block of its own and leaves the shared one to the others |
| `inline_data` (small files and directories inside their inode) | Read / write. Read where Linux keeps them - `i_block` and the `system.data` attribute, directory entries included (a directory is listed and searched as the block it would be). Anything that changes one moves it to a block first, as Linux does when inline data no longer fits: the file's bytes are written to their block before the inode that points there is journaled |
| `encrypt` | Not mounted |

---

## Known Limitations

1. **Fewer than 2³² blocks per volume — approximately 16 TiB with 4 KiB blocks.** The driver accepts the ext4 `64bit` descriptor format, but its Windows block-mapping interfaces still carry 32-bit physical block numbers. Larger volumes are refused at mount. Wider addressing is a separate engineering task.
2. **Microsoft production signing is pending.** KVC and DrvLoader provide the controlled loading paths; HVCI systems may require a reboot before the first load.
3. **No external journals.** A volume whose journal lives on another device is mounted read-only.
4. **POSIX permissions are mapped, not enforced as ACLs.** Ownership can be overridden per volume (`uid`, `gid` in the registry); Windows security descriptors are not stored.
5. **LUKS and LVM.** Ciphers other than AES-XTS are not supported. Logical volumes inside LUKS open read-only, linear (one stripe) and thin ones; striped, mirrored and RAID volumes, a volume group spanning several containers, and a partition table inside a volume (the root volumes of Qubes OS qubes) are not handled — the private volumes of Qubes OS qubes hold ext4 directly and open.
6. **x64 only.** The code has no architecture-specific parts, but only x64 is built and tested.

---

## Installation

Microsoft production signing is pending. Until it is available, use one of the three controlled loading paths below: DrvLoader, the KVC runtime driver manager or the KVC SMSS boot loader. All commands require an elevated Command Prompt or PowerShell session and a machine you administer.

> **What the loaders do:** DrvLoader and KVC do not change the signature of `ext4.sys`. They temporarily open the Code Integrity path, start the driver, and restore DSE immediately afterwards. If Memory Integrity (HVCI) is active, the machine must reboot before an unsigned driver can enter the kernel.

### DrvLoader — load for the current Windows session

Download DrvLoader with KernelResearchKit from the [KernelResearchKit project page](https://kvc.pl/repositories/kernelresearchkit) or the [KernelResearchKit repository on GitHub](https://github.com/wesmar/KernelResearchKit). Extract `ext4.7z` and KernelResearchKit, then place the driver at a stable path. DrvLoader derives the service name `ext4` from `ext4.sys`; start type `3` is `DEMAND_START`.

```cmd
copy /y ext4.sys "%SystemRoot%\System32\drivers\ext4.sys"
drvloader.exe load "%SystemRoot%\System32\drivers\ext4.sys" -s 3

sc query ext4
fsutil fsinfo drives
```

The command performs one bounded cycle: stop an old instance if present, patch DSE, start `ext4`, and restore DSE. If DrvLoader reports that Memory Integrity is enabled, answer `Y`, let Windows restart, open an elevated terminal and run the same `drvloader.exe load ...` command again. Declining the reboot is suitable only for inspecting offsets; do not attempt to load the driver while HVCI remains active.

The interactive path is equivalent: start `drvloader.exe`, select **Load Driver**, enter the full path to `ext4.sys`, and select start type `3 — DEMAND`.

```cmd
drvloader.exe reload ext4
sc stop ext4
drvloader.exe remove ext4
```

`reload` is useful after replacing the binary during development. `remove` stops the driver and deletes its SCM service entry; it does not delete `ext4.sys`.

### KVC — runtime load with automatic DSE restoration

Download KVC from the [KVC project page](https://kvc.pl/project-kvc) or the [KVC repository on GitHub](https://github.com/wesmar/kvc). Its runtime driver manager supports `load`, `reload`, `stop` and `remove`. It accepts either a full path or a short driver name; a short name resolves to `%SystemRoot%\System32\drivers\<name>.sys`:

```cmd
copy /y ext4.sys "%SystemRoot%\System32\drivers\ext4.sys"
kvc driver load ext4 -s 3

sc query ext4
fsutil fsinfo drives
```

KVC restores DSE after the service starts. On an HVCI system it offers to disable Memory Integrity and reboot; after that reboot, run `kvc driver load ext4 -s 3` again. The driver then remains loaded for the current boot, while DSE is back in its original state.

```cmd
kvc driver reload ext4
sc stop ext4
kvc driver remove ext4
```

### KVC — load during the SMSS boot phase

This is separate from the runtime `kvc driver load/reload` path above. For repeated test boots, `kvc install` registers the native SMSS loader. Keep `ext4.sys` under `System32\drivers`, register it once, then reboot:

```cmd
copy /y ext4.sys "%SystemRoot%\System32\drivers\ext4.sys"
kvc install ext4
shutdown /r /t 0
```

`kvc install ext4` writes the `ext4` entry to `C:\Windows\drivers.ini`, registers `kvc_smss.exe` in `BootExecute`, and uses the boot-time kernel scanner by default; no PDB download or hard-coded kernel offset is required. On a machine with HVCI enabled, the first SMSS pass can schedule one additional reboot. The following pass loads `ext4.sys`, restores the Code Integrity callback, and continues the normal boot.

Verify and remove the boot path with:

```cmd
sc query ext4
fsutil fsinfo drives

sc stop ext4
kvc driver remove ext4
kvc uninstall smss
```

Do not move or replace `ext4.sys` while it is loaded. Stop the service first. Open file handles can delay `sc stop ext4`; the driver completes the stop as soon as the last mounted volume is released.

### Mounted volumes and settings

The Linux partitions get drive letters within a second. `sc stop ext4` takes them away again and unloads the driver. LUKS partitions are opened with `ext4ctl unlock` (see [Encrypted Volumes — LUKS](#encrypted-volumes--luks)); copy `ext4ctl.exe` anywhere on the path.

Optional settings live under `HKLM\SYSTEM\CurrentControlSet\Services\ext4\Parameters`: `WritingSupport`, `CheckingBitmap`, `Ext3ForceWriting`, `CodePage`, `HidingPrefix` / `HidingSuffix`, `AutoMount`, and per volume under `Volumes` — `Readonly`, `CodePage`, `MountPoint`, `uid`, `gid`.

> **Before unplugging a disk:** a surprise removal is handled, but the journal gives the guarantee only for what was committed. Eject the volume or stop the service first when you can.

---

## Building from Source

**Requirements:** Visual Studio 2022 or 2026 with the **Desktop development with C++** workload and the **Windows Driver Kit** matching an installed Windows SDK.

```powershell
git clone https://github.com/wesmar/Ext4.git
Set-Location .\Ext4
.\build.ps1                     # Release: bin\ext4.sys, symbols\ext4.pdb
.\build.ps1 -Configuration Debug
.\build.ps1 -Rebuild            # full rebuild; -Clean deletes bin\ and obj\ first
```

`build.ps1` discovers the toolchain from files on disk. A Visual Studio installation qualifies only when it contains `cl.exe` and the kernel-mode driver toolset; `vswhere -products *` also reports products without a C++ compiler. The script selects the highest complete MSVC toolset and Windows Kit by parsed version, then verifies the required kernel headers, libraries and tools. This works with both old and current kits.

The project compiles with `/W4 /WX /std:clatest` in Release and Debug — no warnings, and none suppressed to get there. After a successful build `obj\` is deleted and the PDB files move to `symbols\`; `bin\` contains `ext4.sys` and `ext4ctl.exe`.

The two configurations differ in what they carry, not in how they behave. `DBG`, set by the WDK for Debug only, switches `EXT2_DEBUG`: the Debug build adds assertions, a breakpoint at every internal inconsistency, level-filtered tracing of each IRP with process names and `NTSTATUS` texts, guard bytes and accounting on every pool allocation, and the full object and IRP statistics (`IOCTL_APP_QUERY_PERFSTAT`). The Release build keeps error messages and the one counter the driver itself runs on (cached names, which drive the name-cache reaper) - no per-operation statistics, no shared counters every CPU writes to. The current Release driver is under 1 MB; most of it is character set tables.

| Output | Description |
|---|---|
| `bin\ext4.sys` | The driver |
| `bin\ext4ctl.exe` | LUKS unlock and volume control tool |
| `symbols\ext4.pdb` | Symbols for crash dump analysis, kept out of `bin\` |

---

## Downloads

| Download | Contents |
|---|---|
| [`ext4.7z`](https://github.com/wesmar/Ext4/releases/download/latest/ext4.7z) | Current x64 `ext4.sys` (under 1 MB) and `ext4ctl.exe`; no password |
| [Source tree](https://github.com/wesmar/Ext4) | Driver, control tool, build system, tests and documentation |

The binary archive is the current x64 build:

```text
ext4.7z
├── ext4.sys       under 1 MB
└── ext4ctl.exe
```

The repository excludes binaries, PDB symbols, local utilities, virtual disks and test logs. Two test passphrase defaults are represented as `<TEST_LUKS_PASSPHRASE>`.

---

## License

GNU General Public License version 2 — see [LICENSE.md](LICENSE.md) and [COPYING](COPYING).

- **Author:** Marek Wesołowski (WESMAR)
- **Contact:** marek@wesolowski.eu.org
- **Based on:** Ext2Fsd by Matt Wu, Ext4Fsd by Bo Branten, and the Linux ext4 / jbd2 code
- **Build host:** Windows 10/11 x64, MSVC + WDK, C23
- **Runtime:** Windows 10 / 11 x64
