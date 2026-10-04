/**
 * FileCleanup.c - IRP_MJ_CLEANUP: last handle of a file object, delete-on-close, oplocks, locks.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

NTSTATUS
Ext2Cleanup (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT  DeviceObject;
    NTSTATUS        Status = STATUS_SUCCESS;
    PEXT2_VCB       Vcb = NULL;
    PFILE_OBJECT    FileObject;
    PEXT2_FCB       Fcb = NULL;
    PEXT2_CCB       Ccb = NULL;
    PIRP            Irp = NULL;
    PEXT2_MCB       Mcb = NULL;

    BOOLEAN         VcbResourceAcquired = FALSE;
    BOOLEAN         FcbResourceAcquired = FALSE;
    BOOLEAN         FcbPagingIoResourceAcquired = FALSE;
    BOOLEAN         SymLinkDelete = FALSE;
    BOOLEAN         NameDelete = FALSE;
    BOOLEAN         FileDelete = FALSE;

    __try {

        ASSERT(IrpContext != NULL);
        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        DeviceObject = IrpContext->DeviceObject;
        if (IsExt2FsDevice(DeviceObject))  {
            Status = STATUS_SUCCESS;
            __leave;
        }

        Irp = IrpContext->Irp;
        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        ASSERT(Vcb != NULL);
        ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
               (Vcb->Identifier.Size == sizeof(EXT2_VCB)));

        if (!IsVcbInited(Vcb)) {
            Status = STATUS_SUCCESS;
            __leave;
        }

        FileObject = IrpContext->FileObject;
        Fcb = (PEXT2_FCB) FileObject->FsContext;
        if (!Fcb || (Fcb->Identifier.Type != EXT2VCB &&
                     Fcb->Identifier.Type != EXT2FCB)) {
            Status = STATUS_SUCCESS;
            __leave;
        }
        Mcb = Fcb->Mcb;
        Ccb = (PEXT2_CCB) FileObject->FsContext2;

        if (IsFlagOn(FileObject->Flags, FO_CLEANUP_COMPLETE)) {
            Status = STATUS_SUCCESS;
            __leave;
        }

        if (Fcb->Identifier.Type == EXT2VCB) {

            ExAcquireResourceExclusiveLite(
                    &Vcb->MainResource, TRUE);
            VcbResourceAcquired = TRUE;

            if (FlagOn(Vcb->Flags, VCB_VOLUME_LOCKED) &&
                Vcb->LockFile == FileObject ){

                ClearLongFlag(Vcb->Flags, VCB_VOLUME_LOCKED);
                Vcb->LockFile = NULL;
                Ext2ClearVpbFlag(Vcb->Vpb, VPB_LOCKED);
            }

            if (Ccb) {
                Ext2DerefXcb(&Vcb->OpenHandleCount);
                Ext2DerefXcb(&Vcb->OpenVolumeCount);
            }

            IoRemoveShareAccess(FileObject, &Vcb->ShareAccess);

            Status = STATUS_SUCCESS;
            __leave;
        }

        ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
               (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

        FcbResourceAcquired =
            ExAcquireResourceExclusiveLite(
                &Fcb->MainResource,
                TRUE
            );

        if (IsFlagOn(FileObject->Flags, FO_CLEANUP_COMPLETE)) {
            if (IsFlagOn(FileObject->Flags, FO_FILE_MODIFIED) &&
                    IsFlagOn(Vcb->Flags, VCB_FLOPPY_DISK) &&
                    !IsVcbReadOnly(Vcb) ) {
                Status = Ext2FlushFile(IrpContext, Fcb, Ccb);
            }
            __leave;
        }

        if (Ccb == NULL) {
            Status = STATUS_SUCCESS;
            __leave;
        }

        if (IsDirectory(Fcb)) {
            if (IsFlagOn(Ccb->Flags, CCB_DELETE_ON_CLOSE))  {
                if (Ccb->SymLink) {
                    /* came in through a symlink: the link goes, not the
                       directory it leads to */
                    SetLongFlag(Ccb->SymLink->Flags, MCB_DELETE_PENDING);
                } else {
                    SetLongFlag(Fcb->Flags, FCB_DELETE_PENDING);
                    SetLongFlag(Ext2CcbName(Fcb, Ccb)->Flags, MCB_DELETE_PENDING);
                }

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
                    NULL );
            }

            FsRtlNotifyCleanup(Vcb->NotifySync, &Vcb->NotifyList, Ccb);
        }

        ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
               (Ccb->Identifier.Size == sizeof(EXT2_CCB)));

        Ext2DerefXcb(&Vcb->OpenHandleCount);
        Ext2DerefXcb(&Fcb->OpenHandleCount);

        if (IsFlagOn(FileObject->Flags, FO_FILE_MODIFIED)) {
            Fcb->Mcb->FileAttr |= FILE_ATTRIBUTE_ARCHIVE;
        }

        if (IsDirectory(Fcb)) {

            ext3_release_dir(Fcb->Inode, &Ccb->filp);

            /* a delete asked through a symlink to this directory, by this
               handle (at the open, or by a disposition set on it -
               Ext2SetDispositionInfo marks the link): the link goes, the
               directory stays. The link's flag alone is not enough: a
               handle on the link itself may have set it, and that handle
               removes the link at its own cleanup. */
            if (Ccb->SymLink && IsFlagOn(Ccb->SymLink->Flags, MCB_DELETE_PENDING) &&
                (IsFlagOn(Ccb->Flags, CCB_DELETE_ON_CLOSE) || FileObject->DeletePending)) {
                SymLinkDelete = TRUE;
            }

        } else {

            if ( IsFlagOn(FileObject->Flags, FO_FILE_MODIFIED) &&
                    !IsFlagOn(Ccb->Flags, CCB_LAST_WRITE_UPDATED)) {

                LARGE_INTEGER   SysTime;
                KeQuerySystemTime(&SysTime);

                Ext2SetInodeTime(&SysTime, &Fcb->Inode->i_mtime, &Fcb->Inode->i_mtime_extra);
                Ext2SetInodeTime(&SysTime, &Fcb->Inode->i_atime, &Fcb->Inode->i_atime_extra);
                Fcb->Mcb->Icb->LastAccessTime = Fcb->Mcb->Icb->LastWriteTime = Ext2GetInodeTime(Fcb->Inode->i_atime, Fcb->Inode->i_atime_extra);

                /* fails only once the volume stopped writing, and a cleanup
                   has no one to report to */
                (void)Ext2SaveInode(IrpContext, Vcb, Fcb->Inode);

                Ext2NotifyReportChange(
                    IrpContext,
                    Vcb,
                    Fcb->Mcb,
                    FILE_NOTIFY_CHANGE_ATTRIBUTES |
                    FILE_NOTIFY_CHANGE_LAST_WRITE |
                    FILE_NOTIFY_CHANGE_LAST_ACCESS,
                    FILE_ACTION_MODIFIED );
            }

            FsRtlCheckOplock( &Fcb->Oplock,
                              Irp,
                              IrpContext,
                              NULL,
                              NULL );

            Fcb->Header.IsFastIoPossible = Ext2IsFastIoPossible(Fcb);

            if (!IsFlagOn(FileObject->Flags, FO_CACHE_SUPPORTED)) {
                Fcb->NonCachedOpenCount--;
            }

            if (IsFlagOn(Ccb->Flags, CCB_DELETE_ON_CLOSE))  {
                if (Ccb->SymLink || IsInodeSymLink(Mcb->Inode)) {
                    SymLinkDelete = TRUE;
                } else {
                    /* the name this handle came in through is the one to go */
                    SetLongFlag(Ext2CcbName(Fcb, Ccb)->Flags, MCB_DELETE_PENDING);
                }
            }

            /* a delete disposition set on this handle, which came through
               a symlink, marked the link (Ext2SetDispositionInfo): the link
               goes, the target - this Fcb - stays. Only this handle's own
               request counts (FileObject->DeletePending is per handle): the
               link's flag may come from a handle on the link itself, which
               removes it at its own cleanup, and taking that over here
               skipped the target's pending delete below for good. */
            if (!SymLinkDelete && Ccb->SymLink && FileObject->DeletePending &&
                IsFlagOn(Ccb->SymLink->Flags, MCB_DELETE_PENDING)) {
                SymLinkDelete = TRUE;
            }

            /* A name of a hard-linked file goes with this handle; the file
               itself - with its cache, which the other names still use -
               only with the last handle of its last name. Decided here,
               not when the disposition was set: links may have come or
               gone since. */
            if (!SymLinkDelete &&
                IsFlagOn(Ext2CcbName(Fcb, Ccb)->Flags, MCB_DELETE_PENDING) &&
                !IsFileDeleted(Ext2CcbName(Fcb, Ccb))) {
                if (Ext2IsLastLink(Ext2CcbName(Fcb, Ccb))) {
                    SetLongFlag(Fcb->Flags, FCB_DELETE_PENDING);
                } else {
                    ClearLongFlag(Fcb->Flags, FCB_DELETE_PENDING);
                    NameDelete = TRUE;
                }
            }

            /* Drop any byte range locks this process may have on the file. */

            FsRtlFastUnlockAll(
                &Fcb->FileLockAnchor,
                FileObject,
                IoGetRequestorProcess(Irp),
                NULL  );

            /* If there are no byte range locks owned by other processes on the
               file the fast I/O read/write functions doesn't have to check for
               locks so we set IsFastIoPossible to FastIoIsPossible again. */
            if (!FsRtlGetNextFileLock(&Fcb->FileLockAnchor, TRUE)) {
                if (Fcb->Header.IsFastIoPossible != FastIoIsPossible) {
#if EXT2_DEBUG
                    DEBUG(DL_INF, (": %-16.16s %-31s %wZ\n",
                                   Ext2GetCurrentProcessName(),
                                   "FastIoIsPossible",
                                   &Fcb->Mcb->FullName
                                  ));
#endif

                    Fcb->Header.IsFastIoPossible = FastIoIsPossible;
                }
            }

            if (Fcb->OpenHandleCount == 0 && FlagOn(Fcb->Flags, FCB_ALLOC_IN_CREATE |
                                                                FCB_ALLOC_IN_SETINFO) ){

                if (FlagOn(Fcb->Flags, FCB_ALLOC_IN_SETINFO)) {
                    if (Fcb->Header.ValidDataLength.QuadPart < Fcb->Header.FileSize.QuadPart) {
                        if (!INODE_HAS_EXTENT(Fcb->Inode)) {
                            __try {
                                CcZeroData( FileObject,
                                           &Fcb->Header.ValidDataLength,
                                           &Fcb->Header.AllocationSize,
                                           TRUE);
                            } __except (EXCEPTION_EXECUTE_HANDLER) {
                                /* the tail stays as it was: an I/O error
                                   here is the cache manager's to report */
                            }
                        }
                    }
                }

                if (FlagOn(Fcb->Flags, FCB_ALLOC_IN_CREATE)) {

                    LARGE_INTEGER Size;

                    ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
                    FcbPagingIoResourceAcquired = TRUE;

                    Size.QuadPart = CEILING_ALIGNED(ULONGLONG,
                                                    (ULONGLONG)Fcb->Mcb->Inode->i_size,
                                                    (ULONGLONG)BLOCK_SIZE);
                    if (!IsFlagOn(Fcb->Flags, FCB_DELETE_PENDING)) {

                        /* blocks past the end that stay are legal: Size
                           says how far the release got */
                        (void)Ext2TruncateFile(IrpContext, Vcb, Fcb->Mcb, &Size);
                        Fcb->Header.AllocationSize = Size;
                        Fcb->Header.FileSize.QuadPart = Mcb->Inode->i_size;
                        if (Fcb->Header.ValidDataLength.QuadPart > Fcb->Header.FileSize.QuadPart)
                            Fcb->Header.ValidDataLength.QuadPart = Fcb->Header.FileSize.QuadPart;
                        if (CcIsFileCached(FileObject)) {
                            CcSetFileSizes(FileObject,
                                           (PCC_FILE_SIZES)(&(Fcb->Header.AllocationSize)));
                        }
                    }
                    ClearLongFlag(Fcb->Flags, FCB_ALLOC_IN_CREATE|FCB_ALLOC_IN_WRITE|FCB_ALLOC_IN_SETINFO);
                    ExReleaseResourceLite(&Fcb->PagingIoResource);
                    FcbPagingIoResourceAcquired = FALSE;
                }
            }
        }

        IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);

        if (!IsDirectory(Fcb)) {

            /* Only when the remaining opens are all non-cached does the
               cache have to be written out now, so that they see the data.
               The last close of a file is not that case: its dirty pages
               stay with the cache manager and reach disk by the lazy
               writer, as on NTFS and Linux - a synchronous flush here cost
               a disk write per close (measured: 2 ms per 1 KB file). */
            if ( IsFlagOn(FileObject->Flags, FO_CACHE_SUPPORTED) &&
                    (Fcb->NonCachedOpenCount == Fcb->OpenHandleCount) &&
                    (Fcb->SectionObject.DataSectionObject != NULL)) {

                if (Fcb->NonCachedOpenCount > 0 && !IsVcbReadOnly(Vcb)) {
                    CcFlushCache(&Fcb->SectionObject, NULL, 0, NULL);
                    ClearLongFlag(Fcb->Flags, FCB_FILE_MODIFIED);
                }

                /* purge cache if all remaining openings are non-cached,
                   or if the file is about to be deleted anyway */
                if (Fcb->NonCachedOpenCount > 0 ||
                    IsFlagOn(Fcb->Flags, FCB_DELETE_PENDING)) {
                    if (ExAcquireResourceExclusiveLite(&(Fcb->PagingIoResource), TRUE)) {
                        ExReleaseResourceLite(&(Fcb->PagingIoResource));
                    }

                    /* CcPurge could generate recursive IRP_MJ_CLOSE request */
                    CcPurgeCacheSection( &Fcb->SectionObject,
                                         NULL,
                                         0,
                                         FALSE );
                }
            }

            CcUninitializeCacheMap(FileObject, NULL, NULL);
        }

        /* Two independent decisions. SymLinkDelete: this handle removes the
           link it came through (or the link it was opened on). FileDelete:
           names of this file are due - a hard link this handle unlinks, or
           every pending name once the last handle is gone. Both can hold at
           once: a handle through a link may be the last one of a target
           another handle has asked to delete; choosing one used to leave
           the target delete-pending for good, with its Fcb holding the
           volume (no dismount, sc stop never completed). */
        FileDelete = NameDelete ||
                     (IsFlagOn(Fcb->Flags, FCB_DELETE_PENDING) &&
                      Fcb->OpenHandleCount == 0);

        if (SymLinkDelete || FileDelete) {

            /* Ext2DeleteFile will acquire these lock inside */

            if (FcbResourceAcquired) {
                ExReleaseResourceLite(&Fcb->MainResource);
                FcbResourceAcquired = FALSE;
            }

            /* this file is to be deleted ... */
            if (SymLinkDelete) {

                if (Ccb->SymLink) {
                    Mcb = Ccb->SymLink;
                    FileObject->DeletePending = FALSE;
                } else {
                    Mcb = Ext2CcbName(Fcb, Ccb);
                }

                Status = Ext2DeleteFile(IrpContext, Vcb, Fcb, Mcb);
                if (NT_SUCCESS(Status)) {
                    Ext2NotifyReportChange( IrpContext, Vcb, Mcb,
                                            IsMcbDirectory(Mcb) ?
                                            FILE_NOTIFY_CHANGE_DIR_NAME :
                                            FILE_NOTIFY_CHANGE_FILE_NAME,
                                            FILE_ACTION_REMOVED );
                }
            }

            if (FileDelete) {

                /* every name that asked to go: the one of a plain file,
                   each marked hard link otherwise. Ext2DeleteFile frees
                   the inode with the last name - which, while handles
                   remain, waits for the last of them (cache and all). */
                PEXT2_MCB Name;

                while ((Name = Ext2NextPendingName(Vcb, Fcb->Mcb->Icb)) != NULL) {

                    if (Fcb->OpenHandleCount > 0 && Ext2IsLastLink(Name)) {
                        SetLongFlag(Fcb->Flags, FCB_DELETE_PENDING);
                        Ext2DerefMcb(Name);
                        break;
                    }

                    Status = Ext2DeleteFile(IrpContext, Vcb, Fcb, Name);
                    if (NT_SUCCESS(Status)) {
                        Ext2NotifyReportChange( IrpContext, Vcb, Name,
                                                IsMcbDirectory(Name) ?
                                                FILE_NOTIFY_CHANGE_DIR_NAME :
                                                FILE_NOTIFY_CHANGE_FILE_NAME,
                                                FILE_ACTION_REMOVED );
                    } else {
                        /* not going anywhere (directory not empty ...) */
                        ClearLongFlag(Name->Flags, MCB_DELETE_PENDING);
                    }
                    Ext2DerefMcb(Name);
                }

                /* FCB_DELETE_PENDING stands for "a name of this file is
                   going and it is the last one". With no name left waiting
                   and the file still alive (a delete that failed, a name
                   unmarked meanwhile) the flag is stale: left set, every
                   later open was refused with STATUS_DELETE_PENDING and no
                   cleanup would ever come to clear it. */
                if (!IsInodeDeleted(Fcb)) {
                    PEXT2_MCB Waiting = Ext2NextPendingName(Vcb, Fcb->Mcb->Icb);
                    if (Waiting) {
                        Ext2DerefMcb(Waiting);
                    } else {
                        ClearLongFlag(Fcb->Flags, FCB_DELETE_PENDING);
                    }
                }
            }

            /* re-acquire the main resource lock */

            FcbResourceAcquired =
                ExAcquireResourceExclusiveLite(
                    &Fcb->MainResource,
                    TRUE
                );
            if (FileDelete) {
                SetFlag(FileObject->Flags, FO_FILE_MODIFIED);
                if (CcIsFileCached(FileObject)) {
                    CcSetFileSizes(FileObject,
                                   (PCC_FILE_SIZES)(&(Fcb->Header.AllocationSize)));
                }
            }
        }

        DEBUG(DL_INF, ( "Ext2Cleanup: OpenCount=%u ReferCount=%u NonCachedCount=%xh %wZ\n",
                        Fcb->OpenHandleCount, Fcb->ReferenceCount, Fcb->NonCachedOpenCount, &Fcb->Mcb->FullName));

        Status = STATUS_SUCCESS;

        if (FileObject) {
            SetFlag(FileObject->Flags, FO_CLEANUP_COMPLETE);
        }

    } __finally {

        if (FcbPagingIoResourceAcquired) {
            ExReleaseResourceLite(&Fcb->PagingIoResource);
        }

        if (FcbResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (VcbResourceAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }

        if (!IrpContext->ExceptionInProgress) {
            if (Status == STATUS_PENDING) {
                Ext2QueueRequest(IrpContext);
            } else {
                IrpContext->Irp->IoStatus.Status = Status;
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }

    /* the volume may have lost its last handle */
    Ext2UnloadKick();

    return Status;
}
