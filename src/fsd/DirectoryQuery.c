/**
 * DirectoryQuery.c - IRP_MN_QUERY_DIRECTORY: directory enumeration.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "dirctl_internal.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2QueryDirectory)
#endif

/*
 * The handle's search pattern, set by its first query and kept for the
 * ones that follow (FileName, if given again, is ignored then, as on NTFS):
 * the name asked for, upcased, or "*". FirstQuery: this query set it.
 */
static NTSTATUS
Ext2QueryPattern(IN PEXT2_CCB Ccb, IN PUNICODE_STRING FileName, OUT PBOOLEAN FirstQuery)
{
    static const WCHAR  All[] = L"*";
    PUNICODE_STRING     Pattern = &Ccb->DirectorySearchPattern;
    USHORT              Bytes;
    NTSTATUS            Status;

    *FirstQuery = Pattern->Buffer == NULL;
    if (!*FirstQuery) {
        return STATUS_SUCCESS;
    }

    Bytes = FileName != NULL && FileName->Length > 0 ? FileName->Length : sizeof(All) - sizeof(WCHAR);
    Pattern->Buffer = Ext2AllocatePool(PagedPool, Bytes, EXT2_DIRSP_MAGIC);
    if (Pattern->Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    INC_MEM_COUNT(PS_DIR_PATTERN, Pattern->Buffer, Bytes);
    Pattern->Length = Pattern->MaximumLength = Bytes;

    if (FileName != NULL && FileName->Length > 0) {
        Status = RtlUpcaseUnicodeString(Pattern, FileName, FALSE);
    } else {
        RtlCopyMemory(Pattern->Buffer, All, Bytes);
        Status = STATUS_SUCCESS;
    }
    return Status;
}

/*
 * A first query for one exact name (no wildcards) is a lookup, not a scan:
 * the htree finds it in O(log n), and a miss costs one pass over the
 * directory blocks (ext3_find_entry falls back to the case-insensitive
 * scan). Callers do this all the time - Test-Path, Get-Item, and the filter
 * manager, which normalizes the name of every file being created by asking
 * its directory for it. That last case is a miss by definition, and it must
 * not fall through to the general scan: that one pattern-matches every
 * entry, which made creating a file cost O(n) in the size of its directory.
 *
 * Either way the query is complete: a continuation must report "no more
 * files", so the position is set to the end - the htree end mark, which
 * the walk in disk order also treats as past the end. Symlinks and volumes
 * with hiding patterns take the general path, which knows how to present
 * them. STATUS_MORE_PROCESSING_REQUIRED: the general path is to answer.
 */
static NTSTATUS
Ext2QueryExactName(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PEXT2_FCB Fcb,
                   IN PEXT2_CCB Ccb, IN PUNICODE_STRING Name, IN OUT PEXT2_FILLDIR_CONTEXT fc)
{
    PEXT2_MCB   Found = NULL;
    ULONG       EntrySize = 0;
    NTSTATUS    Status;

    if (Name->Length == 0 || FsRtlDoesNameContainWildCards(Name) ||
        (Name->Length == sizeof(WCHAR) && Name->Buffer[0] == L'.') ||
        (Name->Length == 2 * sizeof(WCHAR) && Name->Buffer[0] == L'.' && Name->Buffer[1] == L'.')) {
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    Status = Ext2LookupFile(IrpContext, Vcb, Name, Fcb->Mcb, &Found, 0);
    if (Status == STATUS_NO_SUCH_FILE || Status == STATUS_OBJECT_NAME_NOT_FOUND ||
        Status == STATUS_OBJECT_NAME_INVALID) {
        /* definitely absent (a name ext2 cannot store is absent too) */
        Ccb->filp.f_pos = EXT3_HTREE_EOF;
        return STATUS_NO_SUCH_FILE;
    }
    if (!NT_SUCCESS(Status) || Found == NULL) {
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    Status = STATUS_MORE_PROCESSING_REQUIRED;
    if (!IsMcbSymLink(Found) && !IsMcbRoot(Found) &&
        !Vcb->bHidingPrefix && !Vcb->bHidingSuffix &&
        NT_SUCCESS(Ext2ProcessEntry(IrpContext, Vcb, Fcb, fc->efc_fi, Found->Inode->i_ino,
                                    fc->efc_buf, 0, fc->efc_size, 0, &Found->ShortName,
                                    &EntrySize, fc->efc_single)) &&
        EntrySize > 0) {
        fc->efc_start = EntrySize;
        fc->efc_prev = 0;
        fc->efc_visible = TRUE;
        Ccb->filp.f_pos = EXT3_HTREE_EOF;   /* nothing more to find */
        Status = STATUS_SUCCESS;
    }
    Ext2DerefMcb(Found);
    return Status;
}

/*
 * A directory without an index, or one whose index is broken: its blocks
 * in disk order from the handle's position, each checked as Linux's
 * ext4_readdir checks it - the block's checksum (ext3_bread), then every
 * entry (ext3_check_dir_entry). A block or an entry that fails is reported
 * and the rest of its block skipped; the walk goes on with the next block.
 * A position in the middle of a block is found again from the block's
 * start: the directory may have changed since the last query.
 */
static NTSTATUS
Ext2QueryInDiskOrder(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb,
                     IN PEXT2_CCB Ccb, IN OUT PEXT2_FILLDIR_CONTEXT fc)
{
    struct inode           *Dir = Mcb->Inode;
    ULONGLONG               Size = (ULONGLONG)Ext4DirSize(Dir);
    ULONGLONG               Pos = (ULONGLONG)Ccb->filp.f_pos;
    ULONGLONG               Base;
    struct buffer_head     *bh;
    struct ext3_dir_entry_2 *de;
    ULONG                   Offset, Len;
    int                     err;

    while (Pos < Size) {

        Base = Pos & ~(ULONGLONG)(BLOCK_SIZE - 1);
        err = 0;
        bh = ext3_bread(IrpContext, Dir, (unsigned long)(Pos >> BLOCK_BITS), &err);
        if (bh == NULL) {
            if (err != -EFSCORRUPTED && err != -EFSBADCRC) {
                return err != 0 ? Ext2WinntError(err) : STATUS_DISK_CORRUPT_ERROR;
            }
            /* refused and reported: on with the next block */
            Pos = Base + BLOCK_SIZE;
            Ccb->filp.f_pos = Pos;
            continue;
        }

        /* the first entry at or after the position; a length that cannot
           be is left for the check below, which reports it */
        for (Offset = 0; Offset < (ULONG)(Pos - Base); Offset += Len) {
            Len = ext3_rec_len_from_disk(((struct ext3_dir_entry_2 *)(bh->b_data + Offset))->rec_len);
            if (Len < EXT3_DIR_REC_LEN(1) || Len > BLOCK_SIZE - Offset) {
                break;
            }
        }

        while (Offset < BLOCK_SIZE && Base + Offset < Size) {
            de = (struct ext3_dir_entry_2 *)(bh->b_data + Offset);
            if (!ext3_check_dir_entry(__FUNCTION__, Dir, de, bh, Offset)) {
                Offset = BLOCK_SIZE;            /* the rest of the block */
                break;
            }
            if (de->inode != 0 &&
                Ext2FillEntry(fc, de->name, de->name_len, (ULONG)(Base + Offset),
                              le32_to_cpu(de->inode), de->file_type) != 0) {
                Ccb->filp.f_pos = Base + Offset;
                brelse(bh);
                return fc->efc_status;
            }
            Offset += ext3_rec_len_from_disk(de->rec_len);
        }
        brelse(bh);

        Pos = Base + Offset;
        Ccb->filp.f_pos = Pos;
    }
    return fc->efc_status;
}

/* the entries from the handle's position: through the index where there is
   one, in disk order otherwise */
static NTSTATUS
Ext2QueryEntries(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb,
                 IN PEXT2_CCB Ccb, IN OUT PEXT2_FILLDIR_CONTEXT fc)
{
    struct inode *Dir = Mcb->Inode;

    if (EXT3_HAS_COMPAT_FEATURE(Dir->i_sb, EXT3_FEATURE_COMPAT_DIR_INDEX) &&
        ((EXT3_I(Dir)->i_flags & EXT3_INDEX_FL) || (Dir->i_size >> BLOCK_BITS) == 1)) {

        int rc = ext3_dx_readdir(&Ccb->filp, Ext2FillEntry, fc);

        if (rc != ERR_BAD_DX_DIR) {
            /* below 0 the walk itself failed (an index block refused, no
               memory); it used to end the listing as "no more files" */
            return rc < 0 && NT_SUCCESS(fc->efc_status) ? Ext2WinntError(rc) : fc->efc_status;
        }
        /* We don't set the inode dirty flag since it's not critical that it
           get flushed back to the disk. */
        EXT3_I(Dir)->i_flags &= ~EXT3_INDEX_FL;
    }

    return Ext2QueryInDiskOrder(IrpContext, Vcb, Mcb, Ccb, fc);
}

/* the answer: what was filled, or why there is nothing */
static NTSTATUS
Ext2QueryResult(IN PEXT2_FILLDIR_CONTEXT fc, IN NTSTATUS Status, IN BOOLEAN FirstQuery)
{
    if (fc->efc_start == 0) {
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        /* the directory has entries, but none matched the search pattern:
           a genuine "no such file"; an empty directory (apps such as CMD
           'dir' and BusyBox crashed on empty folders), or no more entries
           on a continued query: "no more files" */
        return FirstQuery && fc->efc_visible ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
    }

    /* the last entry filled ends the list */
    ((PFILE_DIRECTORY_INFORMATION)(fc->efc_buf + fc->efc_prev))->NextEntryOffset = 0;

    /* what was filled is the answer; an error after it comes again with
       the next query, which starts where this one stopped */
    return Status == STATUS_BUFFER_OVERFLOW ? Status : STATUS_SUCCESS;
}

NTSTATUS
Ext2QueryDirectory (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT          DeviceObject;
    NTSTATUS                Status = STATUS_UNSUCCESSFUL;
    PEXT2_VCB               Vcb;
    PFILE_OBJECT            FileObject;
    PEXT2_FCB               Fcb = NULL;
    PEXT2_MCB               Mcb;
    PEXT2_CCB               Ccb;
    PIRP                    Irp = IrpContext->Irp;
    PIO_STACK_LOCATION      IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PUNICODE_STRING         FileName;
    PUCHAR                  Buffer;
    ULONG                   Length = IrpSp->Parameters.QueryDirectory.Length;
    UCHAR                   Flags = IrpSp->Flags;
    BOOLEAN                 FirstQuery = FALSE;
    BOOLEAN                 FcbResourceAcquired = FALSE;
    EXT2_FILLDIR_CONTEXT    fc = { 0 };

    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    __try {

        DeviceObject = IrpContext->DeviceObject;

        /* This request is not allowed on the main device object */
        if (IsExt2FsDevice(DeviceObject)) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
               (Vcb->Identifier.Size == sizeof(EXT2_VCB)));
        if (!IsMounted(Vcb)) {
            Status = STATUS_VOLUME_DISMOUNTED;
            __leave;
        }
        if (FlagOn(Vcb->Flags, VCB_VOLUME_LOCKED)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        /* nor on volumes */
        FileObject = IrpContext->FileObject;
        Fcb = (PEXT2_FCB) FileObject->FsContext;
        if (Fcb == NULL || Fcb->Identifier.Type != EXT2FCB || Fcb->Mcb == NULL) {
            Fcb = NULL;
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }
        Mcb = Fcb->Mcb;
        ASSERT(!IsMcbSymLink(Mcb));
        if (!IsMcbDirectory(Mcb) || IsFileDeleted(Mcb)) {
            Status = STATUS_NOT_A_DIRECTORY;
            __leave;
        }

        Ccb = (PEXT2_CCB) FileObject->FsContext2;
        ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
               (Ccb->Identifier.Size == sizeof(EXT2_CCB)));

        Buffer = Ext2GetUserBuffer(Irp);
        if (Buffer == NULL) {
            Status = STATUS_INVALID_USER_BUFFER;
            __leave;
        }
        if (!IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT)) {
            Status = STATUS_PENDING;
            __leave;
        }

        ExAcquireResourceSharedLite(&Fcb->MainResource, TRUE);
        FcbResourceAcquired = TRUE;

        FileName = (PUNICODE_STRING)IrpSp->Parameters.QueryDirectory.FileName;
        Status = Ext2QueryPattern(Ccb, FileName, &FirstQuery);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (FlagOn(Flags, SL_INDEX_SPECIFIED)) {
            Ccb->filp.f_pos = IrpSp->Parameters.QueryDirectory.FileIndex;
        } else if (FlagOn(Flags, SL_RESTART_SCAN) || FirstQuery) {
            Ccb->filp.f_pos = 0;
        }

        RtlZeroMemory(Buffer, Length);
        fc.efc_irp = IrpContext;
        fc.efc_buf = Buffer;
        fc.efc_size = Length;
        fc.efc_single = FlagOn(Flags, SL_RETURN_SINGLE_ENTRY) != 0;
        fc.efc_fi = IrpSp->Parameters.QueryDirectory.FileInformationClass;
        fc.efc_status = STATUS_SUCCESS;

        Status = STATUS_MORE_PROCESSING_REQUIRED;
        if (FirstQuery && FileName != NULL && !FlagOn(Flags, SL_INDEX_SPECIFIED)) {
            Status = Ext2QueryExactName(IrpContext, Vcb, Fcb, Ccb, FileName, &fc);
        }
        if (Status == STATUS_MORE_PROCESSING_REQUIRED) {
            DEBUG(DL_CP, ("Ext2QueryDirectory: Dir: %wZ Index=%I64xh Pattern : %wZ.\n",
                          &Mcb->ShortName, (ULONGLONG)Ccb->filp.f_pos, &Ccb->DirectorySearchPattern));
            Status = Ext2QueryEntries(IrpContext, Vcb, Mcb, Ccb, &fc);
        }
        Status = Ext2QueryResult(&fc, Status, FirstQuery);

    } __finally {

        if (FcbResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }
        Ext2FreeFillName(&fc);

        if (!IrpContext->ExceptionInProgress) {
            if (Status == STATUS_PENDING || Status == STATUS_CANT_WAIT) {
                Status = Ext2LockUserBuffer(Irp, Length, IoWriteAccess);
                if (NT_SUCCESS(Status)) {
                    Status = Ext2QueueRequest(IrpContext);
                } else {
                    Ext2CompleteIrpContext(IrpContext, Status);
                }
            } else {
                Irp->IoStatus.Information = NT_ERROR(Status) ? 0 : fc.efc_start;
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }

    return Status;
}
