/**
 * fileinfo_set.c - IRP_MJ_SET_INFORMATION: times, attributes, sizes, disposition.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "linux\ext4.h"
#include "linux\ext4_xattr.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2SetFileInformation)
#pragma alloc_text(PAGE, Ext2ExpandFile)
#pragma alloc_text(PAGE, Ext2TruncateFile)
#pragma alloc_text(PAGE, Ext2SetDispositionInfo)
#endif

NTSTATUS
Ext2SetFileInformation (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT          DeviceObject;
    NTSTATUS                Status = STATUS_UNSUCCESSFUL;
    PEXT2_VCB               Vcb = NULL;
    PFILE_OBJECT            FileObject = NULL;
    PEXT2_FCB               Fcb = NULL;
    PEXT2_CCB               Ccb = NULL;
    PEXT2_MCB               Mcb = NULL;
    PIRP                    Irp = NULL;
    PIO_STACK_LOCATION      IoStackLocation = NULL;
    FILE_INFORMATION_CLASS  FileInformationClass;

    ULONG                   NotifyFilter = 0;

    ULONG                   Length;
    PVOID                   Buffer;

    BOOLEAN                 FcbMainResourceAcquired = FALSE;
    BOOLEAN                 FcbPagingIoResourceAcquired = FALSE;
    BOOLEAN                 VcbMainResourceAcquired = FALSE;

    __try {

        ASSERT(IrpContext != NULL);

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

        /* check io stack location of irp stack */
        Irp = IrpContext->Irp;
        IoStackLocation = IoGetCurrentIrpStackLocation(Irp);
        FileInformationClass =
            IoStackLocation->Parameters.SetFile.FileInformationClass;
        Length = IoStackLocation->Parameters.SetFile.Length;
        Buffer = Irp->AssociatedIrp.SystemBuffer;

        /* check Vcb */
        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        ASSERT(Vcb != NULL);
        ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
               (Vcb->Identifier.Size == sizeof(EXT2_VCB)));
        if (!IsMounted(Vcb)) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        if (FlagOn(Vcb->Flags, VCB_VOLUME_LOCKED)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        FileObject = IrpContext->FileObject;
        Fcb = (PEXT2_FCB) FileObject->FsContext;

        // This request is issued to volumes, just return success
        if (Fcb == NULL || Fcb->Identifier.Type == EXT2VCB) {
            Status = STATUS_SUCCESS;
            __leave;
        }
        ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
               (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

        if (IsInodeDeleted(Fcb)) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        Ccb = (PEXT2_CCB) FileObject->FsContext2;
        ASSERT(Ccb != NULL);
        ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
               (Ccb->Identifier.Size == sizeof(EXT2_CCB)));
        /* the name this handle was opened through may be gone while the
           file lives on through another hard link: the file itself can
           still be changed, the name cannot */
        Mcb = Ext2CcbName(Fcb, Ccb);
        if (IsFlagOn(Mcb->Flags, MCB_FILE_DELETED)) {
            if (Ccb->SymLink ||
                FileInformationClass == FileDispositionInformation ||
                FileInformationClass == FileRenameInformation ||
                FileInformationClass == FileLinkInformation) {
                Status = STATUS_FILE_DELETED;
                __leave;
            }
            Mcb = Fcb->Mcb;
        }

        if (FileInformationClass != FilePositionInformation) {
            if (IsVcbReadOnly(Vcb)) {
                Status = STATUS_MEDIA_WRITE_PROTECTED;
                __leave;
            }
            if (!Ext2CheckFileAccess(Vcb, Mcb, Ext2FileCanWrite)) {
                Status = STATUS_ACCESS_DENIED;
                __leave;
            }
        }

        if ( !IsDirectory(Fcb) && !FlagOn(Fcb->Flags, FCB_PAGE_FILE) &&
                ((FileInformationClass == FileEndOfFileInformation) ||
                 (FileInformationClass == FileValidDataLengthInformation) ||
                 (FileInformationClass == FileAllocationInformation))) {

            Status = FsRtlCheckOplock( &Fcb->Oplock,
                                       Irp,
                                       IrpContext,
                                       NULL,
                                       NULL );

            if (Status != STATUS_SUCCESS) {
                __leave;
            }

            //
            //  Set the flag indicating if Fast I/O is possible
            //

            Fcb->Header.IsFastIoPossible = Ext2IsFastIoPossible(Fcb);
        }

        /* A rename or a new hard link changes the namespace: directory
           entries, the name cache tree and the name buffers of the node
           that moves (Ext2BuildName reallocates them). Every namespace
           change runs under the exclusive volume resource - create of a
           new name and delete do the same - while plain opens, which read
           that namespace, hold it shared. Taken first, before any Fcb or
           Dcb resource, in the order create and delete use:
           Vcb -> FcbLock -> Dcb/Fcb. */
        if (FileInformationClass == FileRenameInformation ||
            FileInformationClass == FileLinkInformation) {

            if (!ExAcquireResourceExclusiveLite(
                        &Vcb->MainResource,
                        IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT) )) {
                Status = STATUS_PENDING;
                __leave;
            }
            VcbMainResourceAcquired = TRUE;
        }

        /* for renaming or set link, we must not grab any Fcb locks,
           and later we will get Dcb or Fcb resources exclusively.  */
        if (!IsFlagOn(Fcb->Flags, FCB_PAGE_FILE) &&
            FileInformationClass != FileRenameInformation &&
            FileInformationClass != FileLinkInformation) {

            if (!ExAcquireResourceExclusiveLite(
                        &Fcb->MainResource,
                        IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT) )) {
                Status = STATUS_PENDING;
                __leave;
            }

            FcbMainResourceAcquired = TRUE;

            if ( FileInformationClass == FileAllocationInformation ||
                 FileInformationClass == FileEndOfFileInformation ||
                 FileInformationClass == FileValidDataLengthInformation) {

                if (!ExAcquireResourceExclusiveLite(
                            &Fcb->PagingIoResource,
                            IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT) )) {
                    Status = STATUS_PENDING;
                    DbgBreak();
                    __leave;
                }
                FcbPagingIoResourceAcquired = TRUE;
            }
        }

        switch (FileInformationClass) {

        case FileBasicInformation:
        {
            PFILE_BASIC_INFORMATION FBI = (PFILE_BASIC_INFORMATION) Buffer;
            struct inode *Inode = Mcb->Inode;

            if (FBI->CreationTime.QuadPart != 0 && FBI->CreationTime.QuadPart != -1) {
                Ext2SetInodeTime(&FBI->CreationTime, &Inode->i_crtime, &Inode->i_crtime_extra);
                Mcb->Icb->CreationTime = Ext2GetInodeTime(Inode->i_crtime, Inode->i_crtime_extra);
                NotifyFilter |= FILE_NOTIFY_CHANGE_CREATION;
            }

            if (FBI->LastAccessTime.QuadPart != 0 && FBI->LastAccessTime.QuadPart != -1) {
                Ext2SetInodeTime(&FBI->LastAccessTime, &Inode->i_atime, &Inode->i_atime_extra);
                Mcb->Icb->LastAccessTime = Ext2GetInodeTime(Inode->i_atime, Inode->i_atime_extra);
                NotifyFilter |= FILE_NOTIFY_CHANGE_LAST_ACCESS;
            }

            if (FBI->LastWriteTime.QuadPart != 0 && FBI->LastWriteTime.QuadPart != -1) {
                Ext2SetInodeTime(&FBI->LastWriteTime, &Inode->i_mtime, &Inode->i_mtime_extra);
                Mcb->Icb->LastWriteTime = Ext2GetInodeTime(Inode->i_mtime, Inode->i_mtime_extra);
                NotifyFilter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
                SetLongFlag(Ccb->Flags, CCB_LAST_WRITE_UPDATED);
            }

            if (FBI->ChangeTime.QuadPart !=0 && FBI->ChangeTime.QuadPart != -1) {
                Ext2SetInodeTime(&FBI->ChangeTime, &Inode->i_ctime, &Inode->i_ctime_extra);
                Mcb->Icb->ChangeTime = Ext2GetInodeTime(Inode->i_ctime, Inode->i_ctime_extra);
                NotifyFilter |= FILE_NOTIFY_CHANGE_ATTRIBUTES;
            }

            if (FBI->FileAttributes != 0) {

                BOOLEAN bIsDirectory = IsDirectory(Fcb);
                NotifyFilter |= FILE_NOTIFY_CHANGE_ATTRIBUTES;

                if (IsFlagOn(FBI->FileAttributes, FILE_ATTRIBUTE_READONLY)) {
                    Ext2SetOwnerReadOnly(Inode->i_mode);
                } else {
                    Ext2SetOwnerWritable(Inode->i_mode);
                }

                if (FBI->FileAttributes & FILE_ATTRIBUTE_TEMPORARY) {
                    SetFlag(FileObject->Flags, FO_TEMPORARY_FILE);
                } else {
                    ClearFlag(FileObject->Flags, FO_TEMPORARY_FILE);
                }

                Mcb->FileAttr = FBI->FileAttributes;
                if (bIsDirectory) {
                    SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_DIRECTORY);
                    ClearFlag(Mcb->FileAttr, FILE_ATTRIBUTE_NORMAL);
                }
            }

            if (NotifyFilter != 0) {
                if (Ext2SaveInode(IrpContext, Vcb, Inode)) {
                    Status = STATUS_SUCCESS;
                }
            }

            ClearFlag(NotifyFilter, FILE_NOTIFY_CHANGE_LAST_ACCESS);
            Status = STATUS_SUCCESS;
        }

        break;

        case FileAllocationInformation:
        {
            PFILE_ALLOCATION_INFORMATION FAI = (PFILE_ALLOCATION_INFORMATION)Buffer;
            LARGE_INTEGER  AllocationSize;

            if (IsMcbDirectory(Mcb) || IsMcbSpecialFile(Mcb)) {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                __leave;
            } else {
                Status = STATUS_SUCCESS;
            }

            /* set Mcb to it's target */
            if (IsMcbSymLink(Mcb)) {
                ASSERT(Fcb->Mcb->Icb == Mcb->Target->Icb);
            }
            Mcb = Fcb->Mcb;

            /* get user specified allocationsize aligned with BLOCK_SIZE */
            AllocationSize.QuadPart = CEILING_ALIGNED(ULONGLONG,
                                      (ULONGLONG)FAI->AllocationSize.QuadPart,
                                      (ULONGLONG)BLOCK_SIZE);

            if (AllocationSize.QuadPart > Fcb->Header.AllocationSize.QuadPart) {

                Status = Ext2ExpandFile(IrpContext, Vcb, Mcb, &AllocationSize); 
                Fcb->Header.AllocationSize = AllocationSize;
                NotifyFilter = FILE_NOTIFY_CHANGE_SIZE;
                SetLongFlag(Fcb->Flags, FCB_ALLOC_IN_SETINFO);

            } else if (AllocationSize.QuadPart < Fcb->Header.AllocationSize.QuadPart) {

                if (MmCanFileBeTruncated(&(Fcb->SectionObject), &AllocationSize))  {

                    /* truncate file blocks */
                    Status = Ext2TruncateFile(IrpContext, Vcb, Mcb, &AllocationSize);

                    if (NT_SUCCESS(Status)) {
                        ClearLongFlag(Fcb->Flags, FCB_ALLOC_IN_CREATE);
                    }

                    NotifyFilter = FILE_NOTIFY_CHANGE_SIZE;
                    Fcb->Header.AllocationSize.QuadPart = AllocationSize.QuadPart;
                    if (Mcb->Inode->i_size > (loff_t)AllocationSize.QuadPart) {
                        Mcb->Inode->i_size = AllocationSize.QuadPart;
                    }
                    Fcb->Header.FileSize.QuadPart = Mcb->Inode->i_size;
                    if (Fcb->Header.ValidDataLength.QuadPart > Fcb->Header.FileSize.QuadPart) {
                        Fcb->Header.ValidDataLength.QuadPart = Fcb->Header.FileSize.QuadPart;
                    }

                } else {

                    Status = STATUS_USER_MAPPED_FILE;
                    DbgBreak();
                    __leave;
                }
            }

            if (NotifyFilter) {

                SetFlag(FileObject->Flags, FO_FILE_MODIFIED);
                SetLongFlag(Fcb->Flags, FCB_FILE_MODIFIED);
                Ext2SaveInode(IrpContext, Vcb, Mcb->Inode);
                if (CcIsFileCached(FileObject)) {
                    CcSetFileSizes(FileObject, (PCC_FILE_SIZES)(&(Fcb->Header.AllocationSize)));
                }
            }

            DEBUG(DL_IO, ("Ext2SetInformation: %wZ NewSize=%I64xh AllocationSize=%I64xh "
                          "FileSize=%I64xh VDL=%I64xh i_size=%I64xh status = %xh\n",
                          &Fcb->Mcb->ShortName, AllocationSize.QuadPart,
                          Fcb->Header.AllocationSize.QuadPart,
                          Fcb->Header.FileSize.QuadPart, Fcb->Header.ValidDataLength.QuadPart,
                          Mcb->Inode->i_size, Status));
        }

        break;

        case FileEndOfFileInformation:
        {
            PFILE_END_OF_FILE_INFORMATION FEOFI = (PFILE_END_OF_FILE_INFORMATION) Buffer;
            LARGE_INTEGER NewSize, OldSize, EndOfFile;

            if (IsMcbDirectory(Mcb) || IsMcbSpecialFile(Mcb)) {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                __leave;
            } else {
                Status = STATUS_SUCCESS;
            }

            /* set Mcb to it's target */
            if (IsMcbSymLink(Mcb)) {
                ASSERT(Fcb->Mcb->Icb == Mcb->Target->Icb);
            }
            Mcb = Fcb->Mcb;

            OldSize = Fcb->Header.AllocationSize;
            EndOfFile = FEOFI->EndOfFile;
            if (EndOfFile.QuadPart < 0) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }

            if (IoStackLocation->Parameters.SetFile.AdvanceOnly) {

                if (IsFlagOn(Fcb->Flags, FCB_DELETE_PENDING)) {
                    __leave;
                }

                if (EndOfFile.QuadPart > Fcb->Header.FileSize.QuadPart) {
                    EndOfFile.QuadPart = Fcb->Header.FileSize.QuadPart;
                }

                if (EndOfFile.QuadPart > Fcb->Header.ValidDataLength.QuadPart) {
                    Fcb->Header.ValidDataLength.QuadPart = EndOfFile.QuadPart;
                    NotifyFilter = FILE_NOTIFY_CHANGE_SIZE;
                }

                __leave;
            }

            NewSize.QuadPart = CEILING_ALIGNED(ULONGLONG,
                                               EndOfFile.QuadPart, BLOCK_SIZE);

            if (NewSize.QuadPart > OldSize.QuadPart) {

                Fcb->Header.AllocationSize = NewSize;
                Status = Ext2ExpandFile(
                                 IrpContext,
                                 Vcb,
                                 Mcb,
                                 &(Fcb->Header.AllocationSize)
                             );
                NotifyFilter = FILE_NOTIFY_CHANGE_SIZE;
                SetLongFlag(Fcb->Flags, FCB_ALLOC_IN_SETINFO);


            } else if (NewSize.QuadPart == OldSize.QuadPart) {

                /* same size: nothing to allocate or free */
                Status = STATUS_SUCCESS;

            } else {

                /* don't truncate file data since it's still being written */
                if (IsFlagOn(Fcb->Flags, FCB_ALLOC_IN_WRITE)) {

                    Status = STATUS_SUCCESS;

                } else {

                    if (!MmCanFileBeTruncated(&(Fcb->SectionObject), &NewSize)) {
                        Status = STATUS_USER_MAPPED_FILE;
                        DbgBreak();
                        __leave;
                    }

                    /* truncate file blocks */
                    Status = Ext2TruncateFile(IrpContext, Vcb, Mcb, &NewSize);

                    /* restore original file size */
                    if (NT_SUCCESS(Status)) {
                        ClearLongFlag(Fcb->Flags, FCB_ALLOC_IN_CREATE);
                    }

                    /* the allocation now ends at the new size */
                    Fcb->Header.AllocationSize.QuadPart = NewSize.QuadPart;

                    ASSERT((loff_t)NewSize.QuadPart >= Mcb->Inode->i_size);
                    if ((loff_t)Fcb->Header.FileSize.QuadPart < Mcb->Inode->i_size) {
                        Fcb->Header.FileSize.QuadPart = Mcb->Inode->i_size;
                    }
                    if (Fcb->Header.ValidDataLength.QuadPart > Fcb->Header.FileSize.QuadPart) {
                        Fcb->Header.ValidDataLength.QuadPart = Fcb->Header.FileSize.QuadPart;
                    }

                    SetFlag(FileObject->Flags, FO_FILE_MODIFIED);
                    SetLongFlag(Fcb->Flags, FCB_FILE_MODIFIED);
                }

                NotifyFilter = FILE_NOTIFY_CHANGE_SIZE;
            }

            if (NT_SUCCESS(Status)) {

                Fcb->Header.FileSize.QuadPart = Mcb->Inode->i_size = EndOfFile.QuadPart;
                if (CcIsFileCached(FileObject)) {
                    CcSetFileSizes(FileObject, (PCC_FILE_SIZES)(&(Fcb->Header.AllocationSize)));
                }

                /* ext4 keeps no valid data length on disk, so everything up
                   to the new end must read as zeros from now on. Blocks past
                   the old end are either not allocated or unwritten extents
                   (allocated by Ext2ExpandFile outside a write): both read
                   as zeros, and a write converts exactly the blocks it
                   covers. Only the old last block can carry bytes past the
                   old end, left by an earlier truncation - Linux zeroes them
                   in ext4_block_truncate_page. So an extent-mapped file gets
                   that block tail zeroed, not the whole range; an indirect-
                   mapped file (ext2/ext3) allocates plain blocks and gets
                   all of it zeroed. */
                if (EndOfFile.QuadPart > Fcb->Header.ValidDataLength.QuadPart) {

                    LARGE_INTEGER ZeroEnd = EndOfFile;

                    if (INODE_HAS_EXTENT(Mcb->Inode)) {
                        LONGLONG BlockEnd = CEILING_ALIGNED(LONGLONG,
                                                Fcb->Header.ValidDataLength.QuadPart,
                                                (LONGLONG)BLOCK_SIZE);
                        if (BlockEnd < ZeroEnd.QuadPart) {
                            ZeroEnd.QuadPart = BlockEnd;
                        }
                    }

                    if (ZeroEnd.QuadPart > Fcb->Header.ValidDataLength.QuadPart &&
                        !CcZeroData(FileObject, &Fcb->Header.ValidDataLength,
                                    &ZeroEnd, TRUE)) {
                        Status = STATUS_UNEXPECTED_IO_ERROR;
                        __leave;
                    }
                }
                Fcb->Header.ValidDataLength = EndOfFile;
                if (CcIsFileCached(FileObject)) {
                    CcSetFileSizes(FileObject, (PCC_FILE_SIZES)(&(Fcb->Header.AllocationSize)));
                }

                if (Fcb->Header.FileSize.QuadPart >= 0x80000000 &&
                        !IsFlagOn(SUPER_BLOCK->s_feature_ro_compat, EXT2_FEATURE_RO_COMPAT_LARGE_FILE)) {
                    SetFlag(SUPER_BLOCK->s_feature_ro_compat, EXT2_FEATURE_RO_COMPAT_LARGE_FILE);
                    Ext2SaveSuper(IrpContext, Vcb);
                }

                SetFlag(FileObject->Flags, FO_FILE_MODIFIED);
                SetLongFlag(Fcb->Flags, FCB_FILE_MODIFIED);
                NotifyFilter = FILE_NOTIFY_CHANGE_SIZE;
            }


            if (!Ext2SaveInode(IrpContext, Vcb, Mcb->Inode) && NT_SUCCESS(Status))
                Status = STATUS_UNEXPECTED_IO_ERROR;

            DEBUG(DL_IO, ("Ext2SetInformation: FileEndOfFileInformation %wZ EndofFile=%I64xh "
                          "AllocatieonSize=%I64xh FileSize=%I64xh VDL=%I64xh i_size=%I64xh status = %xh\n",
                          &Fcb->Mcb->ShortName, EndOfFile.QuadPart, Fcb->Header.AllocationSize.QuadPart,
                          Fcb->Header.FileSize.QuadPart, Fcb->Header.ValidDataLength.QuadPart,
                          Mcb->Inode->i_size, Status));
        }

        break;

        case FileValidDataLengthInformation:
        {
            PFILE_VALID_DATA_LENGTH_INFORMATION FVDL = (PFILE_VALID_DATA_LENGTH_INFORMATION) Buffer;
            LARGE_INTEGER NewVDL;

            if (IsMcbDirectory(Mcb) || IsMcbSpecialFile(Mcb)) {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                __leave;
            } else {
                Status = STATUS_SUCCESS;
            }

            NewVDL = FVDL->ValidDataLength;
            if ((NewVDL.QuadPart < Fcb->Header.ValidDataLength.QuadPart)) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            if (NewVDL.QuadPart > Fcb->Header.FileSize.QuadPart)
                NewVDL = Fcb->Header.FileSize;

            if (!MmCanFileBeTruncated(FileObject->SectionObjectPointer,
                                      &NewVDL)) {
                Status = STATUS_USER_MAPPED_FILE;
                __leave;
            }

            Fcb->Header.ValidDataLength = NewVDL;
            FileObject->Flags |= FO_FILE_MODIFIED;
            if (CcIsFileCached(FileObject)) {
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)(&(Fcb->Header.AllocationSize)));
            }
        }

        break;

        case FileDispositionInformation:
        {
            PFILE_DISPOSITION_INFORMATION FDI = (PFILE_DISPOSITION_INFORMATION)Buffer;

            Status = Ext2SetDispositionInfo(IrpContext, Vcb, Fcb, Ccb, FDI->DeleteFile);

            DEBUG(DL_INF, ( "Ext2SetInformation: SetDispositionInformation: DeleteFile=%d %wZ status = %xh\n",
                            FDI->DeleteFile, &Mcb->ShortName, Status));
        }

        break;

        case FileRenameInformation:
        {
            Status = Ext2SetRenameInfo(IrpContext, Vcb, Fcb, Ccb);
        }

        break;


        case FileLinkInformation:
        {
            Status = Ext2SetLinkInfo(IrpContext, Vcb, Fcb, Ccb);
        }

        break;

        //
        // This is the only set file information request supported on read
        // only file systems
        //
        case FilePositionInformation:
        {
            PFILE_POSITION_INFORMATION FilePositionInformation;

            if (Length < sizeof(FILE_POSITION_INFORMATION)) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }

            FilePositionInformation = (PFILE_POSITION_INFORMATION) Buffer;

            if ((FlagOn(FileObject->Flags, FO_NO_INTERMEDIATE_BUFFERING)) &&
                    (FilePositionInformation->CurrentByteOffset.LowPart &
                     DeviceObject->AlignmentRequirement) ) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }

            FileObject->CurrentByteOffset =
                FilePositionInformation->CurrentByteOffset;

            Status = STATUS_SUCCESS;
            __leave;
        }

        break;

        default:
            DEBUG(DL_WRN, ( "Ext2SetInformation: invalid class: %d\n",
                            FileInformationClass));
            Status = STATUS_INVALID_PARAMETER;/* STATUS_INVALID_INFO_CLASS; */
        }

    } __finally {

        if (FcbPagingIoResourceAcquired) {
            ExReleaseResourceLite(&Fcb->PagingIoResource);
        }

        if (NT_SUCCESS(Status) && (NotifyFilter != 0)) {
            Ext2NotifyReportChange(
                IrpContext,
                Vcb,
                Mcb,
                NotifyFilter,
                FILE_ACTION_MODIFIED );

        }

        if (FcbMainResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (VcbMainResourceAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }

        if (!IrpContext->ExceptionInProgress) {
            if (Status == STATUS_PENDING ||
                    Status == STATUS_CANT_WAIT ) {
                DbgBreak();
                Status = Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext,  Status);
            }
        }
    }

    return Status;
}

ULONG
Ext2TotalBlocks(
    PEXT2_VCB         Vcb,
    PLARGE_INTEGER    Size,
    PULONG            pMeta
)
{
    ULONG Blocks, Meta =0, Remain;

    Blocks = (ULONG)((Size->QuadPart + BLOCK_SIZE - 1) >> BLOCK_BITS);
    if (Blocks <= EXT2_NDIR_BLOCKS)
        goto errorout;
    Blocks -= EXT2_NDIR_BLOCKS;

    Meta += 1;
    if (Blocks <= Vcb->max_blocks_per_layer[1]) {
        goto errorout;
    }
    Blocks -= Vcb->max_blocks_per_layer[1];

level2:

    if (Blocks <= Vcb->max_blocks_per_layer[2]) {
        Meta += 1 + ((Blocks + BLOCK_SIZE/4 - 1) >> (BLOCK_BITS - 2));
        goto errorout;
    }
    Meta += 1 + BLOCK_SIZE/4;
    Blocks -= Vcb->max_blocks_per_layer[2];

    if (Blocks > Vcb->max_blocks_per_layer[3]) {
        Blocks  = Vcb->max_blocks_per_layer[3];
    }

    ASSERT(Vcb->max_blocks_per_layer[2]);
    Remain = Blocks % Vcb->max_blocks_per_layer[2];
    Blocks = Blocks / Vcb->max_blocks_per_layer[2];
    Meta += 1 + Blocks * (1 + BLOCK_SIZE/4);
    if (Remain) {
        Blocks = Remain;
        goto level2;
    }

errorout:

    if (pMeta)
        *pMeta = Meta;
    Blocks = (ULONG)((Size->QuadPart + BLOCK_SIZE - 1) >> BLOCK_BITS);
    return (Blocks + Meta);
}

NTSTATUS
Ext2BlockMap(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN ULONG                Index,
    IN BOOLEAN              bAlloc,
    OUT PULONG              pBlock,
    OUT PULONG              Number
)
{
	NTSTATUS status;

	if (INODE_HAS_EXTENT(Mcb->Inode)) {
        status = Ext2MapExtent(IrpContext, Vcb, Mcb, Index,
                               bAlloc, pBlock, Number );
	} else {
        status = Ext2MapIndirect(IrpContext, Vcb, Mcb, Index,
                                 bAlloc, pBlock, Number );
    }

	return status;
}

NTSTATUS
Ext2ExpandFile(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_MCB         Mcb,
    PLARGE_INTEGER    Size
)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG    Start = 0;
    ULONG    End = 0;

    /* inline data goes to a block before the file grows */
    if (Ext4IsInline(Mcb->Inode)) {
        status = Ext4UninlineFile(IrpContext, Vcb, Mcb, TRUE);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    Start = (ULONG)((Mcb->Inode->i_size + BLOCK_SIZE - 1) >> BLOCK_BITS);
    End = (ULONG)((Size->QuadPart + BLOCK_SIZE - 1) >> BLOCK_BITS);

    /* it's a truncate operation, not expanding */
    if (Start >= End) {
        Size->QuadPart = ((LONGLONG) Start) << BLOCK_BITS;
        return STATUS_SUCCESS;
    }

	/* ignore special files */
	if (IsMcbSpecialFile(Mcb)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

	/* expandind file extents */ 
    if (INODE_HAS_EXTENT(Mcb->Inode)) {

        status = Ext2ExpandExtent(IrpContext, Vcb, Mcb, Start, End, Size);

    } else {

        BOOLEAN do_expand;

        do_expand = TRUE;
        if (!do_expand)
            goto errorout;

        status = Ext2ExpandIndirect(IrpContext, Vcb, Mcb, Start, End, Size);
    }

errorout:
    return status;
}

NTSTATUS
Ext2TruncateFile(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_MCB         Mcb,
    PLARGE_INTEGER    Size
)
{
    NTSTATUS status = STATUS_SUCCESS;

    /* inline data: to nothing is only leaving inline; else to a block first */
    if (Ext4IsInline(Mcb->Inode)) {
        status = Ext4UninlineFile(IrpContext, Vcb, Mcb, Size->QuadPart != 0);
        if (!NT_SUCCESS(status) || Size->QuadPart == 0) {
            return status;
        }
    }

    if (INODE_HAS_EXTENT(Mcb->Inode)) {
		status = Ext2TruncateExtent(IrpContext, Vcb, Mcb, Size);
    } else {
		status = Ext2TruncateIndirect(IrpContext, Vcb, Mcb, Size);
	}

    /* check and clear data/meta mcb extents */
    if (Size->QuadPart == 0) {

        /* check and remove all data extents */
        if (Ext2ListExtents(&Mcb->Icb->Extents)) {
            DbgBreak();
        }
        Ext2ClearAllExtents(&Mcb->Icb->Extents);
        /* check and remove all meta extents */
        if (Ext2ListExtents(&Mcb->Icb->MetaExts)) {
            DbgBreak();
        }
        Ext2ClearAllExtents(&Mcb->Icb->MetaExts);
        ClearLongFlag(Mcb->Icb->Flags, ICB_ZONE_INITED);
    }

    return status;
}

NTSTATUS
Ext2IsFileRemovable(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_FCB            Fcb,
    IN PEXT2_CCB            Ccb
)
{
    /* Ext2CreateFile asks before it has a Ccb */
    PEXT2_MCB Mcb = Ccb ? Ext2CcbName(Fcb, Ccb) : Fcb->Mcb;

    if (Mcb->Inode->i_ino == EXT2_ROOT_INO) {
        return STATUS_CANNOT_DELETE;
    }

    /* the read-only attribute (owner write bit clear) protects the file
       from deletion as it does on NTFS; the attribute has to go first */
    if (!Ext2IsOwnerWritable(Mcb->Inode->i_mode)) {
        return STATUS_CANNOT_DELETE;
    }

    /* chattr +i / +a: such a file does not go, nor does any name in such
       a directory */
    if (Ext4IsSealed(Mcb->Inode) ||
        (Mcb->Parent && Mcb->Parent->Inode && Ext4IsSealed(Mcb->Parent->Inode))) {
        return STATUS_CANNOT_DELETE;
    }

    if (IsMcbDirectory(Mcb)) {
        if (!Ext2IsDirectoryEmpty(IrpContext, Vcb, Mcb)) {
            return STATUS_DIRECTORY_NOT_EMPTY;
        }
    }

    if (!MmFlushImageSection(&Fcb->SectionObject,
                             MmFlushForDelete )) {
        return STATUS_CANNOT_DELETE;
    }

    if (IsMcbDirectory(Mcb)) {
        FsRtlNotifyFullChangeDirectory(
            Vcb->NotifySync,
            &Vcb->NotifyList,
            Ccb,
            NULL,
            FALSE,
            FALSE,
            0,
            NULL,
            NULL,
            NULL
        );
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Ext2SetDispositionInfo(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB Vcb,
    PEXT2_FCB Fcb,
    PEXT2_CCB Ccb,
    BOOLEAN bDelete
)
{
    PIRP    Irp = IrpContext->Irp;
    PIO_STACK_LOCATION IrpSp;
    NTSTATUS status = STATUS_SUCCESS;
    PEXT2_MCB  Mcb = Ext2CcbName(Fcb, Ccb);

    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    DEBUG(DL_INF, ( "Ext2SetDispositionInfo: bDelete=%x\n", bDelete));

    /* A handle that came in through a symlink (the open followed it, so
       Fcb is the target's) deletes the link and never the target: mark
       the link only, Ext2Cleanup removes it. Touching Fcb here would put
       the target's delete-pending state on a file that is not going. */
    if (Ccb->SymLink) {
        if (bDelete) {
            SetLongFlag(Ccb->SymLink->Flags, MCB_DELETE_PENDING);
        } else {
            ClearLongFlag(Ccb->SymLink->Flags, MCB_DELETE_PENDING);
        }
        IrpSp->FileObject->DeletePending = bDelete;
        return STATUS_SUCCESS;
    }

    if (bDelete) {

        DEBUG(DL_INF, ( "Ext2SetDispositionInformation: Removing %wZ.\n",
                        &Mcb->FullName));

        if (IsInodeSymLink(Mcb->Inode)) {
            /* the link itself (opened as a reparse point): always allowed */
        } else {
            status = Ext2IsFileRemovable(IrpContext, Vcb, Fcb, Ccb);
        }

        if (NT_SUCCESS(status)) {
            /* the file goes only with its last name; Ext2Cleanup decides
               again when the handle closes */
            if (Ext2IsLastLink(Mcb)) {
                SetLongFlag(Fcb->Flags, FCB_DELETE_PENDING);
            }
            SetLongFlag(Mcb->Flags, MCB_DELETE_PENDING);
            IrpSp->FileObject->DeletePending = TRUE;
        }

    } else {

        ClearLongFlag(Fcb->Flags, FCB_DELETE_PENDING);
        ClearLongFlag(Mcb->Flags, MCB_DELETE_PENDING);
        IrpSp->FileObject->DeletePending = FALSE;
    }

    return status;
}
