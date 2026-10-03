/**
 * JournalScope.c - per-thread journal handles: request scopes and buffer registration.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/jbd2.h>
#include "JournalInternal.h"

LIST_ENTRY               JnlScopeBuckets[JNL_SCOPE_BUCKETS];
KSPIN_LOCK               JnlScopeLock;
NPAGED_LOOKASIDE_LIST    JnlScopeLookaside;
BOOLEAN                  JnlScopeInited = FALSE;

static __inline ULONG JnlScopeHash(PKTHREAD Thread)
{
    return (ULONG)(((ULONG_PTR)Thread >> 4) & (JNL_SCOPE_BUCKETS - 1));
}

/* SCOPES (per-thread handles) *******************************************/

static PEXT2_JSCOPE JnlFindScope(PKTHREAD Thread)
{
    PLIST_ENTRY head = &JnlScopeBuckets[JnlScopeHash(Thread)];
    PLIST_ENTRY e;

    for (e = head->Flink; e != head; e = e->Flink) {
        PEXT2_JSCOPE s = CONTAINING_RECORD(e, EXT2_JSCOPE, Link);
        if (s->Thread == Thread)
            return s;
    }
    return NULL;
}

/*
 * Find (or create) the current thread's scope and its slot for J.
 * Returns NULL when the thread has no scope (a caller outside of IRP
 * dispatch: reported in checked builds so it can be given one).
 */
static PEXT2_JSCOPE JnlCurrentScope(PEXT2_JOURNAL J, PULONG Slot)
{
    PKTHREAD        Thread = KeGetCurrentThread();
    PEXT2_JSCOPE    s;
    KIRQL           irql;
    ULONG           i;

    KeAcquireSpinLock(&JnlScopeLock, &irql);
    s = JnlFindScope(Thread);
    if (s) {
        for (i = 0; i < s->Count; i++) {
            if (s->Journal[i] == J)
                break;
        }
        if (i == s->Count) {
            if (s->Count < JNL_SCOPE_JOURNALS) {
                s->Journal[s->Count] = J;
                s->Txn[s->Count] = NULL;
                s->Count++;
            } else {
                s = NULL;
            }
        }
        *Slot = i;
    }
    KeReleaseSpinLock(&JnlScopeLock, irql);
    return s;
}

/*
 * Make the current thread's handle on J active: count it in the running
 * transaction so the commit waits for this IRP to finish. Joining a
 * locked-but-unsealed transaction is fine; a sealed one means the copy is
 * in progress, so wait for the next transaction (a bounded wait).
 * Returns the transaction joined, or NULL when the engine is not accepting
 * (aborted / stopping), in which case the caller files nothing.
 */
static PEXT2_JTXN JnlActivate(PEXT2_JOURNAL J)
{
    PEXT2_JSCOPE    s;
    ULONG           slot = 0;
    KIRQL           irql;
    LARGE_INTEGER   tick;
    PEXT2_JTXN      t;

    s = JnlCurrentScope(J, &slot);

    if (s && s->Txn[slot])
        return s->Txn[slot];        /* already active */

    if (!s && J->NoScopeReports < 8) {
        PIRP top = IoGetTopLevelIrp();

        J->NoScopeReports++;
        if (top && top > (PIRP)FSRTL_MAX_TOP_LEVEL_IRP_FLAG) {
            PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(top);
            DbgPrint("ext4: metadata modified outside of a request scope "
                     "(thread %p, top-level IRP mj %u mn %u flags %xh fo %p)\n",
                     KeGetCurrentThread(), sp->MajorFunction, sp->MinorFunction,
                     top->Flags, sp->FileObject);
        } else {
            DbgPrint("ext4: metadata modified outside of a request scope "
                     "(thread %p, top-level %p)\n", KeGetCurrentThread(), top);
        }
    }

    tick.QuadPart = -JNL_WAIT_TICK;

    for (;;) {
        BOOLEAN full;

        JnlLock(J, irql);
        if (!(J->Flags & JF_ACTIVE) || (J->Flags & JF_ABORTED)) {
            JnlUnlock(J, irql);
            return NULL;
        }
        t = J->Running;

        /* A handle may join only while the transaction, with one more
           block, still fits in the free log - so by induction a running
           transaction can always be committed - and only while it is
           below twice its nominal size. Otherwise whoever is already in
           it finishes, the commit takes it away, the checkpoint frees the
           log, and this handle joins the next one (Linux waits the same
           way in start_this_handle / jbd2_log_wait_for_space). Without
           this a fast writer grew the running transaction past what the
           log could hold, and the commit aborted the journal. */
        full = (t->State == JTXN_RUNNING) &&
               (t->NumRecords >= J->MaxTxnBlocks * 2 ||
                JnlLogBlocksNeeded(J, t->NumNormal + 1, t->NumRevokes) + JNL_LOG_SLACK
                    > J->jbd->j_free);
        if (full && !tid_geq(J->CommitRequest, t->Tid)) {
            J->CommitRequest = t->Tid;
            KeSetEvent(&J->WakeEvent, 0, FALSE);
        }

        if (!full &&
            (t->State == JTXN_RUNNING || (t->State == JTXN_LOCKED && !t->Sealed))) {
            if (s) {
                t->Updates++;
                s->Txn[slot] = t;
            }
            JnlUnlock(J, irql);
            return t;
        }
        JnlUnlock(J, irql);
        KeWaitForSingleObject(&J->UnlockedEvent, Executive, KernelMode, FALSE, &tick);
    }
}

static VOID JnlDetach(PEXT2_JOURNAL J, PEXT2_JTXN t)
{
    KIRQL irql;

    JnlLock(J, irql);
    ASSERT(t->Updates > 0);
    t->Updates--;
    /* the commit waits for a locked transaction to drain, a stop for any */
    if (t->Updates == 0 && (t->State == JTXN_LOCKED || (J->Flags & JF_STOPPING)))
        KeSetEvent(&J->UpdatesEvent, 0, FALSE);
    JnlUnlock(J, irql);
}

VOID
Ext2JournalEnterScope(IN PEXT2_VCB Vcb)
{
    PKTHREAD        Thread = KeGetCurrentThread();
    PEXT2_JSCOPE    s;
    KIRQL           irql;

    UNREFERENCED_PARAMETER(Vcb);

    if (!JnlScopeInited)
        return;

    KeAcquireSpinLock(&JnlScopeLock, &irql);
    s = JnlFindScope(Thread);
    if (s) {
        s->Ref++;
    } else {
        s = ExAllocateFromNPagedLookasideList(&JnlScopeLookaside);
        if (s) {
            RtlZeroMemory(s, sizeof(EXT2_JSCOPE));
            s->Thread = Thread;
            s->Ref = 1;
            InsertHeadList(&JnlScopeBuckets[JnlScopeHash(Thread)], &s->Link);
        }
    }
    KeReleaseSpinLock(&JnlScopeLock, irql);
    /* Scope entry owns only thread state. Stream CLOSE can arrive during
       teardown; its dispatch must not borrow the journal before locking
       the volume. Actual metadata registration requests commits below. */
}

VOID
Ext2JournalLeaveScope(IN PEXT2_VCB Vcb)
{
    PKTHREAD        Thread = KeGetCurrentThread();
    PEXT2_JSCOPE    s;
    KIRQL           irql;
    ULONG           i;

    UNREFERENCED_PARAMETER(Vcb);

    if (!JnlScopeInited)
        return;

    KeAcquireSpinLock(&JnlScopeLock, &irql);
    s = JnlFindScope(Thread);
    if (s && --s->Ref == 0) {
        RemoveEntryList(&s->Link);
    } else {
        s = NULL;
    }
    KeReleaseSpinLock(&JnlScopeLock, irql);

    if (!s)
        return;

    for (i = 0; i < s->Count; i++) {
        if (s->Txn[i])
            JnlDetach(s->Journal[i], s->Txn[i]);
    }
    ExFreeToNPagedLookasideList(&JnlScopeLookaside, s);
}

/*
 * Join the running transaction before taking a lock that holders of a
 * handle may wait for. A commit waits for every joined handle, so whoever
 * holds such a lock must never wait to join: a handle queued behind the
 * lock would keep the commit from ever finishing. Joined first, the
 * request's later modifications find the handle active and file their
 * buffers without waiting - Linux starts its handle before i_rwsem and the
 * allocator locks for the same reason.
 */
VOID
Ext2JournalJoin(IN PEXT2_VCB Vcb)
{
    PEXT2_JOURNAL J = Vcb->Journal;

    if (J != NULL && (J->Flags & JF_ACTIVE) && !(J->Flags & JF_ABORTED)) {
        JnlActivate(J);
    }
}

/*
 * __brelse() hook: a buffer the journal holds records for lost a holder.
 * A checkpoint waiting for such buffers to be let go (JnlCheckpointOldest)
 * looks at them again. Reading the event first keeps the common case - no
 * checkpoint waiting, or already woken - free of the dispatcher lock.
 */
VOID
Ext2JournalBufferReleased(IN PEXT2_VCB Vcb)
{
    PEXT2_JOURNAL J = Vcb->Journal;

    if (J != NULL && J->HeldWaiters != 0 && !KeReadStateEvent(&J->HeldEvent)) {
        KeSetEvent(&J->HeldEvent, IO_NO_INCREMENT, FALSE);
    }
}

/*
 * Give up this thread's active handle on J so that a commit can proceed
 * (fsync / flush semantics). The handle re-activates by itself at the
 * thread's next modification.
 */
VOID JnlDetachSelf(PEXT2_JOURNAL J)
{
    PKTHREAD        Thread = KeGetCurrentThread();
    PEXT2_JSCOPE    s;
    KIRQL           irql;
    ULONG           i;
    PEXT2_JTXN      t = NULL;

    if (!JnlScopeInited)
        return;

    KeAcquireSpinLock(&JnlScopeLock, &irql);
    s = JnlFindScope(Thread);
    if (s) {
        for (i = 0; i < s->Count; i++) {
            if (s->Journal[i] == J && s->Txn[i]) {
                t = s->Txn[i];
                s->Txn[i] = NULL;
                break;
            }
        }
    }
    KeReleaseSpinLock(&JnlScopeLock, irql);

    if (t)
        JnlDetach(J, t);
}

BOOLEAN
Ext2JournalIsActive(IN PEXT2_VCB Vcb)
{
    PEXT2_JOURNAL J = Vcb->Journal;
    return (J != NULL && (J->Flags & JF_ACTIVE) && !(J->Flags & JF_ABORTED));
}

/*
 * mark_buffer_dirty() hook: file the buffer into the running transaction.
 * Returns TRUE when the engine owns the operation, including an abort.
 * Only an inactive engine permits the legacy dirty-buffer path.
 */
BOOLEAN
Ext2JournalDirtyBuffer(IN PEXT2_VCB Vcb, IN struct buffer_head *bh)
{
    PEXT2_JOURNAL   J = Vcb->Journal;
    PEXT2_JREC      r, old;
    PEXT2_JTXN      t;
    KIRQL           irql;
    BOOLEAN         request = FALSE;

    if (!J || !(J->Flags & JF_ACTIVE))
        return FALSE;
    /* An aborted journal must not fall through to unjournaled writes. */
    if (J->Flags & JF_ABORTED)
        return TRUE;

    r = ExAllocateFromNPagedLookasideList(&J->RecLookaside);
    if (!r) {
        JnlAbort(J, STATUS_INSUFFICIENT_RESOURCES);
        return TRUE;
    }

    /* count this thread's handle in the running transaction */
    if (JnlActivate(J) == NULL) {
        ExFreeToNPagedLookasideList(&J->RecLookaside, r);
        return TRUE;
    }

    JnlLock(J, irql);

    if (!(J->Flags & JF_ACTIVE) || (J->Flags & JF_ABORTED)) {
        JnlUnlock(J, irql);
        ExFreeToNPagedLookasideList(&J->RecLookaside, r);
        return TRUE;
    }

    t = J->Running;
    old = (PEXT2_JREC)bh->b_jrec;

    if (old) {
        /* already in the running transaction; a freed-then-reused block
           becomes a normal one again (cancels the pending revoke) */
        if (old->Type == JREC_REVOKE) {
            old->Type = JREC_NORMAL;
            t->NumRevokes--;
            t->NumNormal++;
        }
        JnlUnlock(J, irql);
        ExFreeToNPagedLookasideList(&J->RecLookaside, r);
        return TRUE;
    }

    r->bh = bh;
    r->Block = bh->b_blocknr;
    r->Type = JREC_NORMAL;
    r->Snapshot = NULL;
    get_bh(bh);
    bh->b_jrec = r;
    bh->b_jtid = t->Tid;
    bh->b_jrefs++;
    InsertTailList(&t->Records, &r->Link);
    t->NumRecords++;
    t->NumNormal++;

    if (t->NumRecords >= J->MaxTxnBlocks && t->State == JTXN_RUNNING &&
        !tid_geq(J->CommitRequest, t->Tid)) {
        J->CommitRequest = t->Tid;
        request = TRUE;
    }

    JnlUnlock(J, irql);

    if (request)
        KeSetEvent(&J->WakeEvent, 0, FALSE);

    return TRUE;
}

/*
 * Ext2FreeBlock() hook: the blocks are being freed. Any copy of them in the
 * live log window must be revoked so that replay does not resurrect old
 * metadata into a block that may be reused for data (invariant I3).
 */
BOOLEAN
Ext2JournalRevokeBlocks(IN PEXT2_VCB Vcb, IN ULONGLONG Block, IN ULONG Count)
{
    PEXT2_JOURNAL        J = Vcb->Journal;
    struct block_device *bdev = &Vcb->bd;
    ULONG                i;

    if (!J || !(J->Flags & JF_ACTIVE))
        return TRUE;
    if (J->Flags & JF_ABORTED)
        return FALSE;

    if (JnlActivate(J) == NULL)
        return FALSE;

    for (i = 0; i < Count; i++) {

        struct buffer_head *bh = NULL;
        PEXT2_JREC          r;
        PEXT2_JTXN          t;
        KIRQL               irql;
        struct rb_node     *node;
        ULONGLONG           blk = Block + i;

        /* look the block up in the bh tree; a large range (a big file
           being freed) walks the tree once instead of probing per block */
        ExAcquireResourceSharedLite(&bdev->bd_bh_lock, TRUE);
        if (Count > 256) {
            node = rb_first(&bdev->bd_bh_root);
            while (node) {
                struct buffer_head *b = container_of(node, struct buffer_head, b_rb_node);
                if (b->b_blocknr >= blk) {
                    if (b->b_blocknr < Block + Count) {
                        bh = b;
                        i = (ULONG)(b->b_blocknr - Block);
                    } else {
                        i = Count;      /* nothing left in range */
                    }
                    break;
                }
                node = rb_next(node);
            }
            if (!node)
                i = Count;
        } else {
            node = bdev->bd_bh_root.rb_node;
            while (node) {
                struct buffer_head *b = container_of(node, struct buffer_head, b_rb_node);
                if (blk < b->b_blocknr)
                    node = node->rb_left;
                else if (blk > b->b_blocknr)
                    node = node->rb_right;
                else {
                    bh = b;
                    break;
                }
            }
        }
        if (bh)
            get_bh(bh);
        ExReleaseResourceLite(&bdev->bd_bh_lock);

        if (!bh)
            continue;       /* never journaled while in the live window */

        blk = bh->b_blocknr;
        r = ExAllocateFromNPagedLookasideList(&J->RecLookaside);

        JnlLock(J, irql);

        t = J->Running;

        if (!r && !bh->b_jrec && bh->b_jrefs > 0) {
            JnlUnlock(J, irql);
            put_bh(bh);
            JnlAbort(J, STATUS_INSUFFICIENT_RESOURCES);
            return FALSE;
        }

        if (bh->b_jrec) {
            /* dirtied in this transaction: log a revoke instead of data */
            PEXT2_JREC cur = (PEXT2_JREC)bh->b_jrec;
            if (cur->Type == JREC_NORMAL) {
                cur->Type = JREC_REVOKE;
                t->NumNormal--;
                t->NumRevokes++;
            }
            bh->b_jtid = t->Tid;
        } else if (bh->b_jrefs > 0 && r) {
            /* older copies in the log: revoke them */
            r->bh = bh;
            r->Block = blk;
            r->Type = JREC_REVOKE;
            r->Snapshot = NULL;
            get_bh(bh);
            bh->b_jrec = r;
            bh->b_jtid = t->Tid;
            bh->b_jrefs++;
            InsertTailList(&t->Records, &r->Link);
            t->NumRecords++;
            t->NumRevokes++;
            r = NULL;
        }

        JnlUnlock(J, irql);

        if (r)
            ExFreeToNPagedLookasideList(&J->RecLookaside, r);
        put_bh(bh);
    }
    return TRUE;
}
