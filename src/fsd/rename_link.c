/**
 * rename_link.c - rename and hard link creation (FileRenameInformation, FileLinkInformation).
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "linux\ext4.h"
#include "linux\ext4_xattr.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2SetRenameInfo)
#pragma alloc_text(PAGE, Ext2SetLinkInfo)
#endif

NTSTATUS
Ext2SetRenameInfo(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_FCB         Fcb,
    PEXT2_CCB         Ccb
)
{
    PEXT2_MCB               Mcb = Ext2CcbName(Fcb, Ccb);

    PEXT2_FCB               TargetDcb = NULL;   /* Dcb of target directory */
    PEXT2_MCB               TargetMcb = NULL;
    PEXT2_FCB               ParentDcb = NULL;   /* Dcb of it's current parent */
    PEXT2_MCB               ParentMcb = NULL;

    PEXT2_FCB               ExistingFcb = NULL; /* Target file Fcb if it exists*/
    PEXT2_MCB               ExistingMcb = NULL;

    UNICODE_STRING          FileName;

    NTSTATUS                Status;

    PIRP                    Irp;
    PIO_STACK_LOCATION      IrpSp;

    PFILE_OBJECT            FileObject;
    PFILE_OBJECT            TargetObject;

    struct dentry          *NewEntry = NULL;
    __u16                   nlink = 0;

    BOOLEAN                 ReplaceIfExists;
    BOOLEAN                 bMove = FALSE;
    BOOLEAN                 bTargetRemoved = FALSE;

    BOOLEAN                 bFcbLockAcquired = FALSE;

    PFILE_RENAME_INFORMATION    FRI;

    if (Mcb->Inode->i_ino == EXT2_ROOT_INO) {
        Status = STATUS_INVALID_PARAMETER;
        goto errorout;
    }

    Irp = IrpContext->Irp;
    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    FileObject = IrpSp->FileObject;
    TargetObject = IrpSp->Parameters.SetFile.FileObject;
    ReplaceIfExists = IrpSp->Parameters.SetFile.ReplaceIfExists;

    FRI = (PFILE_RENAME_INFORMATION)Irp->AssociatedIrp.SystemBuffer;

    if (TargetObject == NULL) {

        UNICODE_STRING  NewName;

        NewName.Buffer = FRI->FileName;
        NewName.MaximumLength = NewName.Length = (USHORT)FRI->FileNameLength;

        while (NewName.Length > 0 && NewName.Buffer[NewName.Length/2 - 1] == L'\\') {
            NewName.Buffer[NewName.Length/2 - 1] = 0;
            NewName.Length -= 2;
        }

        while (NewName.Length > 0 && NewName.Buffer[NewName.Length/2 - 1] != L'\\') {
            NewName.Length -= 2;
        }

        NewName.Buffer = (USHORT *)((UCHAR *)NewName.Buffer + NewName.Length);
        NewName.Length = (USHORT)(FRI->FileNameLength - NewName.Length);

        FileName = NewName;

        TargetMcb = Mcb->Parent;
        if (IsMcbSymLink(TargetMcb)) {
            TargetMcb = TargetMcb->Target;
            ASSERT(!IsMcbSymLink(TargetMcb));
        }

        if (TargetMcb == NULL || FileName.Length >= EXT2_NAME_LEN*2) {
            Status = STATUS_OBJECT_NAME_INVALID;
            goto errorout;
        }

    } else {

        TargetDcb = (PEXT2_FCB)(TargetObject->FsContext);

        if (!TargetDcb || TargetDcb->Vcb != Vcb) {

            DbgBreak();

            Status = STATUS_INVALID_PARAMETER;
            goto errorout;
        }

        TargetMcb = TargetDcb->Mcb;
        FileName = TargetObject->FileName;
    }

    if (FsRtlDoesNameContainWildCards(&FileName)) {
        Status = STATUS_OBJECT_NAME_INVALID;
        goto errorout;
    }

    if (TargetMcb->Inode->i_ino == Mcb->Parent->Inode->i_ino) {
        if (FsRtlAreNamesEqual( &FileName,
                                &(Mcb->ShortName),
                                FALSE,
                                NULL )) {
            Status = STATUS_SUCCESS;
            goto errorout;
        }
    } else {
        bMove = TRUE;
    }

    if (!bFcbLockAcquired) {
        ExAcquireResourceExclusiveLite(&Vcb->FcbLock, TRUE);
        bFcbLockAcquired = TRUE;
    }

    TargetDcb = TargetMcb->Icb->Fcb;
    if (TargetDcb == NULL) {
        TargetDcb = Ext2AllocateFcb(Vcb, TargetMcb);
    }
    if (TargetDcb) {
        Ext2ReferXcb(&TargetDcb->ReferenceCount);
    }

    ParentMcb = Mcb->Parent;
    ParentDcb = ParentMcb->Icb->Fcb;

    if ((TargetMcb->Inode->i_ino != ParentMcb->Inode->i_ino)) {

        if (ParentDcb == NULL) {
            ParentDcb = Ext2AllocateFcb(Vcb, ParentMcb);
        }
    }
    if (ParentDcb) {
        Ext2ReferXcb(&ParentDcb->ReferenceCount);
    }

    if (bFcbLockAcquired) {
        ExReleaseResourceLite(&Vcb->FcbLock);
        bFcbLockAcquired = FALSE;
    }

    if (!TargetDcb || !ParentDcb) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }

    DEBUG(DL_RES, ("Ext2SetRenameInfo: rename %wZ to %wZ\\%wZ\n",
                   &Mcb->FullName, &TargetMcb->FullName, &FileName));

    Status = Ext2LookupFile(
                 IrpContext,
                 Vcb,
                 &FileName,
                 TargetMcb,
                 &ExistingMcb,
                 0
             );

    if (NT_SUCCESS(Status) && ExistingMcb != Mcb) {

        if (!ReplaceIfExists) {

            Status = STATUS_OBJECT_NAME_COLLISION;
            DEBUG(DL_RES, ("Ext2SetRenameInfo: Target file %wZ exists\n",
                           &ExistingMcb->FullName));
            goto errorout;

        } else {

            if ( (ExistingFcb = ExistingMcb->Icb->Fcb) && !IsMcbSymLink(ExistingMcb) ) {

                Status = Ext2IsFileRemovable(IrpContext, Vcb, ExistingFcb, Ccb);
                if (!NT_SUCCESS(Status)) {
                    DEBUG(DL_REN, ("Ext2SetRenameInfo: Target file %wZ cannot be removed.\n",
                                   &ExistingMcb->FullName));
                    goto errorout;
                }
            }

            Status = Ext2DeleteFile(IrpContext, Vcb, ExistingFcb, ExistingMcb);
            if (!NT_SUCCESS(Status)) {
                DEBUG(DL_REN, ("Ext2SetRenameInfo: Failed to delete %wZ with status: %xh.\n",
                               &FileName, Status));

                goto errorout;
            }

            bTargetRemoved = TRUE;
        }
    }

    /* remove directory entry of old name */
    nlink = Mcb->Inode->i_nlink;
    Status = Ext2RemoveEntry(IrpContext, Vcb, ParentDcb, Mcb);
    if (!NT_SUCCESS(Status)) {
        DEBUG(DL_REN, ("Ext2SetRenameInfo: Failed to remove entry %wZ with status %xh.\n",
                       &Mcb->FullName, Status));
        DbgBreak();
        goto errorout;
    }

    /* add new entry for new target name */
    Status = Ext2AddEntry(IrpContext, Vcb, TargetDcb, Mcb->Inode, &FileName, &NewEntry);
    if (!NT_SUCCESS(Status)) {
        DEBUG(DL_REN, ("Ext2SetRenameInfo: Failed to add entry for %wZ with status: %xh.\n",
                       &FileName, Status));
        Ext2AddEntry(IrpContext, Vcb, ParentDcb, Mcb->Inode, &Mcb->ShortName, &NewEntry);
        goto errorout;
    }

    /* a rename never changes the link count of the inode that moves: the
       remove/add pair above is only right for files (1 -> 0 -> 1); for a
       directory ext3_dec_count stops at 2 and the add would leave 3 */
    if (IsMcbDirectory(Mcb) && Mcb->Inode->i_nlink != nlink) {
        Mcb->Inode->i_nlink = nlink;
        ext3_mark_inode_dirty(IrpContext, Mcb->Inode);
    }

    /* correct the inode number in ..  entry */
    if (IsMcbDirectory(Mcb)) {
        Status = Ext2SetParentEntry(
                     IrpContext, Vcb, Fcb,
                     ParentMcb->Inode->i_ino,
                     TargetMcb->Inode->i_ino );
        if (!NT_SUCCESS(Status)) {
            DEBUG(DL_REN, ("Ext2SetRenameInfo: Failed to set parent refer of %wZ with %xh.\n",
                           &Mcb->FullName, Status));
            DbgBreak();
            goto errorout;
        }
    }

    /* Update current dentry from the newly created one. We need keep the original
       dentry to assure children's links are valid if current entry is a directory */
    if (Mcb->de) {
        char *np = Mcb->de->d_name.name;
        *(Mcb->de) = *NewEntry;
        NewEntry->d_name.name = np;
    }

    if (bTargetRemoved) {
        Ext2NotifyReportChange(
            IrpContext,
            Vcb,
            ExistingMcb,
            (IsMcbDirectory(ExistingMcb) ?
             FILE_NOTIFY_CHANGE_DIR_NAME :
             FILE_NOTIFY_CHANGE_FILE_NAME ),
            FILE_ACTION_REMOVED);
    }

    if (NT_SUCCESS(Status)) {

        if (bMove) {
            Ext2NotifyReportChange(
                IrpContext,
                Vcb,
                Mcb,
                (IsDirectory(Fcb) ?
                 FILE_NOTIFY_CHANGE_DIR_NAME :
                 FILE_NOTIFY_CHANGE_FILE_NAME ),
                FILE_ACTION_REMOVED);

        } else {
            Ext2NotifyReportChange(
                IrpContext,
                Vcb,
                Mcb,
                (IsDirectory(Fcb) ?
                 FILE_NOTIFY_CHANGE_DIR_NAME :
                 FILE_NOTIFY_CHANGE_FILE_NAME ),
                FILE_ACTION_RENAMED_OLD_NAME);

        }

        /* the name cache is hashed by parent and name: out of the tree
           while either changes, back in under the new parent afterwards */
        Ext2RemoveMcb(Vcb, Mcb);

        if (!Ext2BuildName( &Mcb->ShortName,
                            &FileName, NULL     ) ||
            !Ext2BuildName( &Mcb->FullName,
                            &FileName,
                            &TargetMcb->FullName)) {
            Ext2InsertMcb(Vcb, ParentMcb, Mcb);
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto errorout;
        }

        Ext2InsertMcb(Vcb, TargetMcb, Mcb);

        if (bMove) {
            Ext2NotifyReportChange(
                IrpContext,
                Vcb,
                Mcb,
                (IsDirectory(Fcb) ?
                 FILE_NOTIFY_CHANGE_DIR_NAME :
                 FILE_NOTIFY_CHANGE_FILE_NAME ),
                FILE_ACTION_ADDED);
        } else {
            Ext2NotifyReportChange(
                IrpContext,
                Vcb,
                Mcb,
                (IsDirectory(Fcb) ?
                 FILE_NOTIFY_CHANGE_DIR_NAME :
                 FILE_NOTIFY_CHANGE_FILE_NAME ),
                FILE_ACTION_RENAMED_NEW_NAME  );
        }
    }

errorout:

    if (bFcbLockAcquired) {
        ExReleaseResourceLite(&Vcb->FcbLock);
        bFcbLockAcquired = FALSE;
    }

    if (NewEntry)
        Ext2FreeEntry(NewEntry);

    if (TargetDcb) {
        Ext2ReleaseFcb(TargetDcb);
    }

    if (ParentDcb) {
        Ext2ReleaseFcb(ParentDcb);
    }

    if (ExistingMcb)
        Ext2DerefMcb(ExistingMcb);

    return Status;
}

NTSTATUS
Ext2SetLinkInfo(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_FCB         Fcb,
    PEXT2_CCB         Ccb
)
{
    PEXT2_MCB               Mcb = Ext2CcbName(Fcb, Ccb);

    PEXT2_FCB               TargetDcb = NULL;   /* Dcb of target directory */
    PEXT2_MCB               TargetMcb = NULL;
    PEXT2_FCB               ParentDcb = NULL;   /* Dcb of it's current parent */
    PEXT2_MCB               ParentMcb = NULL;

    PEXT2_FCB               ExistingFcb = NULL; /* Target file Fcb if it exists*/
    PEXT2_MCB               ExistingMcb = NULL;
    PEXT2_MCB               LinkMcb = NULL;     /* Mcb for new hardlink */

    UNICODE_STRING          FileName;

    NTSTATUS                Status;

    PIRP                    Irp;
    PIO_STACK_LOCATION      IrpSp;

    PFILE_OBJECT            FileObject;
    PFILE_OBJECT            TargetObject;

    BOOLEAN                 ReplaceIfExists;
    BOOLEAN                 bTargetRemoved = FALSE;

    BOOLEAN                 bFcbLockAcquired = FALSE;

    PFILE_LINK_INFORMATION  FLI;

    if (IsMcbDirectory(Mcb)) {
        Status = STATUS_INVALID_PARAMETER;
        goto errorout;
    }

    /* an immutable or append-only inode gets no further names (Linux) */
    if (Ext4IsSealed(Mcb->Inode)) {
        Status = STATUS_ACCESS_DENIED;
        goto errorout;
    }

    Irp = IrpContext->Irp;
    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    FileObject = IrpSp->FileObject;
    TargetObject = IrpSp->Parameters.SetFile.FileObject;
    ReplaceIfExists = IrpSp->Parameters.SetFile.ReplaceIfExists;

    FLI =(PFILE_LINK_INFORMATION)Irp->AssociatedIrp.SystemBuffer;

    if (TargetObject == NULL) {

        UNICODE_STRING  NewName;

        NewName.Buffer = FLI->FileName;
        NewName.MaximumLength = NewName.Length = (USHORT)FLI->FileNameLength;

        while (NewName.Length > 0 && NewName.Buffer[NewName.Length/2 - 1] == L'\\') {
            NewName.Buffer[NewName.Length/2 - 1] = 0;
            NewName.Length -= 2;
        }

        while (NewName.Length > 0 && NewName.Buffer[NewName.Length/2 - 1] != L'\\') {
            NewName.Length -= 2;
        }

        NewName.Buffer = (USHORT *)((UCHAR *)NewName.Buffer + NewName.Length);
        NewName.Length = (USHORT)(FLI->FileNameLength - NewName.Length);

        FileName = NewName;

        TargetMcb = Mcb->Parent;
        if (IsMcbSymLink(TargetMcb)) {
            TargetMcb = TargetMcb->Target;
            ASSERT(!IsMcbSymLink(TargetMcb));
        }

        if (TargetMcb == NULL || FileName.Length >= EXT2_NAME_LEN*2) {
            Status = STATUS_OBJECT_NAME_INVALID;
            goto errorout;
        }

    } else {

        TargetDcb = (PEXT2_FCB)(TargetObject->FsContext);
        if (!TargetDcb || TargetDcb->Vcb != Vcb) {
            DbgBreak();
            Status = STATUS_INVALID_PARAMETER;
            goto errorout;
        }

        TargetMcb = TargetDcb->Mcb;
        FileName = TargetObject->FileName;
    }

    if (FsRtlDoesNameContainWildCards(&FileName)) {
        Status = STATUS_OBJECT_NAME_INVALID;
        goto errorout;
    }

    if (TargetMcb->Inode->i_ino == Mcb->Parent->Inode->i_ino) {
        if (FsRtlAreNamesEqual( &FileName,
                                &(Mcb->ShortName),
                                FALSE,
                                NULL )) {
            Status = STATUS_SUCCESS;
            goto errorout;
        }
    }

    ExAcquireResourceExclusiveLite(&Vcb->FcbLock, TRUE);
    bFcbLockAcquired = TRUE;

    TargetDcb = TargetMcb->Icb->Fcb;
    if (TargetDcb == NULL) {
        TargetDcb = Ext2AllocateFcb(Vcb, TargetMcb);
    }
    if (TargetDcb) {
        Ext2ReferXcb(&TargetDcb->ReferenceCount);
    }

    ParentMcb = Mcb->Parent;
    ParentDcb = ParentMcb->Icb->Fcb;

    if ((TargetMcb->Inode->i_ino != ParentMcb->Inode->i_ino)) {

        if (ParentDcb == NULL) {
            ParentDcb = Ext2AllocateFcb(Vcb, ParentMcb);
        }
    }
    if (ParentDcb) {
        Ext2ReferXcb(&ParentDcb->ReferenceCount);
    }

    if (bFcbLockAcquired) {
        ExReleaseResourceLite(&Vcb->FcbLock);
        bFcbLockAcquired = FALSE;
    }

    if (!TargetDcb || !ParentDcb) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }

    DEBUG(DL_RES, ("Ext2SetLinkInfo: %wZ\\%wZ -> %wZ\n",
                   &TargetMcb->FullName, &FileName, &Mcb->FullName));

    Status = Ext2LookupFile(IrpContext, Vcb, &FileName,
                            TargetMcb, &ExistingMcb, 0);
    if (NT_SUCCESS(Status) && ExistingMcb != Mcb) {

        if (!ReplaceIfExists) {

            Status = STATUS_OBJECT_NAME_COLLISION;
            DEBUG(DL_RES, ("Ext2SetRenameInfo: Target file %wZ exists\n",
                           &ExistingMcb->FullName));
            goto errorout;

        } else {

            if ( (ExistingFcb = ExistingMcb->Icb->Fcb) && !IsMcbSymLink(ExistingMcb) ) {
                Status = Ext2IsFileRemovable(IrpContext, Vcb, ExistingFcb, Ccb);
                if (!NT_SUCCESS(Status)) {
                    DEBUG(DL_REN, ("Ext2SetRenameInfo: Target file %wZ cannot be removed.\n",
                                   &ExistingMcb->FullName));
                    goto errorout;
                }
            }

            Status = Ext2DeleteFile(IrpContext, Vcb, ExistingFcb, ExistingMcb);
            if (!NT_SUCCESS(Status)) {
                DEBUG(DL_REN, ("Ext2SetRenameInfo: Failed to delete %wZ with status: %xh.\n",
                               &FileName, Status));

                goto errorout;
            }
            bTargetRemoved = TRUE;
        }
    }

    /* add new entry for new target name */
    Status = Ext2AddEntry(IrpContext, Vcb, TargetDcb, Mcb->Inode, &FileName, NULL);
    if (!NT_SUCCESS(Status)) {
        DEBUG(DL_REN, ("Ext2SetLinkInfo: Failed to add entry for %wZ with status: %xh.\n",
                       &FileName, Status));
        goto errorout;
    }

    if (bTargetRemoved) {
        Ext2NotifyReportChange(
            IrpContext,
            Vcb,
            ExistingMcb,
            (IsMcbDirectory(ExistingMcb) ?
             FILE_NOTIFY_CHANGE_DIR_NAME :
             FILE_NOTIFY_CHANGE_FILE_NAME ),
            FILE_ACTION_REMOVED);
    }

    if (NT_SUCCESS(Status)) {

        Ext2LookupFile(IrpContext, Vcb, &FileName, TargetMcb, &LinkMcb, 0);
        if (!LinkMcb)
            goto errorout;

        Ext2NotifyReportChange(
            IrpContext,
            Vcb,
            LinkMcb,
            FILE_NOTIFY_CHANGE_FILE_NAME,
            FILE_ACTION_ADDED);
    }

errorout:

    if (bFcbLockAcquired) {
        ExReleaseResourceLite(&Vcb->FcbLock);
        bFcbLockAcquired = FALSE;
    }

    if (TargetDcb) {
        Ext2ReleaseFcb(TargetDcb);
    }

    if (ParentDcb) {
        Ext2ReleaseFcb(ParentDcb);
    }

    if (ExistingMcb)
        Ext2DerefMcb(ExistingMcb);

    if (LinkMcb)
        Ext2DerefMcb(LinkMcb);

    return Status;
}
