/**
 * JournalReplay.h - replaying a jbd2 log: what is committed goes home.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * Plain C with no system header: the driver, the host model (MSVC, x86 and
 * x64) and the differential test against e2fsck (gcc in WSL) compile the
 * same file. The caller supplies the log's geometry from its superblock and
 * the I/O; see JournalReplay.c for the rules.
 */

#ifndef _EXT4_JOURNAL_REPLAY_H_
#define _EXT4_JOURNAL_REPLAY_H_

/* results: 0, or the negative error Linux reports for the same log */
#define JR_OK           0
#define JR_EIO          (-5)
#define JR_ENOMEM       (-12)
#define JR_EINVAL       (-22)
#define JR_EBADCRC      (-74)       /* a checksum does not match */
#define JR_ECORRUPT     (-117)      /* the superblock points outside the log */

typedef struct _JR_JOURNAL {
    unsigned int    BlockSize;
    unsigned int    First;          /* s_first: the log is [First, Last) */
    unsigned int    Last;           /* s_maxlen */
    unsigned int    TotalLen;       /* blocks that may be read */
    unsigned int    Compat;         /* 0 for a version 1 superblock */
    unsigned int    Incompat;
    unsigned int    CsumSeed;       /* crc32c of the uuid, for v2/v3 */
    unsigned int    Start;          /* s_start; 0: nothing to replay */
    unsigned int    Sequence;       /* s_sequence: the first transaction */

    void           *Context;
    /* read log block LogBlock (0 = the superblock) into Buffer */
    int           (*ReadLog)(void *Context, unsigned int LogBlock, unsigned char *Buffer);
    /* write one block of the volume */
    int           (*WriteHome)(void *Context, unsigned long long Block, const unsigned char *Buffer);
    /* crc32c without the final inversion, as ext4 and jbd2 use it */
    unsigned int  (*Crc32c)(unsigned int Crc, const unsigned char *Data, unsigned int Length);
    void         *(*Allocate)(void *Context, unsigned int Bytes);
    void          (*Release)(void *Context, void *Memory);
} JR_JOURNAL;

typedef struct _JR_RESULT {
    unsigned int    StartTid;       /* s_sequence */
    unsigned int    EndTid;         /* the first transaction not replayed */
    unsigned int    ScanEndTid;     /* the first one the scan did not find: past
                                       End when an async commit failed; a new
                                       log's IDs start after it */
    unsigned int    HeadBlock;      /* the log block after the last good commit */
    unsigned int    FailedCommit;   /* valid when HasFailedCommit */
    int             HasFailedCommit;/* a commit failed its checksum, no async */
    unsigned int    Replayed;       /* blocks written home */
    unsigned int    Revokes;        /* revoke records read */
    unsigned int    RevokeHits;     /* logged copies a revoke kept from home */
    int             WriteError;     /* the first failed WriteHome, or 0 */
} JR_RESULT;

/* replay the log: JR_OK, or the first error (blocks before it are home) */
int JrRecover(const JR_JOURNAL *Journal, JR_RESULT *Result);

/* only find where the log ends (a log that is to be discarded) */
int JrScan(const JR_JOURNAL *Journal, JR_RESULT *Result);

/* crc32 of the version 1 commit checksum (big-endian, no inversion) */
unsigned int JrCrc32Be(unsigned int Crc, const unsigned char *Data, unsigned int Length);

#endif /* _EXT4_JOURNAL_REPLAY_H_ */
