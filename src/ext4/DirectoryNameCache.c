/**
 * DirectoryNameCache.c - the caseless names of a large directory, kept in memory.
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
 * A large directory keeps the 32-bit hashes of its names folded to upper
 * case, each with the number of its names: a multiset. A folded hash it does
 * not hold proves no spelling of the name exists, and the scan is skipped;
 * one it holds (a real case variant, or a collision) still scans. The set is
 * built by one pass on the first miss that needs it, then counts the names
 * added and removed - a name made again after its removal, or moved back,
 * skips the scan too (a set that only grew sent each of those through the
 * whole directory: 50 ms a rename in one of 2 000 blocks).
 *
 * The table is NameSet.c: open addressing with linear probing, a hash whose
 * count drops to 0 leaving by backward shift (no tombstones).
 *
 * A build runs without the lock: a name added meanwhile moves Generation,
 * and a set that may have missed it is not published. A name removed
 * meanwhile may stay counted - a false "maybe", which costs only a scan. A
 * removal of a hash the set does not hold means the set is wrong: it goes,
 * and is built again on demand.
 */

#include "ext4fs.h"
#include "NameSet.h"

#define EXT4_NAMES_TAG          'NC4E'
#define EXT4_NAMES_MIN_BLOCKS   8           /* smaller directories just scan */
#define EXT4_NAMES_MAX_SLOTS    (1u << 23)  /* 64 MiB: beyond it, scan */
#define EXT4_NAMES_FIRST_SLOTS  1024

/* (NS_SET is the EXT4_DIR_NAMES the Icb holds) */
struct _EXT4_DIR_NAMES {
    NS_SET  Set;
};

static PEXT4_DIR_NAMES Ext4NamesAlloc(ULONG Slots)
{
    PEXT4_DIR_NAMES Names;
    SIZE_T          Bytes = (SIZE_T)NsBytes(Slots);

    Names = Ext2AllocatePool(PagedPool, Bytes, EXT4_NAMES_TAG);
    if (Names) {
        RtlZeroMemory(Names, Bytes);
        NsInit(&Names->Set, Slots);
    }
    return Names;
}

static VOID Ext4NamesFree(PEXT4_DIR_NAMES Names)
{
    if (Names) {
        Ext2FreePool(Names, EXT4_NAMES_TAG);
    }
}

/* the same counts in a table twice the size; NULL when out of memory */
static PEXT4_DIR_NAMES Ext4NamesGrow(PEXT4_DIR_NAMES Names)
{
    PEXT4_DIR_NAMES Bigger;

    if (Names->Set.Mask + 1 >= EXT4_NAMES_MAX_SLOTS) {
        return NULL;
    }
    Bigger = Ext4NamesAlloc((Names->Set.Mask + 1) * 2);
    if (Bigger) {
        NsCopy(&Bigger->Set, &Names->Set);
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
                if (Set->Set.Used * 2 >= Set->Set.Mask + 1) {
                    PEXT4_DIR_NAMES Bigger = Ext4NamesGrow(Set);
                    Ext4NamesFree(Set);
                    if (Bigger == NULL) {
                        brelse(bh);
                        return NULL;
                    }
                    Set = Bigger;
                }
                NsPut(&Set->Set, NsHash(de->name, de->name_len), 1);
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
    ULONG           Hash = NsHash(name, len);
    LONG            Generation;
    BOOLEAN         Maybe = TRUE;

    if (Icb == NULL) {
        return TRUE;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Icb->CaselessLock);
    Set = Icb->Caseless;
    if (Set) {
        Maybe = NsCount(&Set->Set, Hash) != 0;
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
    Maybe = NsCount(&Built->Set, Hash) != 0;

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
        if (Set->Set.Used * 2 >= Set->Set.Mask + 1) {
            /* full enough: twice the size, or no set at all */
            Old = Set;
            Set = Icb->Caseless = Ext4NamesGrow(Old);
        }
        if (Set) {
            NsPut(&Set->Set, NsHash(name, len), 1);
        }
    }
    ExReleasePushLockExclusive(&Icb->CaselessLock);
    KeLeaveCriticalRegion();
    Ext4NamesFree(Old);
}

VOID
Ext4DirNameRemoved(struct inode *dir, const char *name, int len)
{
    PEXT2_ICB       Icb = Ext4DirIcb(dir);
    PEXT4_DIR_NAMES Old = NULL;

    if (Icb == NULL) {
        return;
    }
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Icb->CaselessLock);
    if (Icb->Caseless && !NsTake(&Icb->Caseless->Set, NsHash(name, len))) {
        Old = Icb->Caseless;
        Icb->Caseless = NULL;
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
