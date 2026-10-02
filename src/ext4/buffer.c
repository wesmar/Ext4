/**
 * buffer.c - metadata block access through buffer heads and the volume stream.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "linux\ext4.h"

VOID
Ext2DropBH(IN PEXT2_VCB Vcb)
{

    /* nothing to drop before the buffer head cache exists (a mount that
       failed early), or after it is gone */
    if (Vcb->bd.bd_bh_cache == NULL)
        return;

    __try {

        /* acquire bd lock to avoid bh creation */
        ExAcquireResourceExclusiveLite(&Vcb->bd.bd_bh_lock, TRUE);

        /* VCB_BEING_DROPPED is not touched here: it marks a Vcb whose
           destruction has been claimed (Ext2CheckDismount), for good */
        Ext2DropGroupBH(Vcb);

        while (!IsListEmpty(&Vcb->bd.bd_bh_free)) {
            struct buffer_head *bh;
            PLIST_ENTRY         l;
            l = RemoveHeadList(&Vcb->bd.bd_bh_free);
            bh = CONTAINING_RECORD(l, struct buffer_head, b_link);
            InitializeListHead(&bh->b_link);
            if (0 == atomic_read(&bh->b_count)) {
                buffer_head_remove(&Vcb->bd, bh);
                free_buffer_head(bh);
            }
        }

    } __finally {
        ExReleaseResourceLite(&Vcb->bd.bd_bh_lock);
    }
}

/*
 * Leave no buffer head - and so no pinned Bcb - on the volume stream.
 * CcUninitializeCacheMap bugchecks (0x34, CcDeleteBcbs) on a stream with
 * a pin outstanding, and every bh pins its block from the moment
 * get_block_bh_pin creates it until free_buffer_head: that includes bhs
 * released with brelse that sit on the free list, and bhs the reaper has
 * already taken off the tree but not freed yet.
 *
 * Called from Ext2TearDownStream, when the stream is the last file object
 * on the volume and the journal session has ended - the only holders that
 * can remain are driver threads finishing up, so a bh still referenced
 * after a bounded wait is a leak: it is freed anyway, and reported, since
 * keeping it would only turn the leak into a bugcheck.
 */
VOID
Ext2DrainBH(IN PEXT2_VCB Vcb)
{
    LARGE_INTEGER   tick;
    ULONG           waited = 0;

    /* also on a mount that failed half way: the blocks it read are pinned
       in the stream all the same */
    if (Vcb->bd.bd_bh_cache == NULL)
        return;

    tick.QuadPart = -(LONGLONG)10 * 1000 * 10;      /* 10 ms */

    for (;;) {

        BOOLEAN busy;

        Ext2DropBH(Vcb);

        ExAcquireResourceExclusiveLite(&Vcb->bd.bd_bh_lock, TRUE);
        busy = (Vcb->bd.bd_bh_reaping != 0) ||
               (rb_first(&Vcb->bd.bd_bh_root) != NULL);
        ExReleaseResourceLite(&Vcb->bd.bd_bh_lock);

        if (!busy)
            return;

        if (waited++ >= 500)                         /* 5 s */
            break;

        KeWaitForSingleObject(&Vcb->bd.bd_bh_notify, Executive,
                              KernelMode, FALSE, &tick);
    }

    ExAcquireResourceExclusiveLite(&Vcb->bd.bd_bh_lock, TRUE);

    while (Vcb->bd.bd_bh_reaping != 0) {
        /* the reaper holds the lock only briefly: let it finish */
        ExReleaseResourceLite(&Vcb->bd.bd_bh_lock);
        KeWaitForSingleObject(&Vcb->bd.bd_bh_notify, Executive,
                              KernelMode, FALSE, &tick);
        ExAcquireResourceExclusiveLite(&Vcb->bd.bd_bh_lock, TRUE);
    }

    while (rb_first(&Vcb->bd.bd_bh_root) != NULL) {
        struct buffer_head *bh = container_of(rb_first(&Vcb->bd.bd_bh_root),
                                              struct buffer_head, b_rb_node);
        DbgPrint("ext4: bh %p block %I64u count %d jrefs %d leaked, freed at teardown\n",
                 bh, (ULONGLONG)bh->b_blocknr, atomic_read(&bh->b_count),
                 bh->b_jrefs);
        if (!IsListEmpty(&bh->b_link)) {
            RemoveEntryList(&bh->b_link);
        }
        buffer_head_remove(&Vcb->bd, bh);
        free_buffer_head(bh);
    }

    ExReleaseResourceLite(&Vcb->bd.bd_bh_lock);
}

NTSTATUS
Ext2FlushRange(IN PEXT2_VCB Vcb, LARGE_INTEGER s, LARGE_INTEGER e)
{
    ULONG len;
    IO_STATUS_BLOCK IoStatus;

    if (e.QuadPart <= s.QuadPart)
        return STATUS_SUCCESS;

    /* loop per 2G */
    while (s.QuadPart < e.QuadPart) {
        if (e.QuadPart > s.QuadPart + 1024 * 1024 * 1024) {
            len = 1024 * 1024 * 1024;
        } else {
            len = (ULONG) (e.QuadPart - s.QuadPart);
        }
        CcFlushCache(&Vcb->SectionObject, &s, len, &IoStatus);
        if (!NT_SUCCESS(IoStatus.Status))
            return IoStatus.Status;
        s.QuadPart += len;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
Ext2FlushVcb(IN PEXT2_VCB Vcb)
{
    NTSTATUS Status = STATUS_SUCCESS;
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER        s = {0}, o;
    struct rb_node      *node;
    struct buffer_head  *bh;

    /* the free totals live in the Vcb: into the superblock first (on a
       journaled volume Ext2JournalFlush does it) */
    if (!Vcb->Journal) {
        Ext2SyncSuperTotals(NULL, Vcb);
    }

    /* journaled metadata reaches disk only through commit + checkpoint;
       the cache flush below never sees it (invariant I1) */
    if (Vcb->Journal) {
        Status = Ext2JournalFlush(Vcb);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    if (!IsFlagOn(Vcb->Flags, VCB_GD_LOADED)) {
        CcFlushCache(&Vcb->SectionObject, NULL, 0, &IoStatus);
        Status = IoStatus.Status;
        goto errorout;
    }

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Vcb->MainResource));

    __try {

        /* acquire gd block */
        ExAcquireResourceExclusiveLite(&Vcb->sbi.s_gd_lock, TRUE);

        /* acquire bd lock to avoid bh creation */
        ExAcquireResourceExclusiveLite(&Vcb->bd.bd_bh_lock, TRUE);

        /* drop unused bh */
        Ext2DropBH(Vcb);

        /* flush volume with all outstanding bh skipped */

        node = rb_first(&Vcb->bd.bd_bh_root);
        while (node) {

            bh = container_of(node, struct buffer_head, b_rb_node);
            node = rb_next(node);

            o.QuadPart = bh->b_blocknr << BLOCK_BITS;
            ASSERT(o.QuadPart >= s.QuadPart);

            if (o.QuadPart == s.QuadPart) {
                s.QuadPart = s.QuadPart + bh->b_size;
                continue;
            }

            if (o.QuadPart > s.QuadPart) {
                Status = Ext2FlushRange(Vcb, s, o);
                if (!NT_SUCCESS(Status))
                    __leave;
                s.QuadPart = (bh->b_blocknr << BLOCK_BITS) + bh->b_size;
                continue;
            }
        }

        o = Vcb->PartitionInformation.PartitionLength;
        Status = Ext2FlushRange(Vcb, s, o);

    } __finally {

        ExReleaseResourceLite(&Vcb->bd.bd_bh_lock);
        ExReleaseResourceLite(&Vcb->sbi.s_gd_lock);
    }

errorout:
    return Status;
}

BOOLEAN
Ext2LoadBlock (IN PEXT2_VCB Vcb,
               IN ULONG     Index,
               IN PVOID     Buffer )
{
    struct buffer_head *bh = NULL;
    BOOLEAN             rc = 0;

    __try {

        bh = sb_getblk(&Vcb->sb, (sector_t)Index);

        if (!bh) {
            DEBUG(DL_ERR, ("Ext2Loadblock: can't load block %u\n", Index));
            __leave;
        }

        if (!buffer_uptodate(bh)) {
            int err = bh_submit_read(bh);
	        if (err < 0) {
	            DEBUG(DL_ERR, ("Ext2LoadBlock: reading failed %d\n", err));
		        __leave;
	        }
        }

        RtlCopyMemory(Buffer, bh->b_data, BLOCK_SIZE);
        rc = TRUE;

    } __finally {

        if (bh)
            fini_bh(&bh);
    }

    return rc; 
}

BOOLEAN
Ext2SaveBlock ( IN PEXT2_IRP_CONTEXT    IrpContext,
                IN PEXT2_VCB            Vcb,
                IN ULONG                Index,
                IN PVOID                Buf )
{
    UNREFERENCED_PARAMETER(IrpContext);
    struct buffer_head *bh = NULL;
    BOOLEAN             rc = 0;

    __try {

        bh = sb_getblk_zero(&Vcb->sb, (sector_t)Index);

        if (!bh) {
            DEBUG(DL_ERR, ("Ext2Saveblock: can't load block %u\n", Index));
            __leave;
        }

        RtlCopyMemory(bh->b_data, Buf, BLOCK_SIZE);
        mark_buffer_dirty(bh);
        rc = TRUE;

    } __finally {

        if (bh)
            fini_bh(&bh);
    }

    return rc;
}

BOOLEAN
Ext2LoadBuffer( IN PEXT2_IRP_CONTEXT    IrpContext,
                IN PEXT2_VCB            Vcb,
                IN LONGLONG             offset,
                IN ULONG                size,
                IN PVOID                buf )
{
    UNREFERENCED_PARAMETER(IrpContext);
    struct buffer_head *bh = NULL;
    BOOLEAN             rc = FALSE;

    __try {

        while (size) {

            sector_t    block;
            ULONG       len = 0, delta = 0;

            block = (sector_t) (offset >> BLOCK_BITS);
            delta = (ULONG)offset & (BLOCK_SIZE - 1);
            len = BLOCK_SIZE - delta;
            if (size < len)
                len = size;

            bh = sb_getblk(&Vcb->sb, block);
            if (!bh) {
                DEBUG(DL_ERR, ("Ext2SaveBuffer: can't load block %I64u\n", block));
                __leave;
            }

            if (!buffer_uptodate(bh)) {
	            int err = bh_submit_read(bh);
	            if (err < 0) {
		            DEBUG(DL_ERR, ("Ext2SaveBuffer: bh_submit_read failed: %d\n", err));
		            __leave;
	            }
            }

            __try {
                RtlCopyMemory(buf, bh->b_data + delta, len);
            } __finally {
                fini_bh(&bh);
            }

            buf = (PUCHAR)buf + len;
            offset = offset + len;
            size = size - len;
        }

        rc = TRUE;

    } __finally {

        if (bh)
            fini_bh(&bh);

    }

    return rc;
}

BOOLEAN
Ext2ZeroBuffer( IN PEXT2_IRP_CONTEXT    IrpContext,
                IN PEXT2_VCB            Vcb,
                IN LONGLONG             offset,
                IN ULONG                size
    )
{
    UNREFERENCED_PARAMETER(IrpContext);
    struct buffer_head *bh = NULL;
    BOOLEAN             rc = 0;

    __try {

        while (size) {

            sector_t    block;
            ULONG       len = 0, delta = 0;

            block = (sector_t) (offset >> BLOCK_BITS);
            delta = (ULONG)offset & (BLOCK_SIZE - 1);
            len = BLOCK_SIZE - delta;
            if (size < len)
                len = size;

            if (delta == 0 && len >= BLOCK_SIZE) {
                bh = sb_getblk_zero(&Vcb->sb, block);
            } else {
                bh = sb_getblk(&Vcb->sb, block);
            }

            if (!bh) {
                DEBUG(DL_ERR, ("Ext2SaveBuffer: can't load block %I64u\n", block));
                __leave;
            }

            if (!buffer_uptodate(bh)) {
	            int err = bh_submit_read(bh);
	            if (err < 0) {
		            DEBUG(DL_ERR, ("Ext2SaveBuffer: bh_submit_read failed: %d\n", err));
		            __leave;
	            }
            }

            __try {
                if (delta == 0 && len >= BLOCK_SIZE) {
                    /* bh (cache) was already cleaned as zero */
                } else {
                    RtlZeroMemory(bh->b_data + delta, len);
                }
                mark_buffer_dirty(bh);
            } __finally {
                fini_bh(&bh);
            }

            offset = offset + len;
            size = size - len;
        }

        rc = TRUE;

    } __finally {

        if (bh)
            fini_bh(&bh);

    }

    return rc;
}

BOOLEAN
Ext2SaveBuffer( IN PEXT2_IRP_CONTEXT    IrpContext,
                IN PEXT2_VCB            Vcb,
                IN LONGLONG             offset,
                IN ULONG                size,
                IN PVOID                buf )
{
    UNREFERENCED_PARAMETER(IrpContext);
    struct buffer_head *bh = NULL;
    BOOLEAN             rc = 0;

    __try {

        while (size) {

            sector_t    block;
            ULONG       len = 0, delta = 0;

            block = (sector_t) (offset >> BLOCK_BITS);
            delta = (ULONG)offset & (BLOCK_SIZE - 1);
            len = BLOCK_SIZE - delta;
            if (size < len)
                len = size;

            if (delta == 0 && len >= BLOCK_SIZE) {
                bh = sb_getblk_zero(&Vcb->sb, block);
            } else {
                bh = sb_getblk(&Vcb->sb, block);
            }

            if (!bh) {
                DEBUG(DL_ERR, ("Ext2SaveBuffer: can't load block %I64u\n", block));
                __leave;
            }

            if (!buffer_uptodate(bh)) {
	            int err = bh_submit_read(bh);
	            if (err < 0) {
		            DEBUG(DL_ERR, ("Ext2SaveBuffer: bh_submit_read failed: %d\n", err));
		            __leave;
	            }
            }

            __try {
                RtlCopyMemory(bh->b_data + delta, buf, len);
                mark_buffer_dirty(bh);
            } __finally {
                fini_bh(&bh);
            }

            buf = (PUCHAR)buf + len;
            offset = offset + len;
            size = size - len;
        }

        rc = TRUE;

    } __finally {

        if (bh)
            fini_bh(&bh);

    }

    return rc;
}
