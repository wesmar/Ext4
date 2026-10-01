/**
 * dirctl_query.c - IRP_MN_QUERY_DIRECTORY: directory enumeration.
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

NTSTATUS
Ext2QueryDirectory (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT          DeviceObject;
    NTSTATUS                Status = STATUS_UNSUCCESSFUL;
    PEXT2_VCB               Vcb = NULL;
    PFILE_OBJECT            FileObject = NULL;
    PEXT2_FCB               Fcb = NULL;
    PEXT2_MCB               Mcb = NULL;
    PEXT2_CCB               Ccb = NULL;
    PIRP                    Irp = NULL;
    PIO_STACK_LOCATION      IoStackLocation = NULL;

    ULONG Length = 0;
    ULONG                   FileIndex;
    PUNICODE_STRING         FileName;
    PUCHAR                  Buffer;

    BOOLEAN                 RestartScan;
    BOOLEAN                 ReturnSingleEntry;
    BOOLEAN                 IndexSpecified;
    BOOLEAN                 FirstQuery;
    BOOLEAN                 FcbResourceAcquired = FALSE;
    BOOLEAN                 DirHasEntries = FALSE;
    PUNICODE_STRING         ExactName = NULL;   /* the caller's spelling, first query */

    USHORT                  NameLen;
    FILE_INFORMATION_CLASS  fi;

    OEM_STRING              Oem = { 0 };
    UNICODE_STRING          Unicode = { 0 };
    PEXT2_DIR_ENTRY2        pDir = NULL;

    ULONG                   ByteOffset;
    ULONG                   RecLen = 0;
    ULONG                   EntrySize = 0;

    EXT2_FILLDIR_CONTEXT    fc = { 0 };

    __try {

        ASSERT(IrpContext);
        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        DeviceObject = IrpContext->DeviceObject;

        //
        // This request is not allowed on the main device object
        //
        if (IsExt2FsDevice(DeviceObject)) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        ASSERT(Vcb != NULL);
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

        FileObject = IrpContext->FileObject;
        Fcb = (PEXT2_FCB) FileObject->FsContext;
        if (Fcb == NULL) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }
        Mcb = Fcb->Mcb;
        if (NULL == Mcb) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }
        ASSERT (!IsMcbSymLink(Mcb));

        //
        // This request is not allowed on volumes
        //
        if (Fcb->Identifier.Type == EXT2VCB) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
               (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

        if (!IsMcbDirectory(Mcb)) {
            Status = STATUS_NOT_A_DIRECTORY;
            __leave;
        }

        if (IsFileDeleted(Mcb)) {
            Status = STATUS_NOT_A_DIRECTORY;
            __leave;
        }

        Ccb = (PEXT2_CCB) FileObject->FsContext2;

        ASSERT(Ccb);
        ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
               (Ccb->Identifier.Size == sizeof(EXT2_CCB)));

        Irp = IrpContext->Irp;
        IoStackLocation = IoGetCurrentIrpStackLocation(Irp);


        fi = IoStackLocation->Parameters.QueryDirectory.FileInformationClass;

        Length = IoStackLocation->Parameters.QueryDirectory.Length;

        FileName = (PUNICODE_STRING)IoStackLocation->Parameters.QueryDirectory.FileName;

        FileIndex = IoStackLocation->Parameters.QueryDirectory.FileIndex;


        RestartScan = FlagOn(((PEXTENDED_IO_STACK_LOCATION)
                              IoStackLocation)->Flags, SL_RESTART_SCAN);
        ReturnSingleEntry = FlagOn(((PEXTENDED_IO_STACK_LOCATION)
                                    IoStackLocation)->Flags, SL_RETURN_SINGLE_ENTRY);
        IndexSpecified = FlagOn(((PEXTENDED_IO_STACK_LOCATION)
                                 IoStackLocation)->Flags, SL_INDEX_SPECIFIED);

        Buffer = Ext2GetUserBuffer(Irp);
        if (Buffer == NULL) {
            DbgBreak();
            Status = STATUS_INVALID_USER_BUFFER;
            __leave;
        }

        if (!IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT)) {
            Status = STATUS_PENDING;
            __leave;
        }

        if (!ExAcquireResourceSharedLite(
                    &Fcb->MainResource,
                    IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT) )) {
            Status = STATUS_PENDING;
            __leave;
        }
        FcbResourceAcquired = TRUE;

        if (FileName != NULL) {

            if (Ccb->DirectorySearchPattern.Buffer != NULL) {

                FirstQuery = FALSE;

            } else {

                FirstQuery = TRUE;
                ExactName = FileName;

                Ccb->DirectorySearchPattern.Length =
                    Ccb->DirectorySearchPattern.MaximumLength =
                        FileName->Length;

                Ccb->DirectorySearchPattern.Buffer =
                    Ext2AllocatePool(PagedPool, FileName->Length,
                                     EXT2_DIRSP_MAGIC);

                if (Ccb->DirectorySearchPattern.Buffer == NULL) {
                    DEBUG(DL_ERR, ( "Ex2QueryDirectory: failed to allocate SerarchPattern.\n"));
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    __leave;
                }

                INC_MEM_COUNT( PS_DIR_PATTERN,
                               Ccb->DirectorySearchPattern.Buffer,
                               Ccb->DirectorySearchPattern.MaximumLength);

                Status = RtlUpcaseUnicodeString(
                             &(Ccb->DirectorySearchPattern),
                             FileName,
                             FALSE);

                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

        } else if (Ccb->DirectorySearchPattern.Buffer != NULL) {

            FirstQuery = FALSE;
            FileName = &Ccb->DirectorySearchPattern;

        } else {

            FirstQuery = TRUE;

            Ccb->DirectorySearchPattern.Length =
                Ccb->DirectorySearchPattern.MaximumLength = 2;

            Ccb->DirectorySearchPattern.Buffer =
                Ext2AllocatePool(PagedPool, 4, EXT2_DIRSP_MAGIC);

            if (Ccb->DirectorySearchPattern.Buffer == NULL) {
                DEBUG(DL_ERR, ( "Ex2QueryDirectory: failed to allocate SerarchPattern (1st).\n"));
                Status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }

            INC_MEM_COUNT( PS_DIR_PATTERN,
                           Ccb->DirectorySearchPattern.Buffer,
                           Ccb->DirectorySearchPattern.MaximumLength);

            RtlZeroMemory(Ccb->DirectorySearchPattern.Buffer, 4);
            RtlCopyMemory(
                Ccb->DirectorySearchPattern.Buffer,
                L"*\0", 2);
        }

        if (IndexSpecified) {
            Ccb->filp.f_pos = FileIndex;
        } else {
            if (RestartScan || FirstQuery) {
                Ccb->filp.f_pos = FileIndex = 0;
            } else {
                FileIndex = (ULONG)Ccb->filp.f_pos;
            }
        }

        RtlZeroMemory(Buffer, Length);

        fc.efc_irp = IrpContext;
        fc.efc_buf = Buffer;
        fc.efc_size = Length;
        fc.efc_start = 0;
        fc.efc_single = ReturnSingleEntry;
        fc.efc_fi = fi;
        fc.efc_status = STATUS_SUCCESS;

        /*
         * A query for one exact name (no wildcards) is a lookup, not a
         * scan: the htree finds it in O(log n), and a miss costs one pass
         * over the directory blocks (ext3_find_entry falls back to the
         * case-insensitive scan). Callers do this all the time - Test-Path,
         * Get-Item, and the filter manager, which normalizes the name of
         * every file being created by asking its directory for it. That
         * last case is a miss by definition, and it must not fall through
         * to the general scan below: that one builds the whole directory
         * in memory and pattern-matches every entry, which made creating a
         * file cost O(n) in the size of its directory.
         *
         * Either way the query is complete: a continuation must report
         * "no more files", so the position is set to the end - the htree
         * end mark, which the linear path also treats as past i_size.
         * The general path remains for symlinks and hiding patterns.
         */
        if (ExactName != NULL && FirstQuery && !IndexSpecified &&
            ExactName->Length > 0 &&
            !FsRtlDoesNameContainWildCards(ExactName) &&
            !(ExactName->Length == 2 && ExactName->Buffer[0] == L'.') &&
            !(ExactName->Length == 4 && ExactName->Buffer[0] == L'.' && ExactName->Buffer[1] == L'.')) {

            PEXT2_MCB   Found = NULL;
            ULONG       ExactEntrySize = 0;
            NTSTATUS    st;

            st = Ext2LookupFile(IrpContext, Vcb, ExactName, Mcb, &Found, 0);
            if (NT_SUCCESS(st) && Found != NULL) {
                /* symlinks and volumes with hiding patterns take the
                   general path, which knows how to present them */
                if (!IsMcbSymLink(Found) && !IsMcbRoot(Found) &&
                    !Vcb->bHidingPrefix && !Vcb->bHidingSuffix) {
                    st = Ext2ProcessEntry(IrpContext, Vcb, Fcb, fi,
                                          Found->Inode->i_ino, Buffer, 0, Length,
                                          0, &Found->ShortName, &ExactEntrySize,
                                          ReturnSingleEntry);
                }
                Ext2DerefMcb(Found);
                if (NT_SUCCESS(st) && ExactEntrySize > 0) {
                    fc.efc_start = ExactEntrySize;
                    fc.efc_prev = 0;
                    Ccb->filp.f_pos = EXT3_HTREE_EOF;    /* nothing more to find */
                    DirHasEntries = TRUE;
                    Status = STATUS_SUCCESS;
                    goto errorout;
                }
            } else if (st == STATUS_NO_SUCH_FILE ||
                       st == STATUS_OBJECT_NAME_NOT_FOUND ||
                       st == STATUS_OBJECT_NAME_INVALID) {
                /* definitely absent (a name ext2 cannot store is absent
                   too): the answer is "no such file", no scan needed */
                Ccb->filp.f_pos = EXT3_HTREE_EOF;
                Status = STATUS_NO_SUCH_FILE;
                __leave;
            }
            /* a case that needs the general path */
        }


        if (EXT3_HAS_COMPAT_FEATURE(Mcb->Inode->i_sb,
                                    EXT3_FEATURE_COMPAT_DIR_INDEX) &&
                ((EXT3_I(Mcb->Inode)->i_flags & EXT3_INDEX_FL) ||
                 ((Mcb->Inode->i_size >> BLOCK_BITS) == 1)) ) {
            int rc = ext3_dx_readdir(&Ccb->filp, Ext2FillEntry, &fc);
            Status = fc.efc_status;
            if (rc != ERR_BAD_DX_DIR) {
                goto errorout;
            }
            /*
             * We don't set the inode dirty flag since it's not
             * critical that it get flushed back to the disk.
             */
            EXT3_I(Mcb->Inode)->i_flags &= ~EXT3_INDEX_FL;
        }

        if (Ext4DirSize(Mcb->Inode) <= Ccb->filp.f_pos) {
            Status = STATUS_NO_MORE_FILES;
            __leave;
        }

        pDir = Ext2AllocatePool(
                   PagedPool,
                   sizeof(EXT2_DIR_ENTRY2),
                   EXT2_DENTRY_MAGIC
               );

        if (!pDir) {
            DEBUG(DL_ERR, ( "Ex2QueryDirectory: failed to allocate pDir.\n"));
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        INC_MEM_COUNT(PS_DIR_ENTRY, pDir, sizeof(EXT2_DIR_ENTRY2));
        ByteOffset = FileIndex;

        DEBUG(DL_CP, ("Ex2QueryDirectory: Dir: %wZ Index=%xh Pattern : %wZ.\n",
                      &Fcb->Mcb->FullName, FileIndex, &Ccb->DirectorySearchPattern));

        while ((ByteOffset < Ext4DirSize(Mcb->Inode)) &&
                (CEILING_ALIGNED(ULONG, fc.efc_start, 8) < Length)) {

            RtlZeroMemory(pDir, sizeof(EXT2_DIR_ENTRY2));

            Status = Ext2ReadInode(
                         IrpContext,
                         Vcb,
                         Mcb,
                         (ULONGLONG)ByteOffset,
                         (PVOID)pDir,
                         sizeof(EXT2_DIR_ENTRY2),
                         FALSE,
                         &EntrySize);

            if (!NT_SUCCESS(Status)) {
                DbgBreak();
                __leave;
            }

            if (pDir->rec_len == 0) {
                RecLen = BLOCK_SIZE - (ByteOffset & (BLOCK_SIZE - 1));
            } else {
                RecLen = ext3_rec_len_from_disk(pDir->rec_len);
            }

            if (!pDir->inode || pDir->inode >= INODES_COUNT) {
                goto ProcessNextEntry;
            }

            Oem.Buffer = pDir->name;
            Oem.Length = (pDir->name_len & 0xff);
            Oem.MaximumLength = Oem.Length;

            /* "." and ".." are reported the way NTFS and FAT report them:
               for every directory but the root. A first "*" query then
               always finds something, which Win32 callers rely on -
               skipping them made FindFirstFile on an empty directory fail
               with ERROR_NO_MORE_FILES, and .NET's Directory.Delete with it. */
            if ((pDir->name_len == 1 && pDir->name[0] == '.') ||
                    (pDir->name_len == 2 && pDir->name[0] == '.' && pDir->name[1] == '.' )) {
                if (IsRoot(Fcb)) {
                    goto ProcessNextEntry;
                }
            } else if (Ext2IsWearingCloak(Vcb, &Oem)) {
                goto ProcessNextEntry;
            }

            /* A real, visible entry exists in this directory (whether or not it
               matches the search pattern). Used to tell an empty directory
               apart from a pattern that simply matched nothing. */
            DirHasEntries = TRUE;

            NameLen = (USHORT) Ext2OEMToUnicodeSize(Vcb, &Oem);

            if (NameLen <= 0) {
                DEBUG(DL_CP, ("Ext2QueryDirectory: failed to count unicode length for inode: %xh\n",
                              pDir->inode));
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }

            if ( Unicode.Buffer != NULL && Unicode.MaximumLength > NameLen) {
                /* reuse buffer */
            } else {
                /* free and re-allocate it */
                if (Unicode.Buffer) {
                    DEC_MEM_COUNT(PS_INODE_NAME,
                                  Unicode.Buffer,
                                  Unicode.MaximumLength);
                    Ext2FreePool(Unicode.Buffer, EXT2_INAME_MAGIC);
                }
                Unicode.MaximumLength = NameLen + 2;
                Unicode.Buffer = Ext2AllocatePool(
                                     PagedPool, Unicode.MaximumLength,
                                     EXT2_INAME_MAGIC
                                 );
                if (!Unicode.Buffer) {
                    DEBUG(DL_ERR, ( "Ex2QueryDirectory: failed to "
                                    "allocate InodeFileName.\n"));
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    __leave;
                }
                INC_MEM_COUNT(PS_INODE_NAME, Unicode.Buffer, Unicode.MaximumLength);
            }

            Unicode.Length = 0;
            RtlZeroMemory(Unicode.Buffer, Unicode.MaximumLength);

            Status = Ext2OEMToUnicode(Vcb, &Unicode, &Oem);
            if (!NT_SUCCESS(Status)) {
                DEBUG(DL_ERR, ( "Ex2QueryDirectory: Ext2OEMtoUnicode failed with %xh.\n", Status));
                Status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }

            DEBUG(DL_CP, ( "Ex2QueryDirectory: process inode: %xh / %wZ (%d).\n",
                           pDir->inode, &Unicode, Unicode.Length));

            if (FsRtlDoesNameContainWildCards(
                        &(Ccb->DirectorySearchPattern)) ?
                    FsRtlIsNameInExpression(
                        &(Ccb->DirectorySearchPattern),
                        &Unicode,
                        TRUE,
                        NULL) :
                    !RtlCompareUnicodeString(
                        &(Ccb->DirectorySearchPattern),
                        &Unicode,
                        TRUE)           ) {

                Status = Ext2ProcessEntry(
                             IrpContext,
                             Vcb,
                             Fcb,
                             fi,
                             pDir->inode,
                             Buffer,
                             CEILING_ALIGNED(ULONG, fc.efc_start, 8),
                             Length - CEILING_ALIGNED(ULONG, fc.efc_start, 8),
                             ByteOffset,
                             &Unicode,
                             &EntrySize,
                             ReturnSingleEntry
                         );

                if (NT_SUCCESS(Status)) {
                    if (EntrySize > 0) {
                        fc.efc_prev  = CEILING_ALIGNED(ULONG, fc.efc_start, 8);
                        fc.efc_start = fc.efc_prev + EntrySize;
                    } else {
                        DbgBreak();
                    }
                } else {
                    if (Status == STATUS_BUFFER_OVERFLOW) {
                        if (fc.efc_start == 0) {
                            fc.efc_start = EntrySize;
                        } else {
                            Status = STATUS_SUCCESS;
                        }
                    } else {
                        __leave;
                    }
                    break;
                }
            }

ProcessNextEntry:

            ByteOffset += RecLen;
            Ccb->filp.f_pos = ByteOffset;

            if (fc.efc_start && ReturnSingleEntry) {
                Status = STATUS_SUCCESS;
                goto errorout;
            }
        }

errorout:

        ((PULONG)((PUCHAR)Buffer + fc.efc_prev))[0] = 0;

        if (Status == STATUS_BUFFER_OVERFLOW) {
            /* just return fc.efc_start/EntrySize bytes that we filled */
        } else if (!fc.efc_start) {
            if (NT_SUCCESS(Status)) {
                if (FirstQuery && DirHasEntries) {
                    /* the directory has entries, but none matched the search
                       pattern: this is a genuine "no such file" */
                    Status = STATUS_NO_SUCH_FILE;
                } else {
                    /* an empty directory (fixes issue #90, where apps such as
                       CMD 'dir' and BusyBox crashed on empty folders), or no
                       more entries on a continued query */
                    Status = STATUS_NO_MORE_FILES;
                }
            }
        } else {
            Status = STATUS_SUCCESS;
        }

    } __finally {

        if (FcbResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (pDir != NULL) {
            Ext2FreePool(pDir, EXT2_DENTRY_MAGIC);
            DEC_MEM_COUNT(PS_DIR_ENTRY, pDir, sizeof(EXT2_DIR_ENTRY2));
        }

        if (Unicode.Buffer != NULL) {
            DEC_MEM_COUNT(PS_INODE_NAME, Unicode.Buffer, Unicode.MaximumLength);
            Ext2FreePool(Unicode.Buffer, EXT2_INAME_MAGIC);
        }

        if (!IrpContext->ExceptionInProgress) {

            if ( Status == STATUS_PENDING ||
                    Status == STATUS_CANT_WAIT) {

                Status = Ext2LockUserBuffer(
                             IrpContext->Irp,
                             Length,
                             IoWriteAccess );

                if (NT_SUCCESS(Status)) {
                    Status = Ext2QueueRequest(IrpContext);
                } else {
                    Ext2CompleteIrpContext(IrpContext, Status);
                }
            } else {
                IrpContext->Irp->IoStatus.Information = fc.efc_start;
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }

    return Status;
}
