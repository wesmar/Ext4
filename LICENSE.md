# GNU General Public License, version 2

## ext4.sys — ext2/ext3/ext4 file system driver for Windows

**Copyright (c) 2026 Marek Wesolowski (WESMAR)**

---

## License

This program is free software; you can redistribute it and/or modify it under the terms of
the **GNU General Public License version 2** as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful, but **WITHOUT ANY WARRANTY**;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details.

The full license text is in [COPYING](COPYING).

The license is not a choice made for this project alone: the driver is derived from code
published under the GPL version 2, and the same terms apply to the whole of it.

---

## Origins and copyright holders

`ext4.sys` is derived from the following works. Their copyright notices are kept in the
source files they apply to.

| Work | Copyright holders | License |
|---|---|---|
| Ext2Fsd — Ext2 File System Driver for Windows | Matt Wu, KaHo Ng | GPL-2.0 |
| Ext4Fsd — continuation of Ext2Fsd | Bo Branten | GPL-2.0 |
| Linux kernel: `fs/ext4`, `fs/jbd2`, `lib/rbtree.c`, `fs/nls` | Linus Torvalds, Stephen C. Tweedie, Red Hat, Cluster File Systems and other Linux contributors | GPL-2.0 |
| lwext4 — extended attribute code | Grzegorz Kostka, KaHo Ng | BSD-3-Clause, notice kept in `src/include/linux/ext4_xattr.h` |
| Unicode Character Database 12.1.0 — the data in `src/unicode/utf8data.c` | Unicode, Inc. | Unicode Data Files and Software License |
| unicode.org MAPPINGS (Microsoft code pages, ISO/IEC 8859, KOI8) — the data in `src/nls/nls_*.c` | Microsoft Corporation, Unicode, Inc. | Unicode terms of use |

The case folding of casefolded directories - `src/unicode` with its interface
`src/include/ext4fs_unicode.h`, the generator `tools/unicode/make-utf8data.py` and the model
`tests/unicode-model` - is new work by Marek Wesolowski under the MIT license, written to
produce the bytes Linux's `fs/unicode` produces, not derived from its code. MIT is compatible
with the GPL; the driver as a whole remains under GPL-2.0. The tables hold data of the Unicode
Character Database, Copyright (c) 1991-2019 Unicode, Inc., distributed under the Unicode terms
of use (https://www.unicode.org/copyright.html).

The code page converters - `src/nls/CharsetMap.c`, `src/nls/CharsetMap.h` and the 37
`src/nls/nls_<code page>.c` files generated from the published mapping tables, the generator
`tools/nls/make-nls.py` and its lists in `tools/nls/established` - are new work by Marek
Wesolowski under the MIT license. They reproduce the mappings of the converters the driver
inherited from Linux `fs/nls` (commit 4706af9, the test reference); the code page registry,
UTF-8 and the EUC-JP and KOI8-RU adapters in `src/nls` remain Linux code under GPL-2.0.
`tools/lineage/lineage.py`, which measures how much of the code is new, is MIT-licensed as well.

The restructuring into its present layout, the rewritten subsystems (service stop and unload,
drive letters, reapers, registry, journal commit engine, extent conversion) and the test
suite are the work of Marek Wesolowski.

---

## Project Information

- **Project:** ext4.sys — ext2/ext3/ext4 read/write file system driver for Windows
- **Author:** Marek Wesolowski (WESMAR)
- **Contact:** marek@wesolowski.eu.org
- **Platform:** Windows 10/11 x64
- **Language:** C (kernel mode, WDK)

---

## Responsible Use

A file system driver writes directly to disks. Test it on a virtual disk or a copy of your
data first, keep backups of anything that matters, and use it only on disks you own or have
permission to modify.

---

*Copyright (c) 2026 Marek Wesolowski (WESMAR). Distributed under the terms of the GNU General Public License version 2.*
