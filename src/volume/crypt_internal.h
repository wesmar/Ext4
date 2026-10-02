/**
 * crypt_internal.h - what the LUKS device files share (crypt.c, crypt_io.c).
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_VOLUME_CRYPT_INTERNAL_H_
#define _EXT4_VOLUME_CRYPT_INTERNAL_H_

#include <bcrypt.h>
#include "ext4crypt.h"
#include "ext4xts.h"

#define CRYPT_TAG               'YC4E'

/*
 * Worker threads per device. A worker reads a piece from the disk, then
 * decrypts it: half its time it waits, half it computes, so two per
 * processor keep every processor busy (threads = processors x (1 + wait /
 * compute)). Past that, the disk is the limit: by Little's law it runs at
 * full speed once its throughput times its latency is in flight - for the
 * fastest NVMe drives, about 7 GB/s x 0.5 ms = EXT4_CRYPT_INFLIGHT - and a
 * thread more would only hold another bounce buffer of nonpaged pool.
 */
#define EXT4_CRYPT_BOUNCE       (256 * 1024)        /* per thread: largest piece written at once */
#define EXT4_CRYPT_INFLIGHT     (4 * 1024 * 1024)   /* bytes in flight that saturate a disk */
#define EXT4_CRYPT_PER_CPU      2                   /* workers per processor: wait ~ compute */
#define EXT4_CRYPT_WORKERS      (EXT4_CRYPT_INFLIGHT / EXT4_CRYPT_BOUNCE)
#define EXT4_CRYPT_INLINE_MAX   (sizeof(LONG) * 8)  /* key contexts for writes in the caller's
                                                       thread: one bit each in InlineBusy */
#define EXT4_CRYPT_INLINE_LIMIT (1024 * 1024)       /* longer writes go to the workers in pieces */
#define EXT4_CRYPT_UNIQUE_PREFIX "EXT4-LUKS-"

typedef struct _EXT4_CRYPT_DEVICE EXT4_CRYPT_DEVICE, *PEXT4_CRYPT_DEVICE;

typedef struct _EXT4_CRYPT_WORKER {
    PEXT4_CRYPT_DEVICE  Crypt;
    PKTHREAD            Thread;
    EXT4_XTS            Xts;                /* own key handles: nothing shared between threads */
    PUCHAR              Bounce;
    PMDL                BounceMdl;          /* describes all of Bounce */
    PMDL                PartMdl;            /* the part of it one lower request uses */
    PMDL                UserMdl;            /* a piece of the caller's buffer */
    PUCHAR              Scratch;            /* the tweaks of one sector */
    LIST_ENTRY          Stop;               /* queued to end the thread */
} EXT4_CRYPT_WORKER, *PEXT4_CRYPT_WORKER;

/*
 * The unit of work: a piece of an IRP. A request up to one bounce buffer
 * long, or one whose pieces could share an encryption sector, is a single
 * piece that lives in the IRP's own DriverContext (four pointers: exactly
 * this structure) - nothing is allocated, so a paging write always makes
 * progress. A longer aligned one is cut into pieces of EXT4_CRYPT_BOUNCE
 * held by one allocation; the workers take them in parallel and the one
 * that finishes the last piece completes the IRP.
 */
typedef struct _EXT4_CRYPT_PIECE {
    LIST_ENTRY          Entry;              /* in the device's queue */
    struct _EXT4_CRYPT_SPLIT *Split;        /* NULL: the whole IRP, in its DriverContext */
    ULONG               Offset;             /* into the IRP's buffer */
    ULONG               Length;
} EXT4_CRYPT_PIECE, *PEXT4_CRYPT_PIECE;

C_ASSERT(sizeof(EXT4_CRYPT_PIECE) <= sizeof(((PIRP)0)->Tail.Overlay.DriverContext));

typedef struct _EXT4_CRYPT_SPLIT {
    PIRP                Irp;
    volatile LONG       Pending;            /* pieces not finished */
    volatile LONG       Status;             /* the first failure */
    EXT4_CRYPT_PIECE    Piece[ANYSIZE_ARRAY];
} EXT4_CRYPT_SPLIT, *PEXT4_CRYPT_SPLIT;

struct _EXT4_CRYPT_DEVICE {
    EXT2_IDENTIFIER     Identifier;         /* EXT4CRY */
    LIST_ENTRY          Link;               /* Ext4CryptList */
    PDEVICE_OBJECT      Self;
    PDEVICE_OBJECT      Lower;              /* top of the partition's stack, referenced */
    PFILE_OBJECT        LowerFile;          /* our open of the partition */
    ULONG               Number;             /* \Device\Ext4Crypt<Number>, never reused */
    ULONG               Flags;              /* EXT4_CRYPT_READ_ONLY, _IV_LARGE_SECTORS */
    ULONG               Cipher;
    ULONG               KeyBytes;
    ULONG               SectorSize;
    ULONG               SectorShift;
    ULONGLONG           Offset;             /* of the segment on the partition, bytes */
    ULONGLONG           Size;               /* of the segment, bytes */
    ULONGLONG           IvOffset;
    KQUEUE              Queue;              /* IRPs for the workers */
    ULONG               Workers;
    EXT4_CRYPT_WORKER   Worker[EXT4_CRYPT_WORKERS];
    ERESOURCE           Rmw;                /* partial-sector writes: exclusive; whole: shared */
    EX_RUNDOWN_REF      Rundown;            /* I/O in flight */
    volatile LONG       InlineBusy;         /* bit n set: Inline[n] in use */
    ULONG               InlineCount;
    EXT4_CRYPT_WORKER   Inline[EXT4_CRYPT_INLINE_MAX];  /* Xts and Scratch only */
    volatile LONG       Closing;
    UNICODE_STRING      Name;
    WCHAR               NameBuffer[48];
    WCHAR               Source[EXT4_CRYPT_DEVICE_CHARS];
    CHAR                Uuid[EXT4_CRYPT_UUID_CHARS];
};

/* the open devices (crypt.c) */
extern LIST_ENTRY          Ext4CryptList;
extern FAST_MUTEX          Ext4CryptListLock;
extern BCRYPT_ALG_HANDLE   Ext4AesEcb;

/* the I/O engine (crypt_io.c) */
NTSTATUS
Ext4CryptLowerIoctl(IN PDEVICE_OBJECT Lower, IN ULONG Code, OUT PVOID Out, IN ULONG OutLength);

NTSTATUS
Ext4CryptStartWorkers(IN PEXT4_CRYPT_DEVICE Crypt, IN const UCHAR *Key, IN ULONG Count);

NTSTATUS
Ext4CryptStartInline(IN PEXT4_CRYPT_DEVICE Crypt, IN const UCHAR *Key);

VOID
Ext4CryptStopWorkers(IN PEXT4_CRYPT_DEVICE Crypt);

VOID
Ext4CryptFreeWorkers(IN PEXT4_CRYPT_DEVICE Crypt);

#endif /* _EXT4_VOLUME_CRYPT_INTERNAL_H_ */
