/**
 * WriteLog.c - crash-state tests: every write and flush sent to the test
 * volume, in order. Built only with EXT4_WRITE_LOG (tests/crash-vm.ps1).
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * On a volume labelled "crashlog" each write is recorded as it is sent to
 * the disk - offset, length and the bytes - and each flush as a barrier.
 * Any prefix of the record is a state a power cut could leave the disk in;
 * tests/crash-replay.py replays every one onto the volume as it was before
 * the mount and lets e2fsck judge it. The record stays in memory and goes
 * to \SystemRoot\Temp\ext4-writelog.bin when the driver unloads.
 */

#include "ext4fs.h"

#ifdef EXT4_WRITE_LOG

#define WLOG_BYTES      ((SIZE_T)256 << 20)
#define WLOG_CHUNK      (1u << 20)
#define WLOG_MAGIC      0x474f4c57u         /* "WLOG" */
#define WLOG_TAG        'golW'
#define WLOG_LABEL      "crashlog"

/* one record, its bytes after it, padded to 8 */
typedef struct _WLOG_RECORD {
    ULONG       Magic;
    ULONG       Type;                       /* EXT4_WLOG_WRITE, EXT4_WLOG_FLUSH */
    ULONGLONG   Offset;
    ULONG       Length;
    ULONG       Sequence;
} WLOG_RECORD;

static PUCHAR       WlogBuffer;
static SIZE_T       WlogUsed;
static ULONG        WlogSequence;
static BOOLEAN      WlogFull;
static KSPIN_LOCK   WlogLock;

VOID
Ext4WriteLogInitialize(VOID)
{
    KeInitializeSpinLock(&WlogLock);
    WlogBuffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, WLOG_BYTES, WLOG_TAG);
}

VOID
Ext4WriteLogAttach(IN PEXT2_VCB Vcb)
{
    Vcb->WriteLog = RtlCompareMemory(Vcb->SuperBlock->s_volume_name, WLOG_LABEL,
                                     sizeof(WLOG_LABEL)) == sizeof(WLOG_LABEL);
}

/*
 * The slot is taken in order under the lock; the bytes are copied after it,
 * at the caller's level: a journal buffer may be a pageable cache view.
 */
VOID
Ext4WriteLogRecord(IN PEXT2_VCB Vcb, IN ULONG Type, IN ULONGLONG Offset,
                   IN ULONG Length, IN PVOID Data)
{
    SIZE_T      Size = sizeof(WLOG_RECORD) + (((SIZE_T)Length + 7) & ~(SIZE_T)7);
    PUCHAR      Slot = NULL;
    WLOG_RECORD Record;
    KIRQL       Irql;

    if (!Vcb->WriteLog || WlogBuffer == NULL) {
        return;
    }
    KeAcquireSpinLock(&WlogLock, &Irql);
    if (WlogUsed + Size > WLOG_BYTES) {
        WlogFull = TRUE;
    } else {
        Slot = WlogBuffer + WlogUsed;
        WlogUsed += Size;
        Record.Sequence = WlogSequence++;
    }
    KeReleaseSpinLock(&WlogLock, Irql);
    if (Slot == NULL) {
        return;
    }
    Record.Magic = WLOG_MAGIC;
    Record.Type = Type;
    Record.Offset = Offset;
    Record.Length = Length;
    if (Length) {
        RtlCopyMemory(Slot + sizeof(Record), Data, Length);
    }
    RtlCopyMemory(Slot, &Record, sizeof(Record));
}

/* at unload: every volume is gone, nothing records any more */
VOID
Ext4WriteLogSave(VOID)
{
    UNICODE_STRING      Name = RTL_CONSTANT_STRING(L"\\SystemRoot\\Temp\\ext4-writelog.bin");
    OBJECT_ATTRIBUTES   Attributes;
    IO_STATUS_BLOCK     Io;
    HANDLE              File;
    WLOG_RECORD         Head = { WLOG_MAGIC, 0, 0, 0, 0 };
    SIZE_T              Done;

    if (WlogBuffer == NULL) {
        return;
    }
    Head.Type = WlogFull;
    Head.Offset = WlogUsed;
    Head.Sequence = WlogSequence;
    InitializeObjectAttributes(&Attributes, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&File, GENERIC_WRITE | SYNCHRONIZE, &Attributes, &Io, NULL,
                                FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF,
                                FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        ZwWriteFile(File, NULL, NULL, NULL, &Io, &Head, sizeof(Head), NULL, NULL);
        for (Done = 0; Done < WlogUsed; Done += WLOG_CHUNK) {
            ZwWriteFile(File, NULL, NULL, NULL, &Io, WlogBuffer + Done,
                        (ULONG)min(WLOG_CHUNK, WlogUsed - Done), NULL, NULL);
        }
        ZwClose(File);
    }
    ExFreePoolWithTag(WlogBuffer, WLOG_TAG);
    WlogBuffer = NULL;
}

#endif /* EXT4_WRITE_LOG */
