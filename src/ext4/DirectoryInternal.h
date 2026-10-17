/**
 * DirectoryInternal.h - what the directory files share: a directory's
 * geometry for DirBlock.c, the name match, the change of a directory.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 */

#ifndef _EXT4_DIRECTORY_INTERNAL_H_
#define _EXT4_DIRECTORY_INTERNAL_H_

#include "DirBlock.h"
#include "DirIndex.h"

/* the volume's directory geometry: block size, tails, inode count, types */
VOID Ext4DirGeometry(IN struct inode *Dir, OUT DB_GEOMETRY *G);

/* a name looked for, as DirBlock.c's DB_MATCH sees it (ext4_match) */
typedef struct _EXT4_DIR_NAME {
    struct inode   *Dir;
    const char     *Name;
    int             Length;
} EXT4_DIR_NAME, *PEXT4_DIR_NAME;

int Ext4DirMatch(void *Context, const unsigned char *Entry);

/* a directory whose entries changed: its times, version, inode */
int Ext4DirTouched(IN struct ext2_icb *Icb, IN struct inode *Dir);

/* corruption found in a block of a directory, recorded */
VOID Ext4DirCorrupt(IN struct inode *Dir, IN struct buffer_head *Bh);

/* a name compared with an entry, as Windows or a casefolded directory does */
int ext4_match(struct inode *dir, int len, const char *name, struct ext3_dir_entry_2 *de);

/* the file type an entry carries for an inode's mode */
unsigned char ext3_type_by_mode(umode_t mode);

/* an add to an indexed directory (ERR_BAD_DX_DIR: read it as a plain one) */
int ext3_dx_add_entry(struct ext2_icb *icb, struct dentry *dentry, struct inode *inode);

/* a full directory of one block made indexed, the name added; bh released */
int make_indexed_dir(struct ext2_icb *icb, struct dentry *dentry,
                     struct inode *inode, struct buffer_head *bh);

#endif /* _EXT4_DIRECTORY_INTERNAL_H_ */
