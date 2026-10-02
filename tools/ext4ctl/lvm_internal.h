/**
 * lvm_internal.h - LVM2 and dm-thin on disk, and what the LVM files share among themselves.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The structures as LVM2 (lib/format_text/layout.h, lib/label/label.h) and
 * the kernel's dm-thin (drivers/md/dm-thin-metadata.c, persistent-data
 * btree) lay them out. Every integer is little-endian, so every field is
 * bytes, read through Le32/Le64; the offsets are checked at compile time.
 */

#ifndef _LVM_INTERNAL_H_
#define _LVM_INTERNAL_H_

#include "ext4ctl.h"

#define LVM_SECTOR          512
#define LVM_LABEL_SCAN      4                   /* the label is in one of the first sectors */
#define LVM_MAX_TEXT        (8 * 1024 * 1024)
#define LVM_MAX_DEPTH       32                  /* sections nested in the metadata text */
#define LVM_ARENA_FACTOR    4                   /* parse arena: text size times this ... */
#define LVM_ARENA_EXTRA     4096                /* ... plus this */
#define LVM_PV_ID_CHARS     32                  /* a PV uuid without its dashes */
#define EXT4CTL_FIRST_RUNS  1024                /* a run table starts this large, then doubles */

#define THIN_BLOCK          4096
#define THIN_MAGIC          27022010ULL
#define THIN_INTERNAL_NODE  1
#define THIN_LEAF_NODE      2
#define THIN_MAX_DEPTH      16
#define THIN_TIME_BITS      24                  /* mapping value: data block << 24 | time */
#define THIN_VALUE_BYTES    8                   /* every btree this tool reads maps to a u64 */

/* ---------------------------------------------------------------- on disk */

typedef struct _LVM_LABEL_HEADER {
    char    Id[8];                      /* "LABELONE" */
    UINT8   Sector[8];
    UINT8   Crc[4];
    UINT8   Offset[4];                  /* of the PV header, from the label's own start */
    char    Type[8];                    /* "LVM2 001" */
} LVM_LABEL_HEADER;

typedef struct _LVM_DISK_LOCN {
    UINT8   Offset[8];                  /* bytes; a list ends with an all-zero entry */
    UINT8   Size[8];
} LVM_DISK_LOCN;

typedef struct _LVM_PV_HEADER {
    char            Uuid[LVM_PV_ID_CHARS];
    UINT8           DeviceSize[8];
    LVM_DISK_LOCN   Areas[1];           /* data areas, 0, metadata areas, 0 */
} LVM_PV_HEADER;

typedef struct _LVM_RAW_LOCN {
    UINT8   Offset[8];                  /* of the text, from the start of the metadata area */
    UINT8   Size[8];
    UINT8   Checksum[4];
    UINT8   Flags[4];
} LVM_RAW_LOCN;

typedef struct _LVM_MDA_HEADER {
    UINT8           Checksum[4];
    char            Magic[16];          /* " LVM2 x[5A%r0N*>", no NUL */
    UINT8           Version[4];
    UINT8           Start[8];
    UINT8           Size[8];
    LVM_RAW_LOCN    Raw[1];             /* [0]: the current copy of the text */
} LVM_MDA_HEADER;

#define LVM_MDA_HEADER_BYTES    LVM_SECTOR  /* the header sector; a wrapped text goes on after it */

C_ASSERT(FIELD_OFFSET(LVM_LABEL_HEADER, Offset) == 20);
C_ASSERT(FIELD_OFFSET(LVM_LABEL_HEADER, Type) == 24);
C_ASSERT(FIELD_OFFSET(LVM_PV_HEADER, Areas) == 40);
C_ASSERT(sizeof(LVM_DISK_LOCN) == 16);
C_ASSERT(FIELD_OFFSET(LVM_MDA_HEADER, Magic) == 4);
C_ASSERT(FIELD_OFFSET(LVM_MDA_HEADER, Raw) == 40);

typedef struct _THIN_SUPERBLOCK {
    UINT8   Checksum[4];
    UINT8   Flags[4];
    UINT8   Blocknr[8];                 /* 0: the superblock is block 0 */
    UINT8   Uuid[16];
    UINT8   Magic[8];                   /* THIN_MAGIC */
    UINT8   Version[4];
    UINT8   Time[4];
    UINT8   TransactionId[8];
    UINT8   MetadataSnap[8];
    UINT8   DataSpaceMapRoot[128];
    UINT8   MetadataSpaceMapRoot[128];
    UINT8   DataMappingRoot[8];         /* device id -> the device's mapping btree */
    UINT8   DeviceDetailsRoot[8];
    UINT8   DataBlockSize[4];           /* in 512-byte sectors: the pool's chunk size */
} THIN_SUPERBLOCK;

typedef struct _THIN_NODE_HEADER {
    UINT8   Checksum[4];
    UINT8   Flags[4];                   /* THIN_INTERNAL_NODE, THIN_LEAF_NODE */
    UINT8   Blocknr[8];                 /* the node's own block */
    UINT8   NrEntries[4];
    UINT8   MaxEntries[4];
    UINT8   ValueSize[4];
    UINT8   Padding[4];
    /* UINT64 keys[MaxEntries], then values[MaxEntries] */
} THIN_NODE_HEADER;

C_ASSERT(FIELD_OFFSET(THIN_SUPERBLOCK, Magic) == 32);
C_ASSERT(FIELD_OFFSET(THIN_SUPERBLOCK, DataMappingRoot) == 320);
C_ASSERT(FIELD_OFFSET(THIN_SUPERBLOCK, DataBlockSize) == 336);
C_ASSERT(FIELD_OFFSET(THIN_NODE_HEADER, NrEntries) == 16);
C_ASSERT(sizeof(THIN_NODE_HEADER) == 32);

static __inline UINT32 Le32(const UINT8 *p) { UINT32 v; memcpy(&v, p, sizeof(v)); return v; }
static __inline UINT64 Le64(const UINT8 *p) { UINT64 v; memcpy(&v, p, sizeof(v)); return v; }

/* ---------------------------------------------------------------- text metadata (lvm_text.c) */

typedef enum { LVM_SECTION, LVM_VALUE, LVM_LIST } LVM_KIND;

typedef struct _LVM_NODE {
    LVM_KIND            Kind;
    const char         *Name;               /* NULL for list items */
    const char         *Value;              /* LVM_VALUE: text or number */
    struct _LVM_NODE   *Child;              /* LVM_SECTION, LVM_LIST */
    struct _LVM_NODE   *Next;
} LVM_NODE;

typedef struct _LVM_TEXT {
    char       *Arena;
    size_t      Used, Size;
    const char *p, *End;
    int         Depth;
    LVM_NODE   *Root;                       /* the top-level assignments and sections */
} LVM_TEXT;

/* Text parsed into T (arena on the process heap; LvmFreeText) */
BOOL            LvmParseText(LVM_TEXT *T, const char *Text, size_t Length);
void            LvmFreeText(LVM_TEXT *T);

const LVM_NODE *LvmGet(const LVM_NODE *Section, const char *Name);
BOOL            LvmDecimal(const char *Text, UINT64 *Out);
BOOL            LvmU64(const LVM_NODE *Section, const char *Name, UINT64 *Out);
const char     *LvmStr(const LVM_NODE *Section, const char *Name);
BOOL            LvmHasFlag(const LVM_NODE *Section, const char *Name, const char *Flag);
BOOL            LvmIsSegment(const LVM_NODE *Node);

/* ---------------------------------------------------------------- the volume group (lvm_vg.c) */

typedef struct _LVM_VG {
    LUKS_DEVICE     Dev;                    /* the LUKS device = the PV */
    char           *Text;
    LVM_TEXT        Parse;
    const LVM_NODE *Vg;                     /* the VG section */
    const char     *VgName;
    const char     *PvName;                 /* "pv0": the one on this device */
    UINT64          ExtentBytes;
    UINT64          PeStart;                /* bytes */
} LVM_VG;

typedef struct _RUNS {
    EXT4_LV_RUN *Run;
    ULONG        Count, Room;
    BOOL         Failed;
} RUNS;

typedef enum { LV_LINEAR, LV_THIN, LV_OTHER } LV_TYPE;

BOOL            LvmLoad(LVM_VG *G, ULONG CryptIndex, BOOL Quiet);
void            LvmClose(LVM_VG *G);
const LVM_NODE *LvmFindLv(LVM_VG *G, const char *Name);
BOOL            LvmLinearToPv(LVM_VG *G, const LVM_NODE *Lv, UINT64 Off, UINT64 *Pv, UINT64 *Left);
const LVM_NODE *LvmOnlySegment(const LVM_NODE *Lv, UINT64 *Extents);
LV_TYPE         LvmType(const LVM_NODE *Lv, UINT64 ExtentBytes, UINT64 *Size);
BOOL            LvmRuns(LVM_VG *G, const LVM_NODE *Lv, RUNS *R, UINT64 *Size, ULONG *Mapped);
void            RunsAdd(RUNS *R, UINT64 Start, UINT64 Length, UINT64 Target);
void            RunsFree(RUNS *R);
const char     *LvmFsName(LVM_VG *G, const RUNS *R);

/* ---------------------------------------------------------------- dm-thin (lvm_thin.c) */

BOOL            LvmThinRuns(LVM_VG *G, const LVM_NODE *Lv, UINT64 Size, RUNS *R, ULONG *Mapped);

#endif /* _LVM_INTERNAL_H_ */
