/**
 * DirBlock.h - the blocks of an ext4 directory: the entries of a leaf, and
 * the hash index (htree) above the leaves.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * Plain C over byte buffers, every field little-endian at its offset: the
 * driver and the host models compile the same code (DirBlock.c, DirIndex.c).
 */

#ifndef _EXT4_DIR_BLOCK_H_
#define _EXT4_DIR_BLOCK_H_

#define DB_OK           0
#define DB_ENOENT       (-2)
#define DB_ENOMEM       (-12)
#define DB_EEXIST       (-17)
#define DB_ENOSPC       (-28)
#define DB_ECORRUPT     (-117)

/* an entry: inode, the record's length, the name's length, file type, name */
#define DB_E_INODE      0
#define DB_E_REC_LEN    4
#define DB_E_NAME_LEN   6
#define DB_E_TYPE       7
#define DB_E_NAME       8
#define DB_NAME_MAX     255
#define DB_REC_LEN(n)   ((8u + (unsigned int)(n) + 3u) & ~3u)  /* the room for an n-byte name */

/* with metadata_csum a leaf ends in a checksum tail: an empty 12-byte
   entry of type 0xDE whose last word is the checksum */
#define DB_TAIL_SIZE    12u
#define DB_TAIL_TYPE    0xDEu

/*
 * The index. Its root is block 0: '.' (12 bytes), '..' (to the end of the
 * block), the root info at 24, then the entries; a node below it is one
 * empty entry spanning the block (8 bytes), then the entries. An array of
 * entries begins with its limit and count, in the place of the first entry's
 * hash (that child takes every hash below the second's), then 8 bytes each:
 * hash, block. With metadata_csum a checksum tail of 8 bytes follows the
 * limit of entries.
 */
#define DX_ROOT_INFO        24u
#define DX_INFO_VERSION     4u      /* offsets within the root info */
#define DX_INFO_LENGTH      5u
#define DX_INFO_LEVELS      6u
#define DX_INFO_FLAGS       7u
#define DX_INFO_SIZE        8u
#define DX_NODE_ENTRIES     8u
#define DX_ENTRY_SIZE       8u
#define DX_TAIL_SIZE        8u
#define DX_BLOCK_MASK       0x0FFFFFFFu     /* the high bits are not the block's */
#define DX_HASH_CONTINUED   1u      /* this child continues its left neighbour's hash */

typedef struct _DB_GEOMETRY {
    unsigned int    BlockSize;
    unsigned int    Csum;           /* metadata_csum: leaf and index tails */
    unsigned int    InodesCount;    /* an entry naming a higher inode is corrupt */
    unsigned int    FileTypes;      /* the filetype feature: entries carry a type */
} DB_GEOMETRY;

/* a live entry of a leaf being split: where it is, its record, its hash */
typedef struct _DB_MAP {
    unsigned int    Hash;
    unsigned int    Offset;
    unsigned int    Size;
} DB_MAP;

static __inline unsigned int DbGet16(const unsigned char *p) { return p[0] | (unsigned int)p[1] << 8; }
static __inline unsigned int DbGet32(const unsigned char *p)
{
    return p[0] | (unsigned int)p[1] << 8 | (unsigned int)p[2] << 16 | (unsigned int)p[3] << 24;
}
static __inline void DbPut16(unsigned char *p, unsigned int v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static __inline void DbPut32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* the region of a leaf entries live in: the block, its tail excepted */
static __inline unsigned int DbLeafSize(const DB_GEOMETRY *G)
{
    return G->BlockSize - (G->Csum ? DB_TAIL_SIZE : 0);
}

/* the record length on disk: 65536 does not fit in 16 bits */
unsigned int DbRecLen(const DB_GEOMETRY *G, const unsigned char *Entry);
void DbSetRecLen(const DB_GEOMETRY *G, unsigned char *Entry, unsigned int Length);

/* the entry at Offset of a region of Size bytes is sound (Linux
   __ext4_check_dir_entry): DB_OK or DB_ECORRUPT */
int DbCheck(const DB_GEOMETRY *G, const unsigned char *Block, unsigned int Size, unsigned int Offset);

/* where a name of Length bytes fits: DB_OK and *Offset, DB_ENOSPC,
   DB_EEXIST when Match says an entry is that name, DB_ECORRUPT */
typedef int (*DB_MATCH)(void *Context, const unsigned char *Entry);
int DbFindSpace(const DB_GEOMETRY *G, const unsigned char *Block, unsigned int Size, unsigned int Length,
                DB_MATCH Match, void *Context, unsigned int *Offset);

/* the live entry Match accepts: DB_OK and *Offset, DB_ENOENT, DB_ECORRUPT */
int DbSearch(const DB_GEOMETRY *G, const unsigned char *Block, unsigned int Size,
             DB_MATCH Match, void *Context, unsigned int *Offset);

/* a one-block directory DxMakeRoot can index */
int DxCanIndex(const DB_GEOMETRY *G, const unsigned char *Block0);

/* the entry, where DbFindSpace found room */
void DbInsert(const DB_GEOMETRY *G, unsigned char *Block, unsigned int Offset, unsigned int Inode,
              unsigned int Type, const unsigned char *Name, unsigned int Length);

/* the entry at Offset goes: its room to the one before it */
int DbDelete(const DB_GEOMETRY *G, unsigned char *Block, unsigned int Size, unsigned int Offset);

/* an empty leaf, and the tail alone */
void DbInitLeaf(const DB_GEOMETRY *G, unsigned char *Block);
void DbInitTail(const DB_GEOMETRY *G, unsigned char *Block);

/* the live entries of a leaf with their hashes: their number, or an error */
typedef int (*DB_HASH)(void *Context, const unsigned char *Name, unsigned int Length, unsigned int *Hash);
int DbMap(const DB_GEOMETRY *G, const unsigned char *Block, DB_HASH Hash, void *Context, DB_MAP *Map);

/*
 * A full leaf split in two: Map (DbMap's, Count entries) is sorted, the
 * upper part by hash moves to New, both are packed and closed. *Split is
 * the first hash of New, with DX_HASH_CONTINUED when the one before it in
 * Old has the same hash.
 */
void DbSplit(const DB_GEOMETRY *G, unsigned char *Old, unsigned char *New, DB_MAP *Map,
             unsigned int Count, unsigned int *Split);

/* ---- the index ---- */

typedef struct _DX_ROOT {
    unsigned int    Version;        /* the hash version, as on disk */
    unsigned int    Levels;         /* index levels below the root */
    unsigned int    Entries;        /* the offset of its entries */
} DX_ROOT;

unsigned int DxRootLimit(const DB_GEOMETRY *G, unsigned int InfoLength);
unsigned int DxNodeLimit(const DB_GEOMETRY *G);

static __inline unsigned int DxLimit(const unsigned char *E) { return DbGet16(E); }
static __inline unsigned int DxCount(const unsigned char *E) { return DbGet16(E + 2); }
static __inline unsigned int DxHash(const unsigned char *E, unsigned int i) { return DbGet32(E + i * DX_ENTRY_SIZE); }
static __inline unsigned int DxBlock(const unsigned char *E, unsigned int i)
{
    return DbGet32(E + i * DX_ENTRY_SIZE + 4) & DX_BLOCK_MASK;
}

/* the root and a node checked (Linux dx_probe): DB_OK or DB_ECORRUPT */
int DxReadRoot(const DB_GEOMETRY *G, const unsigned char *Block, unsigned int MaxLevels, DX_ROOT *Root);
int DxReadNode(const DB_GEOMETRY *G, const unsigned char *Block, unsigned int *Entries);

/* the child to look for Hash in: the last entry whose hash is not above it */
unsigned int DxFind(const unsigned char *E, unsigned int Hash);

/* a child after entry At */
void DxInsert(unsigned char *E, unsigned int At, unsigned int Hash, unsigned int Block);

/* a node block with Count entries copied from E (first one's hash dropped) */
void DxInitNode(const DB_GEOMETRY *G, unsigned char *Block, const unsigned char *E, unsigned int Count);

/*
 * A one-block directory becomes indexed: its entries after '..' move to
 * Leaf, block 0 becomes the root with one child, block LeafBlock. DB_OK
 * or DB_ECORRUPT (the block is not sound).
 */
int DxMakeRoot(const DB_GEOMETRY *G, unsigned char *Block0, unsigned char *Leaf,
               unsigned int LeafBlock, unsigned int Version);

#endif /* _EXT4_DIR_BLOCK_H_ */
