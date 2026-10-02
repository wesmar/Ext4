/**
 * ext4fs_journal.h - write-ahead journal engine and mount-time recovery.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4FS_JOURNAL_H_
#define _EXT4_EXT4FS_JOURNAL_H_

PEXT2_MCB
Ext2LoadInternalJournal(
    PEXT2_VCB         Vcb,
    ULONG             jNo
);

INT
Ext2CheckJournal(
    PEXT2_VCB          Vcb,
    PULONG             jNo
);

INT
Ext2RecoverJournal(
    PEXT2_IRP_CONTEXT  IrpContext,
    PEXT2_VCB          Vcb
);

struct buffer_head;

struct journal_s;

NTSTATUS
Ext2JournalGlobalInit(VOID);

VOID
Ext2JournalGlobalExit(VOID);

NTSTATUS
Ext2JournalStart(
    IN PEXT2_VCB        Vcb,
    IN struct journal_s *journal,
    IN PEXT2_MCB        Jcb
);

VOID
Ext2JournalAbortQuiet(IN PEXT2_VCB Vcb);

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

VOID
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
