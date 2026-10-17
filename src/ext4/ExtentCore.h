/**
 * ExtentCore.h - the extent tree of an ext4 inode: which blocks of the disk
 * hold which blocks of a file, and how the tree grows and shrinks.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * Plain C, no system header: the driver and the host models compile it.
 * Every block goes through EX_IO, which reads, writes, allocates and frees.
 */

#ifndef _EXT4_EXTENT_CORE_H_
#define _EXT4_EXTENT_CORE_H_

#define EX_OK           0
#define EX_ENOENT       (-2)        /* a hole */
#define EX_EIO          (-5)
#define EX_ENOMEM       (-12)
#define EX_ENOSPC       (-28)
#define EX_EBADCRC      (-74)       /* a node failed its checksum (the caller's check) */
#define EX_ECORRUPT     (-117)

/* the format */
#define EX_MAGIC            0xF30Au
#define EX_HEADER           12u     /* magic, entries, max, depth, generation */
#define EX_ENTRY            12u     /* an extent, or an index entry */
#define EX_ROOT_BYTES       60u     /* i_block */
#define EX_ROOT_MAX         4u      /* entries the root holds */
#define EX_MAX_DEPTH        5u      /* levels below the root, at most */
#define EX_INIT_MAX         32768u  /* blocks of a written extent, at most */
#define EX_UNWRITTEN_MAX    32767u  /* of an unwritten one */
#define EX_END              0x100000000ull  /* past every logical block */

/* header fields */
#define EX_H_MAGIC      0
#define EX_H_ENTRIES    2
#define EX_H_MAX        4
#define EX_H_DEPTH      6

/* an extent: logical block, length (with the unwritten bit), physical start */
#define EX_E_BLOCK      0
#define EX_E_LEN        4
#define EX_E_START_HI   6
#define EX_E_START_LO   8
/* an index entry: the first logical block below it, the child node */
#define EX_I_BLOCK      0
#define EX_I_LEAF_LO    4
#define EX_I_LEAF_HI    8

typedef struct _EX_EXTENT {
    unsigned int        Block;      /* first logical block */
    unsigned int        Length;     /* blocks */
    unsigned long long  Start;      /* first physical block */
    int                 Unwritten;  /* allocated, reads as zeros */
} EX_EXTENT;

typedef struct _EX_IO {
    void   *Context;
    /* a node of the tree: Checked 1 when it passed EX's checks before (they
       are not made again); the caller verifies the checksum */
    int   (*Get)(void *Context, unsigned long long Block, unsigned char **Data, void **Handle, int *Checked);
    void  (*Checked)(void *Context, void *Handle);
    void  (*Put)(void *Context, void *Handle);
    /* a node changed (Handle 0: the root, in the inode) */
    int   (*Dirty)(void *Context, void *Handle);
    /* a new node near Goal, its contents undefined */
    int   (*New)(void *Context, unsigned long long Goal, unsigned long long *Block,
                 unsigned char **Data, void **Handle);
    /* blocks the tree gives up: a node (Node 1) or data */
    int   (*Free)(void *Context, unsigned long long Block, unsigned int Count, int Node);
    /* blocks a file may hold: inside the volume, none of its own metadata */
    int   (*Valid)(void *Context, unsigned long long Block, unsigned int Count);
    /* a node not sound (0: the root) */
    void  (*Corrupt)(void *Context, unsigned long long Block);
} EX_IO;

typedef struct _EX_TREE {
    unsigned char      *Root;       /* the inode's i_block, 60 bytes */
    unsigned int        BlockSize;
    unsigned long long  Goal;       /* where the blocks of an empty tree go */
    EX_IO               Io;
} EX_TREE;

/* the entries a node of a block holds (leaf or index alike) */
static __inline unsigned int ExBlockMax(const EX_TREE *T) { return (T->BlockSize - EX_HEADER) / EX_ENTRY; }

/* an empty tree in the root */
void ExInit(EX_TREE *T);

/* the root, checked as a node (its children are checked when read) */
int ExCheckRoot(const EX_TREE *T);

/*
 * The extent holding Block (EX_OK), or EX_ENOENT for a hole: then *Next is
 * the first mapped block after it (EX_END: none), and *Goal where blocks
 * for it should go (next to the extent before it, as Linux places them).
 */
int ExMap(const EX_TREE *T, unsigned int Block, EX_EXTENT *Found, unsigned long long *Next,
          unsigned long long *Goal);

/* New into a hole (EX_ECORRUPT if it is not one), merged with an extent
   next to it when Merge allows; the tree grows as it must */
int ExInsert(const EX_TREE *T, const EX_EXTENT *New, int Merge);

/* blocks Block..Block+Count-1, all in one unwritten extent, written now:
   split off and merged with written neighbours */
int ExConvert(const EX_TREE *T, unsigned int Block, unsigned int Count);

/* every block from From on unmapped and freed, the tree shrunk with them */
int ExTruncate(const EX_TREE *T, unsigned int From);

#endif /* _EXT4_EXTENT_CORE_H_ */
