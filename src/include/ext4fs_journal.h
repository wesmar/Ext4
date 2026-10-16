/**
 * ext4fs_journal.h - write-ahead journal engine and mount-time recovery.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4FS_JOURNAL_H_
#define _EXT4_EXT4FS_JOURNAL_H_

/* replay what a crash left in the journal, then start the engine
   (journal\JournalLoad.c); nonzero: the volume stays read-only */
INT
Ext2RecoverJournal(
    PEXT2_IRP_CONTEXT  IrpContext,
    PEXT2_VCB          Vcb
);

struct buffer_head;

struct _EXT2_JLOG;

VOID
Ext2JournalGlobalInit(VOID);

VOID
Ext2JournalGlobalExit(VOID);

NTSTATUS
Ext2JournalStart(
    IN PEXT2_VCB            Vcb,
    IN struct _EXT2_JLOG   *Log,
    IN PEXT2_MCB            Jcb
);

VOID
Ext2JournalAbortQuiet(IN PEXT2_VCB Vcb);

/* a change could be neither finished nor undone: it must never commit */
VOID
Ext2JournalAbandon(IN PEXT2_VCB Vcb, IN NTSTATUS Status);

/* an aborted journal's error into its own superblock (Ext2QueueSuperWrite) */
VOID
Ext2JournalRecordAbort(IN PEXT2_VCB Vcb);

/* The transaction a change made now belongs to, or a later one: blocks
   freed by it may be reused once it has committed. FALSE: no journal is
   logging, a change is final when made. */
BOOLEAN
Ext2JournalFreeingTid(IN PEXT2_VCB Vcb, OUT PULONG Tid);

/* has transaction Tid committed (or is no journal logging any more)? */
BOOLEAN
Ext2JournalCommitted(IN PEXT2_VCB Vcb, IN ULONG Tid);

VOID
Ext2JournalStop(
    IN PEXT2_VCB        Vcb,
    IN BOOLEAN          MarkClean
);

VOID
Ext2JournalDestroy(
    IN PEXT2_VCB        Vcb
);

BOOLEAN
Ext2JournalIsActive(
    IN PEXT2_VCB        Vcb
);

VOID
Ext2JournalEnterScope(
    IN PEXT2_VCB        Vcb
);

VOID
Ext2JournalLeaveScope(
    IN PEXT2_VCB        Vcb
);

VOID
Ext2JournalJoin(
    IN PEXT2_VCB        Vcb
);

VOID
Ext2JournalBufferReleased(
    IN PEXT2_VCB        Vcb
);

BOOLEAN
Ext2JournalDirtyBuffer(
    IN PEXT2_VCB        Vcb,
    IN struct buffer_head *bh
);

BOOLEAN
Ext2JournalRevokeBlocks(
    IN PEXT2_VCB        Vcb,
    IN ULONGLONG        Block,
    IN ULONG            Count
);

NTSTATUS
Ext2JournalCommit(
    IN PEXT2_VCB        Vcb,
    IN BOOLEAN          Wait
);

NTSTATUS
Ext2JournalFlush(
    IN PEXT2_VCB        Vcb
);

NTSTATUS
Ext2JournalMarkClean(
    IN PEXT2_VCB        Vcb
);

BOOLEAN
Ext2SaveSuperDirect(
    IN PEXT2_VCB        Vcb
);

#endif /* _EXT4_EXT4FS_JOURNAL_H_ */
