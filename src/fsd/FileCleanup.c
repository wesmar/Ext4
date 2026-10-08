/**
 * FileCleanup.c - IRP_MJ_CLEANUP: last handle of a file object, delete-on-close, oplocks, locks.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

/* a volume handle: the lock it may hold, its share of the volume */
static VOID
Ext2CleanupVolume(IN PEXT2_VCB Vcb, IN PFILE_OBJECT FileObject, IN PEXT2_CCB Ccb)
{
    ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);

    if (FlagOn(Vcb->Flags, VCB_VOLUME_LOCKED) && Vcb->LockFile == FileObject) {
        ClearLongFlag(Vcb->Flags, VCB_VOLUME_LOCKED);
        Vcb->LockFile = NULL;
        Ext2ClearVpbFlag(Vcb->Vpb, VPB_LOCKED);
    }

    if (Ccb) {
        Ext2DerefXcb(&Vcb->OpenHandleCount);
        Ext2DerefXcb(&Vcb->OpenVolumeCount);
    }

    IoRemoveShareAccess(FileObject, &Vcb->ShareAccess);

    ExReleaseResourceLite(&Vcb->MainResource);
}

/* a directory handle's delete on close is marked, and its change
   notifications end with it */
static VOID
Ext2CleanupNotify(IN PEXT2_VCB Vcb, IN PEXT2_FCB Fcb, IN PEXT2_CCB Ccb)
{
    if (IsFlagOn(Ccb->Flags, CCB_DELETE_ON_CLOSE)) {
        if (Ccb->SymLink) {
            /* came in through a symlink: the link goes, not the
               directory it leads to */
            SetLongFlag(Ccb->SymLink->Flags, MCB_DELETE_PENDING);
        } else {
            SetLongFlag(Fcb->Flags, FCB_DELETE_PENDING);
            SetLongFlag(Ext2CcbName(Fcb, Ccb)->Flags, MCB_DELETE_PENDING);
        }

        FsRtlNotifyFullChangeDirectory(Vcb->NotifySync, &Vcb->NotifyList, Ccb,
                                       NULL, FALSE, FALSE, 0, NULL, NULL, NULL);
    }

    FsRtlNotifyCleanup(Vcb->NotifySync, &Vcb->NotifyList, Ccb);
}

/* A directory handle: its readdir state goes. TRUE when the link it came
   through goes with it - a delete asked through a symlink to this
   directory, by this handle (at the open, or by a disposition set on it -
   Ext2SetDispositionInfo marks the link): the link goes, the directory
   stays. The link's flag alone is not enough: a handle on the link itself
   may have set it, and that handle removes the link at its own cleanup. */
static BOOLEAN
Ext2CleanupDirectory(IN PFILE_OBJECT FileObject, IN PEXT2_FCB Fcb, IN PEXT2_CCB Ccb)
{
    ext3_release_dir(Fcb->Inode, &Ccb->filp);

    return Ccb->SymLink && IsFlagOn(Ccb->SymLink->Flags, MCB_DELETE_PENDING) &&
           (IsFlagOn(Ccb->Flags, CCB_DELETE_ON_CLOSE) || FileObject->DeletePending);
}

/* a file written through this handle, whose last write time nobody set:
   the time of this cleanup, as Linux takes it at the write */
static VOID
Ext2CleanupWriteTime(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb,
                     IN PFILE_OBJECT FileObject, IN PEXT2_FCB Fcb, IN PEXT2_CCB Ccb)
{
    LARGE_INTEGER   SysTime;

    if (!IsFlagOn(FileObject->Flags, FO_FILE_MODIFIED) ||
        IsFlagOn(Ccb->Flags, CCB_LAST_WRITE_UPDATED)) {
        return;
    }

    KeQuerySystemTime(&SysTime);
    Ext2SetInodeTime(&SysTime, &Fcb->Inode->i_mtime, &Fcb->Inode->i_mtime_extra);
    Ext2SetInodeTime(&SysTime, &Fcb->Inode->i_atime, &Fcb->Inode->i_atime_extra);
    Fcb->Mcb->Icb->LastAccessTime = Fcb->Mcb->Icb->LastWriteTime =
        Ext2GetInodeTime(Fcb->Inode->i_atime, Fcb->Inode->i_atime_extra);

    /* fails only once the volume stopped writing, and a cleanup has no
       one to report to */
    (void)Ext2SaveInode(IrpContext, Vcb, Fcb->Inode);

    Ext2NotifyReportChange(IrpContext, Vcb, Fcb->Mcb,
                           FILE_NOTIFY_CHANGE_ATTRIBUTES |
                           FILE_NOTIFY_CHANGE_LAST_WRITE |
                           FILE_NOTIFY_CHANGE_LAST_ACCESS,
                           FILE_ACTION_MODIFIED);
}

/*
 * What of a file goes with this handle. TRUE: the symlink it came through
 * (or that it was opened on). *NameDelete: a name of a hard-linked file;
 * the file itself - with its cache, which the other names still use - goes
 * only with the last handle of its last name (FCB_DELETE_PENDING). Decided
 * here, not when the disposition was set: links may have come or gone
 * since.
 */
static BOOLEAN
Ext2CleanupFileDelete(IN PFILE_OBJECT FileObject, IN PEXT2_FCB Fcb, IN PEXT2_CCB Ccb,
                      OUT PBOOLEAN NameDelete)
{
    PEXT2_MCB   Name = Ext2CcbName(Fcb, Ccb);

    *NameDelete = FALSE;

    if (IsFlagOn(Ccb->Flags, CCB_DELETE_ON_CLOSE)) {
        if (Ccb->SymLink || IsInodeSymLink(Fcb->Mcb->Inode)) {
            return TRUE;
        }
        /* the name this handle came in through is the one to go */
        SetLongFlag(Name->Flags, MCB_DELETE_PENDING);
    }

    /* a delete disposition set on this handle, which came through a
       symlink, marked the link (Ext2SetDispositionInfo): the link goes, the
       target - this Fcb - stays. Only this handle's own request counts
       (FileObject->DeletePending is per handle): the link's flag may come
       from a handle on the link itself, which removes it at its own
       cleanup, and taking that over here skipped the target's pending
       delete below for good. */
    if (Ccb->SymLink && FileObject->DeletePending &&
        IsFlagOn(Ccb->SymLink->Flags, MCB_DELETE_PENDING)) {
        return TRUE;
    }

    if (IsFlagOn(Name->Flags, MCB_DELETE_PENDING) && !IsFileDeleted(Name)) {
        if (Ext2IsLastLink(Name)) {
            SetLongFlag(Fcb->Flags, FCB_DELETE_PENDING);
        } else {
            ClearLongFlag(Fcb->Flags, FCB_DELETE_PENDING);
            *NameDelete = TRUE;
        }
    }
    return FALSE;
}

/* the byte range locks this process holds go; with none left by anyone,
   the fast I/O paths need not look for them */
static VOID
Ext2CleanupByteLocks(IN PFILE_OBJECT FileObject, IN PEXT2_FCB Fcb, IN PIRP Irp)
{
    FsRtlFastUnlockAll(&Fcb->FileLockAnchor, FileObject, IoGetRequestorProcess(Irp), NULL);

    if (!FsRtlGetNextFileLock(&Fcb->FileLockAnchor, TRUE)) {
        Fcb->Header.IsFastIoPossible = FastIoIsPossible;
    }
}

/*
 * The last handle of a file grown by create or by set-information outside
 * a write. An indirect-mapped file got plain blocks there: what lies past
 * the valid data is zeroed now (an extent-mapped one has unwritten extents,
 * which read as zeros). An allocation asked for at create and not written
 * is given back.
 */
static VOID
Ext2CleanupAllocation(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb,
                      IN PFILE_OBJECT FileObject, IN PEXT2_FCB Fcb)
{
    LARGE_INTEGER Size;

    if (Fcb->OpenHandleCount != 0 || !FlagOn(Fcb->Flags, FCB_ALLOC_IN_CREATE | FCB_ALLOC_IN_SETINFO)) {
        return;
    }

    if (FlagOn(Fcb->Flags, FCB_ALLOC_IN_SETINFO) && !INODE_HAS_EXTENT(Fcb->Inode) &&
        Fcb->Header.ValidDataLength.QuadPart < Fcb->Header.FileSize.QuadPart) {
        /* a tail that could not be zeroed stays as it was: an I/O error
           here is the cache manager's to report, and a cleanup has no one
           to report to */
        (void)Ext2ZeroData(IrpContext, FileObject, &Fcb->Header.ValidDataLength,
                           &Fcb->Header.AllocationSize);
    }

    if (!FlagOn(Fcb->Flags, FCB_ALLOC_IN_CREATE)) {
        return;
    }

    ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
    __try {
        if (!IsFlagOn(Fcb->Flags, FCB_DELETE_PENDING)) {
            Size.QuadPart = CEILING_ALIGNED(ULONGLONG, (ULONGLONG)Fcb->Inode->i_size,
                                            (ULONGLONG)BLOCK_SIZE);
            /* blocks past the end that stay are legal: Size says how far
               the release got */
            (void)Ext2TruncateFile(IrpContext, Vcb, Fcb->Mcb, &Size);
            Fcb->Header.AllocationSize = Size;
            Fcb->Header.FileSize.QuadPart = Fcb->Inode->i_size;
            if (Fcb->Header.ValidDataLength.QuadPart > Fcb->Header.FileSize.QuadPart) {
                Fcb->Header.ValidDataLength.QuadPart = Fcb->Header.FileSize.QuadPart;
            }
            if (CcIsFileCached(FileObject)) {
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)(&(Fcb->Header.AllocationSize)));
            }
        }
        ClearLongFlag(Fcb->Flags, FCB_ALLOC_IN_CREATE | FCB_ALLOC_IN_WRITE | FCB_ALLOC_IN_SETINFO);
    } __finally {
        ExReleaseResourceLite(&Fcb->PagingIoResource);
    }
}

/* A file handle. TRUE when the symlink it came through goes with it;
   *NameDelete when a hard link of the file does (Ext2CleanupFileDelete). */
static BOOLEAN
Ext2CleanupFile(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PFILE_OBJECT FileObject,
                IN PEXT2_FCB Fcb, IN PEXT2_CCB Ccb, OUT PBOOLEAN NameDelete)
{
    BOOLEAN SymLinkDelete;

    Ext2CleanupWriteTime(IrpContext, Vcb, FileObject, Fcb, Ccb);

    FsRtlCheckOplock(&Fcb->Oplock, IrpContext->Irp, IrpContext, NULL, NULL);
    Fcb->Header.IsFastIoPossible = Ext2IsFastIoPossible(Fcb);

    if (!IsFlagOn(FileObject->Flags, FO_CACHE_SUPPORTED)) {
        Fcb->NonCachedOpenCount--;
    }

    SymLinkDelete = Ext2CleanupFileDelete(FileObject, Fcb, Ccb, NameDelete);
    Ext2CleanupByteLocks(FileObject, Fcb, IrpContext->Irp);
    Ext2CleanupAllocation(IrpContext, Vcb, FileObject, Fcb);
    return SymLinkDelete;
}

/*
 * The handle's view of the cache goes. Only when the remaining opens are all
 * non-cached does the cache have to be written out now, so that they see
 * the data; a file about to be deleted has its cache purged. The last close
 * of a file is not that case: its dirty pages stay with the cache manager
 * and reach disk by the lazy writer, as on NTFS and Linux - a synchronous
 * flush here cost a disk write per close (measured: 2 ms per 1 KB file).
 */
static VOID
Ext2CleanupCache(IN PEXT2_VCB Vcb, IN PFILE_OBJECT FileObject, IN PEXT2_FCB Fcb)
{
    if (IsFlagOn(FileObject->Flags, FO_CACHE_SUPPORTED) &&
        Fcb->NonCachedOpenCount == Fcb->OpenHandleCount &&
        Fcb->SectionObject.DataSectionObject != NULL) {

        if (Fcb->NonCachedOpenCount > 0 && !IsVcbReadOnly(Vcb)) {
            CcFlushCache(&Fcb->SectionObject, NULL, 0, NULL);
            ClearLongFlag(Fcb->Flags, FCB_FILE_MODIFIED);
        }

        if (Fcb->NonCachedOpenCount > 0 || IsFlagOn(Fcb->Flags, FCB_DELETE_PENDING)) {
            /* the paging I/O in flight first */
            ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
            ExReleaseResourceLite(&Fcb->PagingIoResource);

            /* CcPurge could generate recursive IRP_MJ_CLOSE request */
            CcPurgeCacheSection(&Fcb->SectionObject, NULL, 0, FALSE);
        }
    }

    CcUninitializeCacheMap(FileObject, NULL, NULL);
}

/*
 * The names that go with this handle, outside the Fcb's MainResource
 * (Ext2DeleteFile takes its locks itself). SymLinkDelete: the link the
 * handle came through, or was opened on. FileDelete: every name that asked
 * to go - the one of a plain file, each marked hard link otherwise.
 * Ext2DeleteFile frees the inode with the last name, which, while handles
 * remain, waits for the last of them (cache and all).
 */
static VOID
Ext2CleanupDelete(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PFILE_OBJECT FileObject,
                  IN PEXT2_FCB Fcb, IN PEXT2_CCB Ccb, IN BOOLEAN SymLinkDelete, IN BOOLEAN FileDelete)
{
    PEXT2_MCB       Name;
    EXT2_SAVED_NAME Saved;
    NTSTATUS        Status;

    /* each name's path is taken while it is still in the tree: reported
       after it left, it would reach no watcher */
    if (SymLinkDelete) {
        if (Ccb->SymLink) {
            Name = Ccb->SymLink;
            FileObject->DeletePending = FALSE;
        } else {
            Name = Ext2CcbName(Fcb, Ccb);
        }
        Ext2NotifySaveName(Vcb, Name, &Saved);
        Status = Ext2DeleteFile(IrpContext, Vcb, Fcb, Name);
        Ext2NotifySavedRemoval(Vcb, &Saved, NT_SUCCESS(Status));
    }

    if (!FileDelete) {
        return;
    }

    while ((Name = Ext2NextPendingName(Vcb, Fcb->Mcb->Icb)) != NULL) {

        if (Fcb->OpenHandleCount > 0 && Ext2IsLastLink(Name)) {
            SetLongFlag(Fcb->Flags, FCB_DELETE_PENDING);
            Ext2DerefMcb(Name);
            break;
        }

        Ext2NotifySaveName(Vcb, Name, &Saved);
        Status = Ext2DeleteFile(IrpContext, Vcb, Fcb, Name);
        Ext2NotifySavedRemoval(Vcb, &Saved, NT_SUCCESS(Status));
        if (!NT_SUCCESS(Status)) {
            /* not going anywhere (directory not empty ...) */
            ClearLongFlag(Name->Flags, MCB_DELETE_PENDING);
        }
        Ext2DerefMcb(Name);
    }

    /* FCB_DELETE_PENDING stands for "a name of this file is going and it is
       the last one". With no name left waiting and the file still alive (a
       delete that failed, a name unmarked meanwhile) the flag is stale:
       left set, every later open was refused with STATUS_DELETE_PENDING and
       no cleanup would ever come to clear it. */
    if (!IsInodeDeleted(Fcb)) {
        PEXT2_MCB Waiting = Ext2NextPendingName(Vcb, Fcb->Mcb->Icb);
        if (Waiting) {
            Ext2DerefMcb(Waiting);
        } else {
            ClearLongFlag(Fcb->Flags, FCB_DELETE_PENDING);
        }
    }
}

NTSTATUS
Ext2Cleanup (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT  DeviceObject;
    PEXT2_VCB       Vcb;
    PFILE_OBJECT    FileObject;
    PEXT2_FCB       Fcb = NULL;
    PEXT2_CCB       Ccb;

    BOOLEAN         FcbResourceAcquired = FALSE;
    BOOLEAN         SymLinkDelete = FALSE;
    BOOLEAN         NameDelete = FALSE;
    BOOLEAN         FileDelete;

    ASSERT(IrpContext != NULL);
    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    __try {

        DeviceObject = IrpContext->DeviceObject;
        if (IsExt2FsDevice(DeviceObject)) {
            __leave;
        }

        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        ASSERT(Vcb != NULL);
        ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
               (Vcb->Identifier.Size == sizeof(EXT2_VCB)));
        if (!IsVcbInited(Vcb)) {
            __leave;
        }

        FileObject = IrpContext->FileObject;
        Fcb = (PEXT2_FCB) FileObject->FsContext;
        Ccb = (PEXT2_CCB) FileObject->FsContext2;
        if (Fcb == NULL || IsFlagOn(FileObject->Flags, FO_CLEANUP_COMPLETE)) {
            __leave;
        }

        if (Fcb->Identifier.Type == EXT2VCB) {
            Ext2CleanupVolume(Vcb, FileObject, Ccb);
            __leave;
        }
        if (Fcb->Identifier.Type != EXT2FCB || Ccb == NULL) {
            __leave;
        }
        ASSERT(Fcb->Identifier.Size == sizeof(EXT2_FCB));
        ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
               (Ccb->Identifier.Size == sizeof(EXT2_CCB)));

        ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
        FcbResourceAcquired = TRUE;

        if (IsDirectory(Fcb)) {
            Ext2CleanupNotify(Vcb, Fcb, Ccb);
        }

        Ext2DerefXcb(&Vcb->OpenHandleCount);
        Ext2DerefXcb(&Fcb->OpenHandleCount);

        if (IsFlagOn(FileObject->Flags, FO_FILE_MODIFIED)) {
            Fcb->Mcb->FileAttr |= FILE_ATTRIBUTE_ARCHIVE;
        }

        if (IsDirectory(Fcb)) {
            SymLinkDelete = Ext2CleanupDirectory(FileObject, Fcb, Ccb);
        } else {
            SymLinkDelete = Ext2CleanupFile(IrpContext, Vcb, FileObject, Fcb, Ccb, &NameDelete);
        }

        IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);

        if (!IsDirectory(Fcb)) {
            Ext2CleanupCache(Vcb, FileObject, Fcb);
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
                     (IsFlagOn(Fcb->Flags, FCB_DELETE_PENDING) && Fcb->OpenHandleCount == 0);

        if (SymLinkDelete || FileDelete) {

            ExReleaseResourceLite(&Fcb->MainResource);
            FcbResourceAcquired = FALSE;

            Ext2CleanupDelete(IrpContext, Vcb, FileObject, Fcb, Ccb, SymLinkDelete, FileDelete);

            ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
            FcbResourceAcquired = TRUE;

            if (FileDelete) {
                SetFlag(FileObject->Flags, FO_FILE_MODIFIED);
                if (CcIsFileCached(FileObject)) {
                    CcSetFileSizes(FileObject, (PCC_FILE_SIZES)(&(Fcb->Header.AllocationSize)));
                }
            }
        }

        DEBUG(DL_INF, ("Ext2Cleanup: OpenCount=%u ReferCount=%u NonCachedCount=%xh %wZ\n",
                       Fcb->OpenHandleCount, Fcb->ReferenceCount, Fcb->NonCachedOpenCount,
                       &Fcb->Mcb->ShortName));

        SetFlag(FileObject->Flags, FO_CLEANUP_COMPLETE);

    } __finally {

        if (FcbResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        /* a cleanup does not fail: what it could not do (a delete refused,
           a write-back error) has no one left to report to */
        if (!IrpContext->ExceptionInProgress) {
            Ext2CompleteIrpContext(IrpContext, STATUS_SUCCESS);
        }
    }

    /* the volume may have lost its last handle */
    Ext2UnloadKick();

    return STATUS_SUCCESS;
}
