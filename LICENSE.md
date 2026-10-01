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
