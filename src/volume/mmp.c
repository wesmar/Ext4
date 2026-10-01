/**
 * mmp.c - multi-mount protection (mkfs -O mmp), as Linux fs/ext4/mmp.c.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Linux fs/ext4/mmp.c.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * A volume on shared storage (a SAN LUN, a disk two machines can reach)
 * carries a block whose sequence number the node that has it mounted
 * rewrites every s_mmp_update_interval seconds. Before a writable mount:
 *
 *   seq == CLEAN   nobody has it: take it
 *   seq == FSCK    e2fsck is running: refuse
 *   anything else  wait two check intervals; if seq moved, a node is alive
 *                  and the mount is refused; if not, that node is gone
 *
 * Taking it means writing a random seq, waiting, and reading it back: had
 * another node started the same at the same time, one of the two sees the
 * other's number. Then a thread rewrites the block every interval. When a
 * write was late, it reads the block back first: someone else's number or
 * node name means the volume is mounted elsewhere, and from then on it is
 * read-only here and nothing more is written - not even the superblock.
 * A clean dismount writes CLEAN.
 *
 * The waits are the protocol's own (another node is given time to show
 * itself), not a guess at how long something might take.
 */

#include "ext4fs.h"
#include <ntstrsafe.h>

#define MMP_TAG                 'PM4E'
#define MMP_NODENAME_KEY        L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\ComputerName\\ActiveComputerName"
#define MMP_NODENAME_VALUE      L"ComputerName"
#define MMP_DEVICE_NAME         "ext4.sys"
#define MMP_SECOND              (10 * 1000 * 1000LL)    /* in 100 ns units */
#define MMP_RETRY_SEQ           16                      /* draws for a seq <= SEQ_MAX */

typedef struct _EXT4_MMP {
    PEXT2_VCB           Vcb;
    ULONGLONG           Offset;         /* of the MMP block on the volume */
    ULONG               Size;           /* the block size */
    struct mmp_struct  *Block;          /* our copy, a whole block */
    ULONG               UpdateInterval; /* seconds */
    ULONG               Seq;            /* the last one we wrote */
    KEVENT              Stop;
    HANDLE              Thread;         /* a kernel handle */
    volatile LONG       Lost;           /* another node wrote it: hands off */
    volatile LONG       Release;        /* on stop: write CLEAN */
    ULONG               Seed;
} EXT4_MMP, *PEXT4_MMP;

static ULONG MmpChecksum(PEXT2_VCB Vcb, struct mmp_struct *Mmp)
{
    return ext4_chksum(EXT3_SB(&Vcb->sb), EXT3_SB(&Vcb->sb)->s_csum_seed,
                       Mmp, (unsigned int)FIELD_OFFSET(struct mmp_struct, mmp_checksum));
}

/* the block from disk into Into, checked: magic, and checksum with metadata_csum */
static NTSTATUS MmpRead(PEXT4_MMP M, struct mmp_struct *Into)
{
    NTSTATUS Status = Ext2ReadSync(M->Vcb, M->Offset, M->Size, Into);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    if (le32_to_cpu(Into->mmp_magic) != EXT4_MMP_MAGIC) {
        DbgPrint("ext4: MMP block %I64u has no MMP magic\n", M->Offset / M->Size);
        return STATUS_DISK_CORRUPT_ERROR;
    }
    if (ext4_has_metadata_csum(&M->Vcb->sb) &&
        le32_to_cpu(Into->mmp_checksum) != MmpChecksum(M->Vcb, Into)) {
        DbgPrint("ext4: MMP block %I64u: checksum invalid\n", M->Offset / M->Size);
        return STATUS_DISK_CORRUPT_ERROR;
    }
    return STATUS_SUCCESS;
}

/* our copy to disk, written through, with the time and the checksum */
static NTSTATUS MmpWrite(PEXT4_MMP M)
{
    LARGE_INTEGER Now;

    KeQuerySystemTime(&Now);
    M->Block->mmp_time = cpu_to_le64((__u64)Ext2UnixTime(&Now));
    if (ext4_has_metadata_csum(&M->Vcb->sb)) {
        M->Block->mmp_checksum = cpu_to_le32(MmpChecksum(M->Vcb, M->Block));
    }
    return Ext2WriteDiskSync(M->Vcb, M->Offset, M->Size, M->Block, TRUE);
}

/* a random sequence number a running node could have written */
static ULONG MmpNewSeq(PEXT4_MMP M)
{
    ULONG Seq = 1, i;

    for (i = 0; i < MMP_RETRY_SEQ; i++) {
        Seq = RtlRandomEx(&M->Seed);
        if (Seq != 0 && Seq <= EXT4_MMP_SEQ_MAX) {
            break;
        }
        Seq = (Seq % EXT4_MMP_SEQ_MAX) + 1;
    }
    return Seq;
}

/* this machine's name, for the node field (informational, as in Linux) */
static VOID MmpNodeName(char *Name, ULONG Size)
{
    UCHAR                           Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 64 * sizeof(WCHAR)];
    PKEY_VALUE_PARTIAL_INFORMATION  Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    UNICODE_STRING                  KeyName, ValueName, Value;
    ANSI_STRING                     Ansi;
    OBJECT_ATTRIBUTES               Attributes;
    HANDLE                          Key;
    ULONG                           Got;

    RtlZeroMemory(Name, Size);
    RtlStringCbCopyA(Name, Size, "windows");

    RtlInitUnicodeString(&KeyName, MMP_NODENAME_KEY);
    RtlInitUnicodeString(&ValueName, MMP_NODENAME_VALUE);
    InitializeObjectAttributes(&Attributes, &KeyName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&Key, KEY_QUERY_VALUE, &Attributes))) {
        return;
    }
    if (NT_SUCCESS(ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Info,
                                   sizeof(Buffer) - sizeof(WCHAR), &Got)) &&
        Info->Type == REG_SZ && Info->DataLength >= sizeof(WCHAR)) {
        Value.Buffer = (PWCH)Info->Data;
        Value.Length = (USHORT)(Info->DataLength - sizeof(WCHAR));
        Value.MaximumLength = Value.Length;
        RtlZeroMemory(Name, Size);      /* no tail of the default name left */
        Ansi.Buffer = Name;
        Ansi.Length = 0;
        Ansi.MaximumLength = (USHORT)(Size - 1);
        if (!NT_SUCCESS(RtlUnicodeStringToAnsiString(&Ansi, &Value, FALSE))) {
            RtlStringCbCopyA(Name, Size, "windows");
        }
    }
    ZwClose(Key);
}

/*
 * The protocol's waits: give another node the time to show itself. Cut
 * short by "sc stop" (the mount would be refused anyway, and the unload
 * should not wait out a minute): STATUS_CANCELLED then.
 */
static NTSTATUS MmpWait(ULONG Seconds)
{
    LARGE_INTEGER Interval;

    Interval.QuadPart = -(LONGLONG)Seconds * MMP_SECOND;
    if (KeWaitForSingleObject(&Ext2Global->UnloadStarted, Executive, KernelMode,
                              FALSE, &Interval) == STATUS_SUCCESS) {
        DbgPrint("ext4: MMP: wait cut short, the driver is unloading\n");
        return STATUS_CANCELLED;
    }
    return STATUS_SUCCESS;
}

/*
 * Another node has the volume: read-only from now on, and nothing more is
 * written - no commit, no superblock (a journal abort would write one).
 */
static VOID MmpLose(PEXT4_MMP M, const char *Why)
{
    PEXT2_VCB Vcb = M->Vcb;

    if (InterlockedExchange(&M->Lost, TRUE)) {
        return;
    }
    DbgPrint("ext4: MMP: %s; the volume is mounted elsewhere (node \"%.64s\"), "
             "read-only here from now on\n", Why, M->Block->mmp_nodename);
    SetLongFlag(Vcb->Flags, VCB_READ_ONLY);
    Ext2JournalAbortQuiet(Vcb);
}

static VOID MmpThread(PVOID Context)
{
    PEXT4_MMP           M = Context;
    struct mmp_struct  *Check;
    ULONG               CheckInterval;
    NTSTATUS            Status;

    Check = Ext2AllocatePool(NonPagedPoolNx, M->Size, MMP_TAG);

    /* start with the longer check interval, shortened when writes are on time */
    CheckInterval = max((ULONG)EXT4_MMP_CHECK_MULT * M->UpdateInterval,
                        (ULONG)EXT4_MMP_MIN_CHECK_INTERVAL);

    for (;;) {
        LARGE_INTEGER   Due;
        LONGLONG        Written, Elapsed;

        if (++M->Seq > EXT4_MMP_SEQ_MAX) {
            M->Seq = 1;
        }
        M->Block->mmp_seq = cpu_to_le32(M->Seq);
        M->Block->mmp_check_interval = cpu_to_le16((__u16)CheckInterval);
        Written = (LONGLONG)KeQueryInterruptTime();
        Status = IsFlagOn(M->Vcb->Flags, VCB_DEVICE_REMOVED) ? STATUS_NO_SUCH_DEVICE : MmpWrite(M);
        if (!NT_SUCCESS(Status)) {
            DbgPrint("ext4: MMP: update failed (%08x)\n", Status);
        }

        Elapsed = (LONGLONG)KeQueryInterruptTime() - Written;
        Due.QuadPart = -max((LONGLONG)M->UpdateInterval * MMP_SECOND - Elapsed, 0);
        if (KeWaitForSingleObject(&M->Stop, Executive, KernelMode, FALSE, &Due) == STATUS_SUCCESS) {
            break;
        }

        /* late: someone else may have written meanwhile */
        Elapsed = (LONGLONG)KeQueryInterruptTime() - Written;
        if (Check && Elapsed > (LONGLONG)CheckInterval * MMP_SECOND) {
            if (NT_SUCCESS(MmpRead(M, Check)) &&
                (Check->mmp_seq != M->Block->mmp_seq ||
                 memcmp(Check->mmp_nodename, M->Block->mmp_nodename, sizeof(Check->mmp_nodename)))) {
                RtlCopyMemory(M->Block->mmp_nodename, Check->mmp_nodename, sizeof(Check->mmp_nodename));
                MmpLose(M, "the MMP block changed under us");
                break;
            }
        }

        CheckInterval = (ULONG)max(min((ULONGLONG)EXT4_MMP_CHECK_MULT * (ULONGLONG)(Elapsed / MMP_SECOND),
                                       (ULONGLONG)EXT4_MMP_MAX_CHECK_INTERVAL),
                                   (ULONGLONG)EXT4_MMP_MIN_CHECK_INTERVAL);
    }

    /* a clean dismount: the volume is free again (not when the device went
       away, nor when another node holds the volume) */
    if (!M->Lost && M->Release && !IsFlagOn(M->Vcb->Flags, VCB_DEVICE_REMOVED)) {
        M->Block->mmp_seq = cpu_to_le32(EXT4_MMP_SEQ_CLEAN);
        MmpWrite(M);
    }
    if (Check) {
        Ext2FreePool(Check, MMP_TAG);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID MmpFree(PEXT4_MMP M)
{
    if (M->Block) {
        Ext2FreePool(M->Block, MMP_TAG);
    }
    Ext2FreePool(M, MMP_TAG);
}

/*
 * Before a writable mount, ahead of anything written (the journal replay
 * first of all). STATUS_SUCCESS: the volume is ours and the update thread
 * runs. Any failure: it is not ours to write - mount it read-only, without
 * a replay.
 */
NTSTATUS
Ext4MmpStart(IN PEXT2_VCB Vcb)
{
    struct ext4_super_block *es = Vcb->SuperBlock;
    ULONGLONG               Block = le64_to_cpu(es->s_mmp_block);
    ULONG                   Interval = le16_to_cpu(es->s_mmp_update_interval);
    ULONG                   CheckInterval, Wait = 0, Seq;
    PEXT4_MMP               M;
    OBJECT_ATTRIBUTES       Attributes;
    LARGE_INTEGER           Tick;
    NTSTATUS                Status;

    if (Block < le32_to_cpu(es->s_first_data_block) || Block >= TOTAL_BLOCKS) {
        DbgPrint("ext4: MMP block %I64u is outside the volume\n", Block);
        return STATUS_DISK_CORRUPT_ERROR;
    }

    M = Ext2AllocatePool(NonPagedPoolNx, sizeof(EXT4_MMP), MMP_TAG);
    if (M == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(M, sizeof(EXT4_MMP));
    M->Vcb = Vcb;
    M->Size = BLOCK_SIZE;
    M->Offset = Block << BLOCK_BITS;
    M->UpdateInterval = max(Interval, 1);
    KeQueryTickCount(&Tick);
    M->Seed = Tick.LowPart ^ (ULONG)(ULONG_PTR)M;
    KeInitializeEvent(&M->Stop, NotificationEvent, FALSE);
    M->Block = Ext2AllocatePool(NonPagedPoolNx, M->Size, MMP_TAG);
    if (M->Block == NULL) {
        MmpFree(M);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = MmpRead(M, M->Block);
    if (!NT_SUCCESS(Status)) {
        goto failed;
    }

    CheckInterval = max(Interval, (ULONG)EXT4_MMP_MIN_CHECK_INTERVAL);
    if ((ULONG)le16_to_cpu(M->Block->mmp_check_interval) > CheckInterval) {
        CheckInterval = le16_to_cpu(M->Block->mmp_check_interval);
    }

    Seq = le32_to_cpu(M->Block->mmp_seq);
    /* FSCK, or any other code above SEQ_MAX: not safe to use (Linux says so) */
    if (Seq == EXT4_MMP_SEQ_FSCK || (Seq > EXT4_MMP_SEQ_MAX && Seq != EXT4_MMP_SEQ_CLEAN)) {
        DbgPrint("ext4: MMP: e2fsck is running on the volume (node \"%.64s\")\n",
                 M->Block->mmp_nodename);
        Status = STATUS_DEVICE_BUSY;
        goto failed;
    }
    if (Seq != EXT4_MMP_SEQ_CLEAN) {
        /* in use, or left so by a node that died: give it time to write */
        Wait = min(CheckInterval * 2 + 1, CheckInterval + 60);
        DbgPrint("ext4: MMP: volume not cleanly released (node \"%.64s\"), "
                 "waiting %u s for a sign of life\n", M->Block->mmp_nodename, Wait);
        Status = MmpWait(Wait);
        if (!NT_SUCCESS(Status)) {
            goto failed;
        }
        Status = MmpRead(M, M->Block);
        if (!NT_SUCCESS(Status)) {
            goto failed;
        }
        if (le32_to_cpu(M->Block->mmp_seq) != Seq) {
            DbgPrint("ext4: MMP: the volume is active on node \"%.64s\"\n",
                     M->Block->mmp_nodename);
            Status = STATUS_DEVICE_BUSY;
            goto failed;
        }
    }

    /* claim it: a number of our own, then check nobody claimed it as well */
    Seq = MmpNewSeq(M);
    M->Block->mmp_seq = cpu_to_le32(Seq);
    Status = MmpWrite(M);
    if (!NT_SUCCESS(Status)) {
        goto failed;
    }
    if (Wait) {
        Status = MmpWait(Wait);
        if (!NT_SUCCESS(Status)) {
            /* our number is still there: nobody else wants the volume, and
               it was free (clean, or its last owner proven gone) - leave it
               clean, not looking like a crash of ours */
            if (NT_SUCCESS(MmpRead(M, M->Block)) && le32_to_cpu(M->Block->mmp_seq) == Seq) {
                M->Block->mmp_seq = cpu_to_le32(EXT4_MMP_SEQ_CLEAN);
                MmpWrite(M);
            }
            goto failed;
        }
    }
    Status = MmpRead(M, M->Block);
    if (!NT_SUCCESS(Status)) {
        goto failed;
    }
    if (le32_to_cpu(M->Block->mmp_seq) != Seq) {
        DbgPrint("ext4: MMP: the volume is active on node \"%.64s\"\n",
                 M->Block->mmp_nodename);
        Status = STATUS_DEVICE_BUSY;
        goto failed;
    }

    M->Seq = Seq;
    MmpNodeName(M->Block->mmp_nodename, sizeof(M->Block->mmp_nodename));
    RtlZeroMemory(M->Block->mmp_bdevname, sizeof(M->Block->mmp_bdevname));
    RtlStringCbCopyA(M->Block->mmp_bdevname, sizeof(M->Block->mmp_bdevname), MMP_DEVICE_NAME);

    InitializeObjectAttributes(&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread(&M->Thread, SYNCHRONIZE, &Attributes, NULL, NULL, MmpThread, M);
    if (!NT_SUCCESS(Status)) {
        goto failed;
    }

    Vcb->Mmp = M;
    return STATUS_SUCCESS;

failed:
    MmpFree(M);
    return Status;
}

/*
 * The volume is let go: the update thread ends, writing CLEAN when Release
 * (an orderly dismount, the device still there) unless another node took
 * the volume. Safe to call more than once.
 */
VOID
Ext4MmpStop(IN PEXT2_VCB Vcb, IN BOOLEAN Release)
{
    PEXT4_MMP M = Vcb->Mmp;

    if (M == NULL) {
        return;
    }
    Vcb->Mmp = NULL;
    InterlockedExchange(&M->Release, Release);
    KeSetEvent(&M->Stop, IO_NO_INCREMENT, FALSE);
    ZwWaitForSingleObject(M->Thread, FALSE, NULL);
    ZwClose(M->Thread);
    MmpFree(M);
}

/* a volume about to become writable: with multi-mount protection only if
   the protocol gives it to us */
BOOLEAN
Ext4MmpHold(IN PEXT2_VCB Vcb)
{
    if (!EXT3_HAS_INCOMPAT_FEATURE(&Vcb->sb, EXT4_FEATURE_INCOMPAT_MMP) || Vcb->Mmp) {
        return TRUE;
    }
    return NT_SUCCESS(Ext4MmpStart(Vcb));
}
