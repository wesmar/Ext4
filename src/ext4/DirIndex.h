/**
 * DirIndex.h - walking and growing an indexed (htree) directory.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 */

#ifndef _EXT4_DIR_INDEX_H_
#define _EXT4_DIR_INDEX_H_

#include "DirBlock.h"

#define DX_LEVELS_MAX   3       /* the root and two levels of nodes (large_dir) */
#define DX_NEXT_ALWAYS  1u      /* DxNextLeaf: go on whatever the next hash */

/* the directory's blocks and names, as the caller has them */
typedef struct _DX_IO {
    void   *Context;
    /* block Block of the directory, as an index block (Index) or a leaf */
    int   (*Get)(void *Context, unsigned int Block, int Index, unsigned char **Data, void **Handle);
    void  (*Put)(void *Context, void *Handle);
    /* a new block at the directory's end */
    int   (*Append)(void *Context, unsigned int *Block, unsigned char **Data, void **Handle);
    /* a block changed: its checksum, its write */
    int   (*Dirty)(void *Context, void *Handle, int Index);
    /* the hash of a name, by version (the unsigned ones included) */
    int   (*Hash)(void *Context, const unsigned char *Name, unsigned int Length, unsigned int Version,
                  unsigned int *Major, unsigned int *Minor);
    void *(*Allocate)(void *Context, unsigned int Bytes);
    void  (*Release)(void *Context, void *Memory);
    /* a block found not sound: an index block (Index) or a leaf; may be NULL */
    void  (*Corrupt)(void *Context, unsigned int Block, int Index);
} DX_IO;

typedef struct _DX_FRAME {
    void           *Handle;
    unsigned char  *Data;
    unsigned int    Entries;        /* the offset of the entries */
    unsigned int    At;             /* the entry followed */
} DX_FRAME;

typedef struct _DX_PATH {
    DX_FRAME        Frame[DX_LEVELS_MAX];
    unsigned int    Depth;          /* frames in use: the root and the nodes */
    unsigned int    Version;        /* the hash version names are hashed by */
    unsigned int    Hash;
    unsigned int    Minor;
} DX_PATH;

typedef struct _DX_DIR {
    DB_GEOMETRY     G;
    unsigned int    MaxLevels;      /* 2, or 3 with large_dir */
    unsigned int    UnsignedDelta;  /* 0, or 3 on a volume of unsigned char */
    DX_IO           Io;
} DX_DIR;

/* the path to the leaf for a name - or, with Name NULL, for Hash - and its
   hash. DB_ECORRUPT: the index is not sound (Linux then reads the
   directory as a plain one). */
int DxProbe(const DX_DIR *D, const unsigned char *Name, unsigned int Length, unsigned int Hash,
            DX_PATH *Path);
void DxRelease(const DX_DIR *D, DX_PATH *Path);

/* the leaf after the path's: 1 and the path moved, 0 at the end - or when
   Hash is even and the next leaf does not continue it - or an error;
   *StartHash: the next leaf's first hash */
int DxNextLeaf(const DX_DIR *D, DX_PATH *Path, unsigned int Hash, unsigned int *StartHash);

/* the leaf block the path stands on */
static __inline unsigned int DxLeaf(const DX_PATH *Path)
{
    const DX_FRAME *f = &Path->Frame[Path->Depth - 1];
    return DxBlock(f->Data + f->Entries, f->At);
}

/* the entry of a name: DB_OK with its leaf held (*Handle, *Data) and its
   offset, DB_ENOENT, or an error */
int DxLookup(const DX_DIR *D, const unsigned char *Name, unsigned int Length, DB_MATCH Match,
             void *Context, void **Handle, unsigned char **Data, unsigned int *Offset);

/* a name added: DB_OK, DB_EEXIST, DB_ENOSPC (the index is full), or an error */
int DxAdd(const DX_DIR *D, const unsigned char *Name, unsigned int Length, unsigned int Inode,
          unsigned int Type, DB_MATCH Match, void *Context);

/* the one-block directory in Block0 (held, changed in place) becomes
   indexed, by Version, with the name added; *Converted: block 0 became the
   root (the directory is indexed from then on, whatever the result) */
int DxMakeIndexed(const DX_DIR *D, void *Handle0, unsigned char *Block0, unsigned int Version,
                  const unsigned char *Name, unsigned int Length, unsigned int Inode, unsigned int Type,
                  int *Converted);

#endif /* _EXT4_DIR_INDEX_H_ */
