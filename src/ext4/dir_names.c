/**
 * dir_names.c - the caseless names of a large directory, kept in memory.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Names are matched without regard to ASCII case (Windows semantics,
 * ext3_match), while the directory index hashes the exact spelling. A miss
 * in the index is therefore not final: an entry spelt in another case could
 * exist, and only a scan of the whole directory could tell. Every create
 * asks for a name that is not there, so each one read the whole directory:
 * creating n files in one directory cost O(n^2) - 45 000 files with long
 * names took seven minutes.
 *
 * A large directory now keeps a set of the 32-bit hashes of its names
 * folded to upper case. A folded hash that is not in the set proves no
 * spelling of the name exists, and the scan is skipped; one that is in it
 * (a real case variant, or a collision) still scans. The set is built by
 * one pass on the first miss that needs it, takes the names added later,
 * and keeps those removed (a false "maybe" costs only the scan) until they
 * are half of it, when it is dropped and built again on demand.
 *
 * A build runs without the lock: a name added meanwhile moves Generation,
 * and a set that may have missed it is not published.
 */

#include "ext4fs.h"
#include "htree_internal.h"

#define EXT4_NAMES_TAG          'NC4E'
#define EXT4_NAMES_MIN_BLOCKS   8           /* smaller directories just scan */
#define EXT4_NAMES_MAX_SLOTS    (1u << 24)  /* 64 MiB: beyond it, scan */
#define EXT4_NAMES_FIRST_SLOTS  1024
#define FNV_OFFSET              2166136261u
#define FNV_PRIME               16777619u

struct _EXT4_DIR_NAMES {
    ULONG   Mask;           /* slots - 1, a power of two minus one */
    ULONG   Used;           /* hashes held */
    ULONG   Stale;          /* of them, names removed since */
    ULONG   Slot[1];        /* 0: empty; a hash is stored with its low bit set */
};

/* FNV-1a of the name with a-z folded to A-Z, as _strnicmp compares */
static ULONG Ext4NameHash(const char *Name, int Length)
{
    ULONG h = FNV_OFFSET;
    int   i;

    for (i = 0; i < Length; i++) {
        UCHAR c = (UCHAR)Name[i];
        if (c >= 'a' && c <= 'z') {
            c = (UCHAR)(c - 'a' + 'A');
        }
        h = (h ^ c) * FNV_PRIME;
    }
    return h | 1;
}

static PEXT4_DIR_NAMES Ext4NamesAlloc(ULONG Slots)
{
    PEXT4_DIR_NAMES Set;
    SIZE_T          Bytes = FIELD_OFFSET(EXT4_DIR_NAMES, Slot) + (SIZE_T)Slots * sizeof(ULONG);

    Set = Ext2AllocatePool(PagedPool, Bytes, EXT4_NAMES_TAG);
    if (Set) {
        RtlZeroMemory(Set, Bytes);
        Set->Mask = Slots - 1;
    }
    return Set;
}

static VOID Ext4NamesFree(PEXT4_DIR_NAMES Set)
{
    if (Set) {
        Ext2FreePool(Set, EXT4_NAMES_TAG);
    }
}

static BOOLEAN Ext4NamesHas(PEXT4_DIR_NAMES Set, ULONG Hash)
{
    ULONG i = Hash & Set->Mask;

    while (Set->Slot[i]) {
        if (Set->Slot[i] == Hash) {
            return TRUE;
        }
        i = (i + 1) & Set->Mask;
    }
    return FALSE;
}

static VOID Ext4NamesPut(PEXT4_DIR_NAMES Set, ULONG Hash)
{
    ULONG i = Hash & Set->Mask;

    while (Set->Slot[i]) {
        if (Set->Slot[i] == Hash) {
            return;
        }
        i = (i + 1) & Set->Mask;
    }
    Set->Slot[i] = Hash;
    Set->Used++;
}

/* the same hashes in a table twice the size; NULL when out of memory */
static PEXT4_DIR_NAMES Ext4NamesGrow(PEXT4_DIR_NAMES Set)
{
    PEXT4_DIR_NAMES Bigger;
    ULONG           i;

    if (Set->Mask + 1 >= EXT4_NAMES_MAX_SLOTS) {
        return NULL;
    }
    Bigger = Ext4NamesAlloc((Set->Mask + 1) * 2);
    if (Bigger) {
        for (i = 0; i <= Set->Mask; i++) {
            if (Set->Slot[i]) {
                Ext4NamesPut(Bigger, Set->Slot[i]);
            }
        }
        Bigger->Stale = Set->Stale;
    }
    return Bigger;
}

/* the Icb a directory inode is embedded in, or NULL */
static PEXT2_ICB Ext4DirIcb(struct inode *dir)
{
    PEXT2_ICB Icb = CONTAINING_RECORD(dir, EXT2_ICB, Inode);

    return (Icb->Identifier.Type == EXT2ICB && S_ISDIR(dir->i_mode)) ? Icb : NULL;
}

/* one pass over the directory; NULL when it is small, huge or unreadable */
static PEXT4_DIR_NAMES Ext4NamesBuild(struct ext2_icb *icb, struct inode *dir)
{
    struct super_block *sb = dir->i_sb;
    ULONG               blocks = (ULONG)(Ext4DirSize(dir) >> EXT3_BLOCK_SIZE_BITS(sb));
    ULONG               slots = EXT4_NAMES_FIRST_SLOTS, b;
    PEXT4_DIR_NAMES     Set;

    if (blocks < EXT4_NAMES_MIN_BLOCKS) {
        return NULL;
    }
    /* about a hundred short names per 4 KiB block: start near the need */
    while (slots < EXT4_NAMES_MAX_SLOTS &&
           slots < blocks * (sb->s_blocksize / 16)) {
        slots *= 2;
    }
    Set = Ext4NamesAlloc(slots);
    if (Set == NULL) {
        return NULL;
    }

    for (b = 0; b < blocks; b++) {
        struct ext3_dir_entry_2 *de, *top;
        struct buffer_head      *bh;
        int                      err;

        bh = ext3_bread(icb, dir, b, &err);
        if (bh == NULL) {
            Ext4NamesFree(Set);
            return NULL;
        }
        de = (struct ext3_dir_entry_2 *)bh->b_data;
        top = (struct ext3_dir_entry_2 *)(bh->b_data + sb->s_blocksize - EXT3_DIR_REC_LEN(0));
        while (de <= top) {
            unsigned rec = ext3_rec_len_from_disk(de->rec_len);

            if (rec < EXT3_DIR_REC_LEN(0) || (char *)de + rec > bh->b_data + sb->s_blocksize ||
                (unsigned)EXT3_DIR_REC_LEN(de->name_len) > rec) {
                break;          /* damaged: what was read is still right */
            }
            if (de->inode && de->name_len) {
                if (Set->Used * 2 >= Set->Mask + 1) {
                    PEXT4_DIR_NAMES Bigger = Ext4NamesGrow(Set);
                    Ext4NamesFree(Set);
                    if (Bigger == NULL) {
                        brelse(bh);
                        return NULL;
                    }
                    Set = Bigger;
                }
                Ext4NamesPut(Set, Ext4NameHash(de->name, de->name_len));
            }
            de = (struct ext3_dir_entry_2 *)((char *)de + rec);
        }
        brelse(bh);
    }
    return Set;
}

BOOLEAN
Ext4DirNameMayExist(struct ext2_icb *icb, struct inode *dir, const char *name, int len)
{
    PEXT2_ICB       Icb = Ext4DirIcb(dir);
    PEXT4_DIR_NAMES Set, Built;
    ULONG           Hash = Ext4NameHash(name, len);
    LONG            Generation;
    BOOLEAN         Maybe = TRUE;

    if (Icb == NULL) {
        return TRUE;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Icb->CaselessLock);
    Set = Icb->Caseless;
    if (Set) {
        Maybe = Ext4NamesHas(Set, Hash);
    }
    ExReleasePushLockShared(&Icb->CaselessLock);
    KeLeaveCriticalRegion();
    if (Set) {
        return Maybe;
    }

    Generation = Icb->CaselessGeneration;
    Built = Ext4NamesBuild(icb, dir);
    if (Built == NULL) {
        return TRUE;
    }
    Maybe = Ext4NamesHas(Built, Hash);

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Icb->CaselessLock);
    if (Icb->Caseless == NULL && Icb->CaselessGeneration == Generation) {
        Icb->Caseless = Built;
        Built = NULL;
    }
    ExReleasePushLockExclusive(&Icb->CaselessLock);
    KeLeaveCriticalRegion();
    Ext4NamesFree(Built);

    return Maybe;
}

VOID
Ext4DirNameAdded(struct inode *dir, const char *name, int len)
{
    PEXT2_ICB       Icb = Ext4DirIcb(dir);
    PEXT4_DIR_NAMES Set, Old = NULL;

    if (Icb == NULL) {
        return;
    }
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Icb->CaselessLock);
    InterlockedIncrement(&Icb->CaselessGeneration);
    Set = Icb->Caseless;
    if (Set) {
        if (Set->Used * 2 >= Set->Mask + 1) {
            /* full enough: twice the size, or no set at all */
            Old = Set;
            Set = Icb->Caseless = Ext4NamesGrow(Old);
        }
        if (Set) {
            Ext4NamesPut(Set, Ext4NameHash(name, len));
        }
    }
    ExReleasePushLockExclusive(&Icb->CaselessLock);
    KeLeaveCriticalRegion();
    Ext4NamesFree(Old);
}

VOID
Ext4DirNameRemoved(struct inode *dir)
{
    PEXT2_ICB       Icb = Ext4DirIcb(dir);
    PEXT4_DIR_NAMES Old = NULL;

    if (Icb == NULL) {
        return;
    }
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Icb->CaselessLock);
    if (Icb->Caseless) {
        Icb->Caseless->Stale++;
        if (Icb->Caseless->Stale * 2 > Icb->Caseless->Used) {
            Old = Icb->Caseless;
            Icb->Caseless = NULL;
        }
    }
    ExReleasePushLockExclusive(&Icb->CaselessLock);
    KeLeaveCriticalRegion();
    Ext4NamesFree(Old);
}

VOID
Ext4DirNamesFree(PEXT2_ICB Icb)
{
    Ext4NamesFree(Icb->Caseless);
    Icb->Caseless = NULL;
}
