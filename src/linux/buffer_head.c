/**
 * buffer_head.c - buffer heads: pinned cache views of metadata blocks.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/errno.h>
#include "linux_internal.h"

/* kernel timer routines */

/* buffer head routines */

struct _EXT2_BUFFER_HEAD {
    kmem_cache_t *  bh_cache;
    atomic_t        bh_count;
    atomic_t        bh_acount;
} g_jbh = {NULL, ATOMIC_INIT(0)};

int
ext2_init_bh()
{
    g_jbh.bh_count.counter = 0;
    g_jbh.bh_acount.counter = 0;
    g_jbh.bh_cache = kmem_cache_create(
                         "ext2_bh",   /* bh */
                         sizeof(struct buffer_head),
                         0,		        /* offset */
                         SLAB_TEMPORARY,	/* flags */
                         NULL);		    /* ctor */
    if (g_jbh.bh_cache == NULL) {
        printk(KERN_EMERG "JBD: failed to create handle cache\n");
        return -ENOMEM;
    }
    return 0;
}

void
ext2_destroy_bh()
{
    if (g_jbh.bh_cache) {
        kmem_cache_destroy(g_jbh.bh_cache);
        g_jbh.bh_cache = NULL;
    }
}

struct buffer_head *
new_buffer_head()
{
    struct buffer_head * bh = NULL;
    bh = kmem_cache_alloc(g_jbh.bh_cache, GFP_NOFS);
    if (bh) {
        atomic_inc(&g_jbh.bh_count);
        atomic_inc(&g_jbh.bh_acount);

        memset(bh, 0, sizeof(struct buffer_head));
        InitializeListHead(&bh->b_link);
        KeQuerySystemTime(&bh->b_ts_creat);
        DEBUG(DL_BH, ("bh=%p allocated.\n", bh));
        INC_MEM_COUNT(PS_BUFF_HEAD, bh, sizeof(struct buffer_head));
    }

    return bh;
}

void
free_buffer_head(struct buffer_head * bh)
{
    if (bh) {
        if (bh->b_mdl) {

            DEBUG(DL_BH, ("bh=%p mdl=%p (Flags:%xh VA:%p) released.\n", bh, bh->b_mdl,
                          bh->b_mdl->MdlFlags, bh->b_mdl->MappedSystemVa));
            if (IsFlagOn(bh->b_mdl->MdlFlags, MDL_MAPPED_TO_SYSTEM_VA)) {
                MmUnmapLockedPages(bh->b_mdl->MappedSystemVa, bh->b_mdl);
            }
            Ext2DestroyMdl(bh->b_mdl);
        }
        if (bh->b_bcb) {
            CcUnpinDataForThread(bh->b_bcb, (ERESOURCE_THREAD)bh | 0x3);
        }

        DEBUG(DL_BH, ("bh=%p freed.\n", bh));
        ASSERT(bh->b_jrefs == 0 && bh->b_jrec == NULL);
        DEC_MEM_COUNT(PS_BUFF_HEAD, bh, sizeof(struct buffer_head));
        kmem_cache_free(g_jbh.bh_cache, bh);
        atomic_dec(&g_jbh.bh_count);
    }
}

/* Red-black tree insert routine. */

static struct buffer_head *__buffer_head_search(struct rb_root *root,
                       sector_t blocknr)
{
    struct rb_node *new = root->rb_node;

    /* Figure out where to put new node */
    while (new) {
        struct buffer_head *bh =
            container_of(new, struct buffer_head, b_rb_node);
        if (blocknr < bh->b_blocknr)
            new = new->rb_left;
        else if (blocknr > bh->b_blocknr)
            new = new->rb_right;
        else
            return bh;

    }

    return NULL;
}

static int buffer_head_blocknr_cmp(struct rb_node *a, struct rb_node *b)
{
    struct buffer_head *a_bh, *b_bh;
    a_bh = container_of(a, struct buffer_head, b_rb_node);
    b_bh = container_of(b, struct buffer_head, b_rb_node);
    if (a_bh->b_blocknr < b_bh->b_blocknr)
        return -1;
    if (a_bh->b_blocknr > b_bh->b_blocknr)
        return 1;
    return 0;
}

static struct buffer_head *buffer_head_search(struct block_device *bdev,
                     sector_t blocknr)
{
    struct rb_root *root;
    root = &bdev->bd_bh_root;
    return __buffer_head_search(root, blocknr);
}

static void buffer_head_insert(struct block_device *bdev, struct buffer_head *bh)
{
    rb_insert(&bdev->bd_bh_root, &bh->b_rb_node, buffer_head_blocknr_cmp);
}

void buffer_head_remove(struct block_device *bdev, struct buffer_head *bh)
{
    rb_erase(&bh->b_rb_node, &bdev->bd_bh_root);
}

struct buffer_head *
get_block_bh_pin(
    struct block_device *   bdev,
    sector_t                block,
    unsigned long           size,
    int                     zero
) 
{
    PEXT2_VCB Vcb = bdev->bd_priv;
    LARGE_INTEGER offset;


    /* allocate buffer_head and initialize it */
    struct buffer_head *bh = NULL, *tbh = NULL;

    /* check the block is valid or not */
    if (block >= TOTAL_BLOCKS) {
        goto errorout;
    }

    /* search the bdev bh list */
    ExAcquireSharedStarveExclusive(&bdev->bd_bh_lock, TRUE);
    tbh = buffer_head_search(bdev, block);
    if (tbh) {
        bh = tbh;
        get_bh(bh);
        ExReleaseResourceLite(&bdev->bd_bh_lock);
        goto errorout;
    }
    ExReleaseResourceLite(&bdev->bd_bh_lock);

    bh = new_buffer_head();
    if (!bh) {
        goto errorout;
    }
    bh->b_bdev = bdev;
    bh->b_blocknr = block;
    bh->b_size = size;
    bh->b_data = NULL;

    offset.QuadPart = (s64) bh->b_blocknr;
    offset.QuadPart <<= BLOCK_BITS;

    /* Always a read pin, also for a block about to be overwritten in
       full: CcPreparePinWrite marks the page dirty and the lazy writer
       then needs the Bcb exclusively, which it can only get once every
       pin in that page is gone - and the group descriptors stay pinned
       for the life of the mount (on a 1 KB volume they share page 0 with
       the superblock). Metadata reaches disk through the journal or
       submit_bh() only, never through the cache manager (invariant I1),
       so the page must never be dirty in its eyes. */
    /* With PIN_WAIT the cache manager pins the block or raises (read
       error, device gone); a FALSE return cannot happen. An expected error
       costs this bh and the caller gets NULL, which it handles anyway. */
    __try {
        if (!CcPinRead(Vcb->Volume, &offset, (ULONG)bh->b_size, PIN_WAIT,
                       &bh->b_bcb, &bh->b_data)) {
            bh->b_bcb = NULL;
            bh->b_data = NULL;
        }
    } __except (FsRtlIsNtstatusExpected(GetExceptionCode()) ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        DEBUG(DL_ERR, ("getblk: block %I64u cannot be read (%xh)\n",
                       (ULONGLONG)block, GetExceptionCode()));
        bh->b_bcb = NULL;
        bh->b_data = NULL;
    }

    if (bh->b_bcb)
        CcSetBcbOwnerPointer(bh->b_bcb, (PVOID)((ERESOURCE_THREAD)bh | 0x3));

    if (!bh->b_data) {
        free_buffer_head(bh);
        bh = NULL;
        goto errorout;
    }
    if (zero) {
        RtlZeroMemory(bh->b_data, bh->b_size);
    }
    set_buffer_uptodate(bh);
    get_bh(bh);

    DEBUG(DL_BH, ("getblk: Vcb=%p bhcount=%u block=%u bh=%p ptr=%p.\n",
                  Vcb, atomic_read(&g_jbh.bh_count), block, bh, bh->b_data));

    ExAcquireResourceExclusiveLite(&bdev->bd_bh_lock, TRUE);
    /* do search again here */
    tbh = buffer_head_search(bdev, block);
    if (tbh) {
        get_bh(tbh);
        free_buffer_head(bh);
        bh = tbh;
        RemoveEntryList(&bh->b_link);
        InitializeListHead(&bh->b_link);
        ExReleaseResourceLite(&bdev->bd_bh_lock);
        goto errorout;
    } else {
        buffer_head_insert(bdev, bh);
    }
    ExReleaseResourceLite(&bdev->bd_bh_lock);

    /* bh is referenced for the caller, or NULL */
errorout:

    return bh;
}

int submit_bh_pin(int rw, struct buffer_head *bh)
{
    struct block_device *bdev = bh->b_bdev;
    PEXT2_VCB            Vcb  = bdev->bd_priv;
    PVOID                Buffer;
    LARGE_INTEGER        Offset;
    int                  rc = 0;

    ASSERT(Vcb->Identifier.Type == EXT2VCB);
    ASSERT(bh->b_data && bh->b_bcb);

    if (rw == WRITE) {

        NTSTATUS Status;

        if (IsVcbReadOnly(Vcb)) {
            goto errorout;
        }

        /* Not journaled (ext2, or the engine is not running): write the
           block home right now, from a copy of the pinned view, and leave
           the cache page clean. Marking it dirty instead would make the
           lazy writer wait for exclusive access to the Bcb, i.e. for
           every pin in that page to go away - forever, if the page also
           holds a long-lived pin such as the group descriptor cache. */
        SetFlag(Vcb->Volume->Flags, FO_FILE_MODIFIED);
        Offset.QuadPart = ((LONGLONG)bh->b_blocknr) << BLOCK_BITS;

        Buffer = Ext2AllocatePool(NonPagedPool, bh->b_size, EXT2_DATA_MAGIC);
        if (Buffer) {
            RtlCopyMemory(Buffer, bh->b_data, bh->b_size);
            Status = Ext2WriteDiskSync(Vcb, Offset.QuadPart, (ULONG)bh->b_size,
                                       Buffer, FALSE);
            Ext2FreePool(Buffer, EXT2_DATA_MAGIC);
        } else {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }

        if (!NT_SUCCESS(Status)) {
            DbgPrint("ext4: write of block %I64u failed (%xh)\n",
                     (ULONGLONG)bh->b_blocknr, Status);
            rc = -EIO;
        }
    }

errorout:

    unlock_buffer(bh);
    put_bh(bh);
    return rc;
}

struct buffer_head *
get_block_bh(
    struct block_device *   bdev,
    sector_t                block,
    unsigned long           size,
    int                     zero
) 
{
    return get_block_bh_pin(bdev, block, size, zero);
}

int submit_bh(int rw, struct buffer_head *bh)
{
    return submit_bh_pin(rw, bh);
}

struct buffer_head *
__getblk(
    struct block_device *   bdev,
    sector_t                block,
    unsigned long           size
)
{
    return get_block_bh(bdev, block, size, 0);
}

/*
 * A buffer that is no block of the volume: a view built in memory, such as
 * the block an inline directory would be. Read only - never dirtied, never
 * written, in no lookup tree - and freed with its last reference.
 */
struct buffer_head *alloc_virtual_bh(struct block_device *bdev, size_t size)
{
    struct buffer_head *bh = new_buffer_head();

    if (!bh) {
        return NULL;
    }
    bh->b_bdev = bdev;
    bh->b_size = size;
    bh->b_blocknr = (sector_t)-1;
    bh->b_data = Ext2AllocatePool(NonPagedPool, size, EXT2_DATA_MAGIC);
    if (!bh->b_data) {
        free_buffer_head(bh);
        return NULL;
    }
    RtlZeroMemory(bh->b_data, size);
    set_buffer_virtual(bh);
    set_buffer_uptodate(bh);
    get_bh(bh);
    return bh;
}

void __brelse(struct buffer_head *bh)
{
    struct block_device *bdev = bh->b_bdev;
    PEXT2_VCB Vcb = (PEXT2_VCB)bdev->bd_priv;
    BOOLEAN JournalHeld;

    ASSERT(Vcb->Identifier.Type == EXT2VCB);

    if (buffer_virtual(bh)) {
        if (atomic_dec_and_test(&bh->b_count)) {
            Ext2FreePool(bh->b_data, EXT2_DATA_MAGIC);
            bh->b_data = NULL;
            free_buffer_head(bh);
        }
        return;
    }

    /* write data in case it's dirty */
    while (buffer_dirty(bh)) {
        ll_rw_block(WRITE, 1, &bh);
    }

    /* Snapshot the notification hint while our reference keeps bh alive.
       After releasing it, another thread may drop the last reference. */
    JournalHeld = (bh->b_jrefs != 0);

    /* Fast path: a reference that is not the last one changes nothing but
       the count. Group descriptors and bitmaps stay referenced for long
       stretches, so most releases end here. */
    if (atomic_add_unless(&bh->b_count, -1, 1)) {
        /* the journal's records keep their own references: a buffer it
           holds only ever gets here, and a checkpoint may wait for it */
        if (JournalHeld) {
            Ext2JournalBufferReleased(Vcb);
        }
        return;
    }

    /* The 1 -> 0 step puts the bh at the young end of the free list. The
       tree lock is held shared - only the reaper, the teardown and a new
       bh entering the tree take it exclusively, and those take no bh off
       the tree while a release is here - and the list itself is guarded
       by its spin lock. A lookup may revive the bh meanwhile: the reaper
       finds it referenced and takes it off the list, as it always has.
       Exclusive here, every last release - one per block touched by an
       operation - queued all threads of the volume behind each other. */
    ExAcquireResourceSharedLite(&bdev->bd_bh_lock, TRUE);
    if (atomic_dec_and_test(&bh->b_count)) {
        KIRQL Irql;

        KeQuerySystemTime(&bh->b_ts_drop);
        KeAcquireSpinLock(&bdev->bd_bh_free_lock, &Irql);
        RemoveEntryList(&bh->b_link);
        InsertTailList(&Vcb->bd.bd_bh_free, &bh->b_link);
        KeReleaseSpinLock(&bdev->bd_bh_free_lock, Irql);
        KeClearEvent(&Vcb->bd.bd_bh_notify);
        DEBUG(DL_BH, ("brelse: cnt=%u size=%u blk=%10.10xh bh=%p ptr=%p\n",
                      atomic_read(&g_jbh.bh_count) - 1, bh->b_size,
                      bh->b_blocknr, bh, bh->b_data ));
        ExReleaseResourceLite(&bdev->bd_bh_lock);
        Ext2ReaperKick(&Ext2Global->bhReaper, FALSE);
    } else {
        ExReleaseResourceLite(&bdev->bd_bh_lock);
        return;
    }
}

void __bforget(struct buffer_head *bh)
{
    clear_buffer_dirty(bh);
    __brelse(bh);
}

void __lock_buffer(struct buffer_head *bh)
{
    UNREFERENCED_PARAMETER(bh);
}

void unlock_buffer(struct buffer_head *bh)
{
    clear_buffer_locked(bh);
}

void __wait_on_buffer(struct buffer_head *bh)
{
    UNREFERENCED_PARAMETER(bh);
}

void ll_rw_block(int rw, int nr, struct buffer_head * bhs[])
{
    int i;

    for (i = 0; i < nr; i++) {

        struct buffer_head *bh = bhs[i];

        if (rw == SWRITE)
            lock_buffer(bh);
        else if (test_set_buffer_locked(bh))
            continue;

        if (rw == WRITE || rw == SWRITE) {
            if (test_clear_buffer_dirty(bh)) {
                get_bh(bh);
                submit_bh(WRITE, bh);
                continue;
            }
        } else {
            if (!buffer_uptodate(bh)) {
                get_bh(bh);
                submit_bh(rw, bh);
                continue;
            }
        }
        unlock_buffer(bh);
    }
}

int bh_submit_read(struct buffer_head *bh)
{
	ll_rw_block(READ, 1, &bh);
    return 0;
}

int sync_dirty_buffer(struct buffer_head *bh)
{
    int ret = 0;

    ASSERT(atomic_read(&bh->b_count) <= 1);
    lock_buffer(bh);
    if (test_clear_buffer_dirty(bh)) {
        get_bh(bh);
        ret = submit_bh(WRITE, bh);
        wait_on_buffer(bh);
    } else {
        unlock_buffer(bh);
    }
    return ret;
}

void mark_buffer_dirty(struct buffer_head *bh)
{
    PEXT2_VCB Vcb = (PEXT2_VCB)bh->b_bdev->bd_priv;

    /* a view of an inline directory has no block to go to: whoever
       changes a directory converts it to blocks first (InlineData.c) */
    if (buffer_virtual(bh)) {
        DbgPrint("ext4: a change to an inline directory view was dropped\n");
        ASSERT(FALSE);
        return;
    }

    /* with the journal engine active the block is filed into the running
       transaction; it reaches disk through commit + checkpoint only */
    if (Vcb->Journal && Ext2JournalDirtyBuffer(Vcb, bh))
        return;

    set_buffer_dirty(bh);
}

int sync_blockdev(struct block_device *bdev)
{
    PEXT2_VCB Vcb = (PEXT2_VCB) bdev->bd_priv;
    Ext2FlushVolume(NULL, Vcb, FALSE);
    return 0;
}

/*
 * Perform a pagecache lookup for the matching buffer.  If it's there, refre
 * it in the LRU and mark it as accessed.  If it is not present then return
 * NULL
 */
struct buffer_head *
__find_get_block(struct block_device *bdev, sector_t block, unsigned long size)
{
    return __getblk(bdev, block, size);
}

/* inode block mapping */

ULONGLONG bmap(struct inode *i, ULONGLONG b)
{
    ULONGLONG lcn = 0;
    struct super_block *s = i->i_sb;

    PEXT2_MCB  Mcb = (PEXT2_MCB)i->i_priv;
    PEXT2_VCB  Vcb = (PEXT2_VCB)s->s_priv;
    PEXT2_EXTENT extent = NULL;
    ULONGLONG  offset = (ULONGLONG)b;
    NTSTATUS   status;

    if (!Mcb || !Vcb) {
        goto errorout;
    }

    offset <<= BLOCK_BITS;
    status = Ext2BuildExtents(
                 NULL,
                 Vcb,
                 Mcb,
                 offset,
                 BLOCK_SIZE,
                 FALSE,
                 &extent
             );

    if (!NT_SUCCESS(status)) {
        goto errorout;
    }

    if (extent == NULL) {
        goto errorout;
    }

    lcn = (unsigned long)(extent->Lba >> BLOCK_BITS);

errorout:

    if (extent) {
        Ext2FreeExtent(extent);
    }

    return lcn;
}
