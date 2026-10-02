/**
 * jnl_internal.h - definitions shared by the files split from commit.c.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * This is a Windows-native replacement for the parts of Linux jbd2 that
 * are tied to the Linux page cache (transaction.c, commit.c and
 * checkpoint.c). It reuses the on-disk format helpers of the jbd2 port
 * (journal.c / recovery.c) so that anything we write can be replayed by
 * Linux and by our own recovery code, and vice versa.
 *
 * Model
 * -----
 * All metadata modifications of the driver go through the buffer_head
 * layer in linux.c: the data lives in pinned cache-manager views of the
 * volume stream. While the engine is active, mark_buffer_dirty() files
 * the buffer into the running transaction instead of dirtying the cache.
 * The paging-write path (Ext2WriteVolume) only writes byte ranges that are
 * registered in Vcb->Extents, and journaled metadata is never registered,
 * so the cache manager can never push uncommitted metadata to disk.
 *
 * A commit copies the transaction's blocks (nobody is modifying them: all
 * handles have finished and new ones are held off), writes revoke blocks,
 * descriptor + data blocks, flushes the disk cache, writes the commit
 * block write-through and flushes again. Afterwards the transaction sits
 * on the checkpoint list, still holding a reference on every buffer so the
 * pinned pages stay resident until their final-location write is done.
 *
 * A checkpoint writes committed blocks to their home location straight
 * from the cached copy, taken while nobody holds the buffer, then drops
 * the reference. When every record of the oldest transaction is done the
 * log tail advances. The on-disk tail is published before the log wraps
 * over the space it frees.
 *
 * Handles are per thread: Ext2DispatchRequest enters a scope for every
 * IRP that may modify metadata. The handle becomes *active* (counted in
 * the transaction's Updates) at the thread's first modification and stays
 * active until the IRP is done. A commit locks the running transaction,
 * waits for the active handles to finish, seals it, copies the blocks and
 * only then opens the next transaction. A modification that arrives while
 * the transaction is locked but not yet sealed simply joins it; one that
 * arrives after sealing waits for the copy to finish, which is a bounded
 * memcpy with no locks involved. Threads that are merely blocked on a
 * resource have not modified anything and therefore never hold a commit
 * back, which is what keeps "flush while others are busy" deadlock free.
 *
 * Invariants
 * ----------
 *  I1  A block reaches its home location only through a checkpoint, and a
 *      checkpoint copies a block only when (a) its newest owner is a
 *      committed transaction and (b) nobody holds the buffer.
 *  I2  A transaction is dropped from the log only when every one of its
 *      records is either written home, superseded by a newer *committed*
 *      copy, or a revoke.
 *  I3  A block freed by the fs is revoked in the running transaction if any
 *      copy of it can still be replayed; a block freed and re-dirtied in the
 *      same transaction is logged normally (the revoke is cancelled).
 *  I4  The fs superblock says "needs recovery" on disk before the first
 *      log block of a session is written; it says "clean" only after the
 *      log is empty and every block is home.
 *
 * Lifetime: the EXT2_JOURNAL block lives as long as the volume stream
 * (Ext2TearDownStream frees it); sessions start and stop inside it, so a
 * request that looked at Vcb->Journal never sees freed memory.
 */

#ifndef _EXT4_JOURNAL_JNL_INTERNAL_H_
#define _EXT4_JOURNAL_JNL_INTERNAL_H_

/* journal flags */
#define JF_ACTIVE           0x00000001  /* engine accepts buffers */
#define JF_ABORTED          0x00000002  /* I/O error: fs went read-only */
#define JF_STOPPING         0x00000004  /* engine is shutting down */
#define JF_SB_MARKED        0x00000008  /* fs sb carries needs_recovery */

/* transaction states */
#define JTXN_RUNNING        1           /* accepting handles and buffers */
#define JTXN_LOCKED         2           /* draining handles, then copying */

/* record types */
#define JREC_NORMAL         0           /* block is logged and checkpointed */
#define JREC_REVOKE         1           /* block was freed: revoke record */

#define JNL_WAIT_TICK           ((LONGLONG)50 * 10 * 1000)         /* 50 ms */
#define JNL_STOP_DRAIN          ((LONGLONG)5 * 1000 * 1000 * 10)   /* 5 s: handles at stop */
#define JNL_HELD_STUCK          ((LONGLONG)100 * 1000 * 1000 * 10) /* 100 s: buffers held */

#define JNL_LOG_SLACK           32      /* free log blocks always kept    */
#define JNL_SCOPE_BUCKETS       64
#define JNL_SCOPE_JOURNALS      4       /* journals one thread can attach */

#define JNL_TAG                 'LNJE'

typedef struct _EXT2_JTXN EXT2_JTXN, *PEXT2_JTXN;
typedef struct _EXT2_JOURNAL EXT2_JOURNAL, *PEXT2_JOURNAL;

/* one record per (block, transaction) */
typedef struct _EXT2_JREC {
    LIST_ENTRY              Link;       /* EXT2_JTXN::Records */
    struct buffer_head     *bh;         /* referenced with get_bh() */
    ULONGLONG               Block;      /* fs block number */
    ULONG                   Type;       /* JREC_NORMAL / JREC_REVOKE */
    PUCHAR                  Snapshot;   /* commit-time copy (may be NULL) */
} EXT2_JREC, *PEXT2_JREC;

struct _EXT2_JTXN {
    LIST_ENTRY              Link;       /* EXT2_JOURNAL::Checkpoint */
    tid_t                   Tid;
    ULONG                   State;
    LIST_ENTRY              Records;    /* EXT2_JREC list */
    ULONG                   NumRecords;
    ULONG                   NumNormal;
    ULONG                   NumRevokes;
    LONG                    Updates;    /* active handles */
    BOOLEAN                 Sealed;     /* no more joins: copy in progress */
    LARGE_INTEGER           StartTime;
    unsigned long           LogStart;   /* first log block used */
};

struct _EXT2_JOURNAL {
    PEXT2_VCB               Vcb;
    journal_t              *jbd;        /* jbd2 port: sb, bmap, geometry */
    PEXT2_MCB               Jcb;        /* journal inode Mcb */

    KSPIN_LOCK              Lock;       /* guards everything below */
    ULONG                   Flags;
    ULONG                   NoScopeReports; /* "modified outside a scope" said */
    PEXT2_JTXN              Running;
    LIST_ENTRY              Checkpoint; /* committed, oldest first */
    ULONG                   CheckpointRecords;
    tid_t                   CommitRequest;
    tid_t                   CommitSequence;

    KMUTEX                  CommitLock; /* serializes commit/checkpoint */
    KEVENT                  WakeEvent;  /* commit thread wake-up */
    KEVENT                  UnlockedEvent;  /* running txn accepts handles */
    KEVENT                  UpdatesEvent;   /* handles drained on LOCKED txn */
    KEVENT                  HeldEvent;      /* a journaled buffer was released */
    volatile LONG           HeldWaiters;    /* a checkpoint is waiting for one */
    PKTHREAD                Thread;
    NPAGED_LOOKASIDE_LIST   RecLookaside;
    NPAGED_LOOKASIDE_LIST   TxnLookaside;

    ULONG                   BlockSize;
    ULONG                   BlockBits;
    ULONG                   MaxTxnBlocks;
    ULONG                   GroupBlocks;    /* data blocks per descriptor */
    ULONG                   RevokesPerBlock; /* records one revoke block holds;
                                               from the superblock at start:
                                               the sizing runs under Lock, and
                                               the superblock is a cache view,
                                               pageable */
    PUCHAR                  IoBuffer;       /* (GroupBlocks + 1) blocks */
    ULONG                   IoBufferSize;

    unsigned long           DiskTail;       /* s_start as last written */
    tid_t                   DiskTailSeq;    /* s_sequence as last written */
    USHORT                  MountState;     /* s_state before we touched it */
    LARGE_INTEGER           LastCommitTime;
};

/* per-thread handle scope */
typedef struct _EXT2_JSCOPE {
    LIST_ENTRY              Link;
    PKTHREAD                Thread;
    LONG                    Ref;
    ULONG                   Count;
    PEXT2_JOURNAL           Journal[JNL_SCOPE_JOURNALS];
    PEXT2_JTXN              Txn[JNL_SCOPE_JOURNALS];
} EXT2_JSCOPE, *PEXT2_JSCOPE;

extern LIST_ENTRY JnlScopeBuckets[JNL_SCOPE_BUCKETS];
extern KSPIN_LOCK JnlScopeLock;
extern NPAGED_LOOKASIDE_LIST JnlScopeLookaside;
extern BOOLEAN JnlScopeInited;

NTSTATUS JnlCommitTransaction(PEXT2_JOURNAL J);
NTSTATUS JnlCheckpointOldest(PEXT2_JOURNAL J, BOOLEAN Wait, BOOLEAN AllowCommit);
VOID     JnlAbort(PEXT2_JOURNAL J, NTSTATUS Status);

#define JnlLock(J, irql)    KeAcquireSpinLock(&(J)->Lock, &(irql))
#define JnlUnlock(J, irql)  KeReleaseSpinLock(&(J)->Lock, (irql))

static __inline LONGLONG JnlNow(VOID)
{
    LARGE_INTEGER t;
    KeQuerySystemTime(&t);
    return t.QuadPart;
}


PEXT2_JTXN JnlAllocTxn(PEXT2_JOURNAL J, tid_t Tid);

VOID JnlFreeTxn(PEXT2_JOURNAL J, PEXT2_JTXN t);

VOID JnlReleaseRecord(PEXT2_JOURNAL J, PEXT2_JREC r);

VOID JnlReleaseRecordList(PEXT2_JOURNAL J, PLIST_ENTRY List);

VOID JnlUnlinkRecord(PEXT2_JOURNAL J, PEXT2_JTXN t, PEXT2_JREC r);

unsigned long JnlNextLogBlock(PEXT2_JOURNAL J);

ULONG JnlLogBlocksNeeded(PEXT2_JOURNAL J, ULONG Normal, ULONG Revokes);

VOID JnlAdvanceTail(PEXT2_JOURNAL J);

NTSTATUS JnlWriteLogRun(PEXT2_JOURNAL J, unsigned long Log, ULONG Count,
                               PUCHAR Buffer, BOOLEAN WriteThrough);

NTSTATUS JnlPublishTail(PEXT2_JOURNAL J);

BOOLEAN JnlMarkSuper(PEXT2_JOURNAL J, BOOLEAN Dirty);

VOID JnlDetachSelf(PEXT2_JOURNAL J);

VOID JnlThread(PVOID Context);

#endif /* _EXT4_JOURNAL_JNL_INTERNAL_H_ */
