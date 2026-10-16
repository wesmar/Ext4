/**
 * Orphans.c - inodes a crash left half deleted or half truncated.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * An inode that loses its last name while it is still open, or whose
 * truncation is under way, goes on an orphan list first: the superblock's
 * s_last_orphan names the latest, each orphan's i_dtime the next one (the
 * classic list); with the orphan_file feature Linux keeps them in the
 * blocks of a special file instead, arrays of inode numbers each closed by
 * a magic and a checksum. A crash leaves them there, and the next writable
 * mount finishes the work, as Linux's ext4_orphan_cleanup and e2fsck's
 * release_orphan_inodes do:
 *
 *  O1  an orphan that still has links is cut to its size: the blocks past
 *      i_size are freed;
 *  O2  an orphan without links goes - its blocks, its extended attributes,
 *      then the inode;
 *  O3  each orphan leaves its list in the transaction that finishes it;
 *      one that cannot be finished stays on it, for e2fsck;
 *  O4  an orphan must be one: an inode number past the reserved ones and
 *      within the volume, allocated in its bitmap, of a kind that can be
 *      cut when it keeps links, and (on the classic list) pointing at a
 *      next one within the volume. A bad one is recorded as corruption;
 *      the classic list is then given up, a bad orphan file entry skipped;
 *  O5  a volume with errors recorded is left as it is - only its classic
 *      list is dropped, as Linux drops it: e2fsck has to look at it anyway;
 *  O6  the orphan file must be sound, every block with its magic and,
 *      with metadata_csum, its checksum; one that is not leaves the volume
 *      read-only. This driver puts no orphan into the file (it uses the
 *      classic list, which Linux and e2fsck process as well), so once the
 *      file is empty the orphan_present flag goes.
 * Linux processes orphans only at mount: a volume switched to writable
 * with orphans waiting stays read-only (Ext4OrphansPending).
 */

#include "ext4fs.h"

#define ORPHAN_BLOCK_MAGIC  0x0b10ca04u
#define ORPHAN_TAIL         8           /* magic, checksum */
#define ORPHAN_TAG          'roxE'

typedef struct _ORPHAN_FILE {
    PEXT2_MCB   Mcb;
    ULONG       Blocks;
    ULONG       PerBlock;               /* inode numbers in a block */
    ULONG       CsumSeed;
    PULONGLONG  Where;                  /* the volume block of each block */
} ORPHAN_FILE, *PORPHAN_FILE;

/* an inode by number, loaded, in an Mcb of its own */
static PEXT2_MCB
OrOpen(IN PEXT2_VCB Vcb, IN ULONG Ino)
{
    PEXT2_MCB Mcb = Ext2AllocateMcb(Vcb, NULL, 0);

    if (!Mcb)
        return NULL;
    if (!Ext2AttachIcb(Vcb, Mcb, Ino)) {
        Ext2FreeMcb(Vcb, Mcb);
        return NULL;
    }
    if (!IsFlagOn(Mcb->Icb->Flags, ICB_INODE_LOADED)) {
        if (!Ext2LoadInode(Vcb, Mcb->Inode)) {
            Ext2FreeMcb(Vcb, Mcb);
            return NULL;
        }
        SetLongFlag(Mcb->Icb->Flags, ICB_INODE_LOADED);
    }
    return Mcb;
}

/* O4, Linux ext4_orphan_get: NULL for a bad orphan */
static PEXT2_MCB
OrGet(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN ULONG Ino, IN BOOLEAN OnList)
{
    struct super_block *sb = &Vcb->sb;
    PEXT2_MCB           Mcb;
    struct inode       *Inode;
    BOOLEAN             Allocated = FALSE;
    BOOLEAN             Cut;

    if (Ino < EXT4_FIRST_INO(sb) || Ino > INODES_COUNT)
        return NULL;
    if (!NT_SUCCESS(Ext2InodeAllocated(IrpContext, Vcb, Ino, &Allocated)) || !Allocated)
        return NULL;
    Mcb = OrOpen(Vcb, Ino);
    if (!Mcb)
        return NULL;
    Inode = Mcb->Inode;
    Cut = S_ISREG(Inode->i_mode) || S_ISDIR(Inode->i_mode) ||
          (S_ISLNK(Inode->i_mode) && !ext4_inode_is_fast_symlink(Inode));
    if ((Inode->i_nlink && !Cut) || (OnList && Inode->i_dtime > INODES_COUNT)) {
        Ext2FreeMcb(Vcb, Mcb);
        return NULL;
    }
    return Mcb;
}

/* the classic list's head moves to Next */
static VOID
OrSetHead(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN ULONG Next)
{
    Ext2LockSuper(Vcb);
    Vcb->SuperBlock->s_last_orphan = cpu_to_le32(Next);
    (void)Ext2SaveSuper(IrpContext, Vcb);
    Ext2UnlockSuper(Vcb);
}

/*
 * O1/O2/O3 for one orphan, in one journal scope. From the classic list it
 * leaves the list first and goes back on it if it cannot be finished
 * (TRUE: finished). An orphan file entry is cleared by the caller.
 */
static BOOLEAN
OrFinish(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb, IN BOOLEAN OnList)
{
    struct inode   *Inode = Mcb->Inode;
    NTSTATUS        Status;

    if (OnList)
        OrSetHead(IrpContext, Vcb, Inode->i_dtime);

    if (Inode->i_nlink) {
        LARGE_INTEGER Size;

        /* O1: the blocks past the size go, the inode leaves the list */
        Size.QuadPart = Inode->i_size;
        Inode->i_dtime = 0;
        Status = Ext2TruncateFile(IrpContext, Vcb, Mcb, &Size);
        if (!NT_SUCCESS(Status) && OnList)
            (void)Ext2AddOrphan(IrpContext, Vcb, Inode);
        if (!Ext2SaveInode(IrpContext, Vcb, Inode) && NT_SUCCESS(Status))
            Status = STATUS_UNEXPECTED_IO_ERROR;
    } else {
        ULONG   Ino = Inode->i_ino;
        BOOLEAN Allocated = TRUE;

        /* O2: as the last name's delete does; one that keeps blocks it
           cannot give back stays allocated (back on the classic list) */
        Status = Ext2ReleaseInode(IrpContext, Vcb, NULL, Mcb, OnList);
        if (NT_SUCCESS(Status))
            Status = Ext2InodeAllocated(IrpContext, Vcb, Ino, &Allocated);
        if (NT_SUCCESS(Status) && Allocated)
            Status = STATUS_DISK_FULL;
    }
    if (!NT_SUCCESS(Status))
        DbgPrint("ext4: orphan inode %u could not be finished (%08x), e2fsck will\n",
                 Inode->i_ino, Status);
    return NT_SUCCESS(Status);
}

/* ---- the orphan file ---- */

static ULONG
OrBlockSum(IN PEXT2_VCB Vcb, IN PORPHAN_FILE F, IN ULONGLONG Block, IN PUCHAR Data)
{
    __le64 where = cpu_to_le64(Block);
    ULONG  sum = ext4_chksum(&Vcb->sbi, F->CsumSeed, (__u8 *)&where, sizeof(where));
    return ext4_chksum(&Vcb->sbi, sum, Data, F->PerBlock * sizeof(__le32));
}

static VOID
OrCloseFile(IN PEXT2_VCB Vcb, IN PORPHAN_FILE F)
{
    if (F->Where)
        Ext2FreePool(F->Where, ORPHAN_TAG);
    if (F->Mcb)
        Ext2FreeMcb(Vcb, F->Mcb);
    RtlZeroMemory(F, sizeof(*F));
}

/* open the orphan file and map its blocks; FALSE with no file */
static NTSTATUS
OrOpenFile(IN PEXT2_VCB Vcb, OUT PORPHAN_FILE F, OUT PBOOLEAN Present)
{
    struct super_block *sb = &Vcb->sb;
    ULONG               ino = le32_to_cpu(Vcb->SuperBlock->s_orphan_file_inum);
    PEXT2_EXTENT        chain = NULL, e;
    ULONG               next = 0;
    __le32              id;
    NTSTATUS            Status;

    RtlZeroMemory(F, sizeof(*F));
    *Present = FALSE;
    if (!EXT3_HAS_COMPAT_FEATURE(sb, EXT4_FEATURE_COMPAT_ORPHAN_FILE) || ino == 0)
        return STATUS_SUCCESS;
    *Present = TRUE;

    F->Mcb = OrOpen(Vcb, ino);
    if (!F->Mcb)
        return STATUS_DISK_CORRUPT_ERROR;
    if ((ULONGLONG)F->Mcb->Inode->i_size >> BLOCK_BITS > 0x10000)
        return STATUS_DISK_CORRUPT_ERROR;
    F->Blocks = (ULONG)(F->Mcb->Inode->i_size >> BLOCK_BITS);
    F->PerBlock = (BLOCK_SIZE - ORPHAN_TAIL) / sizeof(__le32);
    if (F->Blocks == 0)
        return STATUS_SUCCESS;

    /* the inode's checksum seed (Linux: i_csum_seed) */
    id = cpu_to_le32(ino);
    F->CsumSeed = ext4_chksum(&Vcb->sbi, Vcb->sbi.s_csum_seed, (__u8 *)&id, sizeof(id));
    id = cpu_to_le32(F->Mcb->Inode->i_generation);
    F->CsumSeed = ext4_chksum(&Vcb->sbi, F->CsumSeed, (__u8 *)&id, sizeof(id));

    F->Where = Ext2AllocatePool(PagedPool, F->Blocks * sizeof(ULONGLONG), ORPHAN_TAG);
    if (!F->Where)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = Ext2BuildExtents(NULL, Vcb, F->Mcb, 0, F->Blocks << BLOCK_BITS, FALSE, &chain);
    for (e = chain; NT_SUCCESS(Status) && e; e = e->Next) {
        ULONG first = e->Offset >> BLOCK_BITS, count = e->Length >> BLOCK_BITS, i;
        if (first != next || e->Lba == 0) {
            Status = STATUS_DISK_CORRUPT_ERROR;     /* a hole in the file */
            break;
        }
        for (i = 0; i < count; i++)
            F->Where[first + i] = ((ULONGLONG)e->Lba >> BLOCK_BITS) + i;
        next += count;
    }
    if (chain)
        Ext2DestroyExtentChain(chain);
    if (NT_SUCCESS(Status) && next != F->Blocks)
        Status = STATUS_DISK_CORRUPT_ERROR;
    return Status;
}

/* O6: block Index read and checked */
static NTSTATUS
OrReadBlock(IN PEXT2_VCB Vcb, IN PORPHAN_FILE F, IN ULONG Index, OUT struct buffer_head **Bh)
{
    struct buffer_head *bh = sb_getblk(&Vcb->sb, (sector_t)F->Where[Index]);
    PUCHAR              data;

    *Bh = NULL;
    if (!bh)
        return STATUS_UNEXPECTED_IO_ERROR;
    data = (PUCHAR)bh->b_data;
    if (le32_to_cpu(*(__le32 *)(data + BLOCK_SIZE - ORPHAN_TAIL)) != ORPHAN_BLOCK_MAGIC ||
        (ext4_has_metadata_csum(&Vcb->sb) &&
         le32_to_cpu(*(__le32 *)(data + BLOCK_SIZE - 4)) != OrBlockSum(Vcb, F, F->Where[Index], data))) {
        brelse(bh);
        return STATUS_DISK_CORRUPT_ERROR;
    }
    *Bh = bh;
    return STATUS_SUCCESS;
}

/* every entry of the orphan file, finished when Finish; *Left: not cleared */
static NTSTATUS
OrWalkFile(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PORPHAN_FILE F,
           IN BOOLEAN Finish, IN OUT PULONG Finished, OUT PULONG Left)
{
    ULONG       b, i;
    NTSTATUS    Status = STATUS_SUCCESS;

    *Left = 0;
    for (b = 0; b < F->Blocks && NT_SUCCESS(Status); b++) {
        struct buffer_head *bh;

        Status = OrReadBlock(Vcb, F, b, &bh);
        if (!NT_SUCCESS(Status))
            break;
        for (i = 0; i < F->PerBlock; i++) {
            __le32     *entry = (__le32 *)bh->b_data + i;
            ULONG       ino = le32_to_cpu(*entry);
            PEXT2_MCB   Mcb;
            BOOLEAN     done;

            if (ino == 0)
                continue;
            if (!Finish || (Vcb->SuperBlock->s_state & cpu_to_le16(EXT4_ERROR_FS))) {
                (*Left)++;
                continue;
            }
            Mcb = OrGet(IrpContext, Vcb, ino, FALSE);
            if (!Mcb) {
                Ext2ReportCorruption(Vcb, ino, F->Where[b], -EFSCORRUPTED);
                (*Left)++;
                continue;
            }
            Ext2JournalEnterScope(Vcb);
            done = OrFinish(IrpContext, Vcb, Mcb, FALSE);
            if (done) {
                /* O3: the entry goes in the same transaction */
                lock_buffer(bh);
                *entry = 0;
                if (ext4_has_metadata_csum(&Vcb->sb))
                    *(__le32 *)((PUCHAR)bh->b_data + BLOCK_SIZE - 4) =
                        cpu_to_le32(OrBlockSum(Vcb, F, F->Where[b], (PUCHAR)bh->b_data));
                mark_buffer_dirty(bh);
                unlock_buffer(bh);
                (*Finished)++;
            } else {
                (*Left)++;
            }
            Ext2JournalLeaveScope(Vcb);
            Ext2FreeMcb(Vcb, Mcb);
        }
        brelse(bh);
    }
    return Status;
}

/*
 * O6 before anything is written: every block of the orphan file with its
 * magic and checksum (TRUE without an orphan file). Linux refuses to mount
 * a volume whose orphan file is not (ext4_init_orphan_info).
 */
BOOLEAN
Ext4OrphanFileSound(IN PEXT2_VCB Vcb)
{
    ORPHAN_FILE F;
    BOOLEAN     present;
    ULONG       b;
    NTSTATUS    Status = OrOpenFile(Vcb, &F, &present);

    for (b = 0; NT_SUCCESS(Status) && present && b < F.Blocks; b++) {
        struct buffer_head *bh;
        Status = OrReadBlock(Vcb, &F, b, &bh);
        if (NT_SUCCESS(Status))
            brelse(bh);
    }
    OrCloseFile(Vcb, &F);
    return NT_SUCCESS(Status);
}

/*
 * Orphans waiting: a classic list, or an orphan file not empty or not
 * sound. A volume switched to writable with any stays read-only (Linux
 * refuses that remount the same way).
 */
BOOLEAN
Ext4OrphansPending(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb)
{
    ORPHAN_FILE F;
    BOOLEAN     present;
    ULONG       left = 0, finished = 0;
    NTSTATUS    Status;

    if (Vcb->SuperBlock->s_last_orphan != 0)
        return TRUE;
    Status = OrOpenFile(Vcb, &F, &present);
    if (NT_SUCCESS(Status) && present)
        Status = OrWalkFile(IrpContext, Vcb, &F, FALSE, &finished, &left);
    OrCloseFile(Vcb, &F);
    return !NT_SUCCESS(Status) || left != 0;
}

/*
 * The writable mount's orphan cleanup, with the journal running. FALSE:
 * the orphan file is not sound and the volume has to stay read-only.
 */
BOOLEAN
Ext4CleanupOrphans(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb)
{
    PEXT2_SUPER_BLOCK   es = Vcb->SuperBlock;
    ORPHAN_FILE         F;
    BOOLEAN             present;
    ULONG               finished = 0, left = 0, guard = 0;
    NTSTATUS            Status;

    if (IsVcbReadOnly(Vcb))
        return TRUE;

    /* O5 */
    if (es->s_state & cpu_to_le16(EXT4_ERROR_FS)) {
        if (es->s_last_orphan != 0) {
            DbgPrint("ext4: errors on the volume, its orphan list is dropped for e2fsck\n");
            Ext2JournalEnterScope(Vcb);
            OrSetHead(IrpContext, Vcb, 0);
            Ext2JournalLeaveScope(Vcb);
        }
    }

    /* the classic list, from its head */
    while (es->s_last_orphan != 0 && !(es->s_state & cpu_to_le16(EXT4_ERROR_FS))) {
        ULONG       ino = le32_to_cpu(es->s_last_orphan);
        PEXT2_MCB   Mcb = OrGet(IrpContext, Vcb, ino, TRUE);
        BOOLEAN     done;

        Ext2JournalEnterScope(Vcb);
        if (!Mcb || ++guard > INODES_COUNT) {
            /* O4: the list is given up */
            Ext2ReportCorruption(Vcb, ino, 0, -EFSCORRUPTED);
            OrSetHead(IrpContext, Vcb, 0);
            Ext2JournalLeaveScope(Vcb);
            if (Mcb)
                Ext2FreeMcb(Vcb, Mcb);
            break;
        }
        done = OrFinish(IrpContext, Vcb, Mcb, TRUE);
        Ext2JournalLeaveScope(Vcb);
        Ext2FreeMcb(Vcb, Mcb);
        if (!done)
            break;          /* it is back at the head: e2fsck's */
        finished++;
    }

    /* the orphan file */
    Status = OrOpenFile(Vcb, &F, &present);
    if (NT_SUCCESS(Status) && present)
        Status = OrWalkFile(IrpContext, Vcb, &F, TRUE, &finished, &left);
    OrCloseFile(Vcb, &F);
    if (!NT_SUCCESS(Status)) {
        Ext2ReportCorruption(Vcb, le32_to_cpu(es->s_orphan_file_inum), 0, -EFSCORRUPTED);
        DbgPrint("ext4: the orphan file is not sound (%08x), the volume stays read-only\n", Status);
        return FALSE;
    }

    /* O6: an empty file needs no flag */
    if (left == 0 && EXT3_HAS_RO_COMPAT_FEATURE(&Vcb->sb, EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT)) {
        Ext2JournalEnterScope(Vcb);
        Ext2LockSuper(Vcb);
        ClearFlag(es->s_feature_ro_compat, EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT);
        (void)Ext2SaveSuper(IrpContext, Vcb);
        Ext2UnlockSuper(Vcb);
        Ext2JournalLeaveScope(Vcb);
    }
    if (finished || left)
        DbgPrint("ext4: orphans: %u finished, %u left for e2fsck\n", finished, left);
    return TRUE;
}
