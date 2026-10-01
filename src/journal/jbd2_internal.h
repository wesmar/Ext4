/**
 * jbd2_internal.h - definitions shared by the files split from journal.c.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_JOURNAL_JBD2_INTERNAL_H_
#define _EXT4_JOURNAL_JBD2_INTERNAL_H_

int jbd2_journal_create_slab(size_t slab_size);

int __init journal_init(void);

void __exit journal_exit(void);

#endif /* _EXT4_JOURNAL_JBD2_INTERNAL_H_ */
