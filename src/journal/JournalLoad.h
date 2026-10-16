/**
 * JournalLoad.h - a volume's journal log as loaded: geometry and superblock.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 */

#ifndef _EXT4_JOURNAL_LOAD_H_
#define _EXT4_JOURNAL_LOAD_H_

#include "JournalFormat.h"

/* a run of the journal file: log blocks [Log, Log + Count) at Phys */
typedef struct _EXT2_JRUN {
    ULONG       Log;
    ULONG       Count;
    ULONGLONG   Phys;
} EXT2_JRUN, *PEXT2_JRUN;

#define JLOG_FLUSHED    0x00000001      /* the log on disk is empty (s_start 0) */
#define JLOG_ABORTED    0x00000002      /* the tail is not written any more */

/*
 * The log of one journal session (JournalLoad.c builds it, the engine owns
 * it while active). The superblock is a nonpaged copy written straight to
 * disk; the fields below it are what the engine works with.
 */
typedef struct _EXT2_JLOG {
    PEXT2_VCB   Vcb;
    PUCHAR      Super;                  /* the journal superblock block */
    ULONG       BlockSize;
    ULONG       BlockBits;
    ULONG       Version;                /* 1 or 2 */
    ULONG       Compat;                 /* features, 0 for version 1 */
    ULONG       Incompat;
    ULONG       CsumSeed;               /* crc32c of the uuid, for v2/v3 */
    ULONG       MaxLen;                 /* journal blocks in use (s_maxlen) */
    ULONG       First;                  /* the log is [First, Last) */
    ULONG       Last;
    ULONG       Head;                   /* the next block to write */
    ULONG       Tail;                   /* the oldest block still needed */
    ULONG       Free;                   /* blocks between Head and Tail */
    ULONG       TailSequence;           /* the transaction at Tail */
    ULONG       TransactionSequence;    /* the next transaction */
    ULONG       CommitSequence;         /* the last one committed */
    ULONG       Flags;                  /* JLOG_* */
    LONG        Errno;                  /* what s_errno is to say */
    ULONG       RunCount;
    PEXT2_JRUN  Runs;                   /* the journal file, log -> volume */
    KEVENT      SuperGate;              /* one superblock change and write at a time */
} EXT2_JLOG, *PEXT2_JLOG;

/* the volume block of a log block, and how many follow it contiguously */
NTSTATUS JlogMap(IN PEXT2_JLOG Log, IN ULONG LogBlock, OUT PULONGLONG Phys, OUT PULONG Contiguous);

/* Each change below is written at once: checksummed, between two device
   flushes. Where recovery starts: Start 0 says the log is empty. */
NTSTATUS JlogSetTail(IN PEXT2_JLOG Log, IN ULONG Sequence, IN ULONG Start);

/* the features the log is written with from now on (the log is empty) */
NTSTATUS JlogSetFeatures(IN PEXT2_JLOG Log, IN ULONG Compat, IN ULONG Incompat);

/* Errno into s_errno */
NTSTATUS JlogRecordErrno(IN PEXT2_JLOG Log);

VOID JlogFree(IN PEXT2_JLOG Log);

#endif /* _EXT4_JOURNAL_LOAD_H_ */
