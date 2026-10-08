/**
 * RenameAndLink.c - rename and hard link creation (FileRenameInformation, FileLinkInformation).
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

/* the name a rename or a new hard link makes: where it goes, what it replaces */
typedef struct _EXT2_NEW_NAME {
    PEXT2_MCB       Mcb;            /* the name that moves, or the file linked */
    UNICODE_STRING  FileName;       /* the new name's last component */
    PEXT2_MCB       TargetMcb;      /* the directory it goes in */
    PEXT2_MCB       TargetHeld;     /* TargetMcb, when referenced here */
    PEXT2_FCB       TargetDcb;      /* referenced */
    PEXT2_MCB       ParentMcb;      /* the directory Mcb is in now */
    PEXT2_FCB       ParentDcb;      /* referenced */
    PEXT2_MCB       ExistingMcb;    /* a name there already, referenced */
    BOOLEAN         Replaced;       /* ... which the new one replaced */
    EXT2_SAVED_NAME Existing;       /* ... its path, for the report after it went */
} EXT2_NEW_NAME, *PEXT2_NEW_NAME;

/* a name Ext2BuildName made */
static VOID
Ext2FreeName(IN OUT PUNICODE_STRING Name)
{
    if (Name->Buffer != NULL) {
        DEC_MEM_COUNT(PS_MCB_NAME, Name->Buffer, Name->MaximumLength);
        Ext2FreePool(Name->Buffer, EXT2_FNAME_MAGIC);
        RtlZeroMemory(Name, sizeof(UNICODE_STRING));
    }
}

/*
 * Where the new name goes. Without a target directory handle it is a name
 * in Mcb's own directory, and only its last component counts (trailing
 * backslashes are not part of it - they used to be kept, zeroed, in its
 * length). With one, the I/O manager opened that directory and left the
 * last component as its file object's name.
 */
static NTSTATUS
Ext2NewNameTarget(IN PEXT2_VCB Vcb, IN PFILE_OBJECT TargetObject,
                  IN ULONG NameLength, IN PWCHAR Name, IN OUT PEXT2_NEW_NAME N)
{
    if (TargetObject == NULL) {

        ULONG Length = NameLength & ~(ULONG)(sizeof(WCHAR) - 1);
        ULONG Start;

        while (Length > 0 && Name[Length / sizeof(WCHAR) - 1] == L'\\') {
            Length -= sizeof(WCHAR);
        }
        for (Start = Length; Start > 0 && Name[Start / sizeof(WCHAR) - 1] != L'\\'; ) {
            Start -= sizeof(WCHAR);
        }
        if (Length - Start > MAXUSHORT) {
            return STATUS_OBJECT_NAME_INVALID;
        }
        N->FileName.Buffer = Name + Start / sizeof(WCHAR);
        N->FileName.Length = N->FileName.MaximumLength = (USHORT)(Length - Start);

        /* the directory the name is in, through a symlink to it if that is
           how it was opened; referenced until the end */
        N->TargetMcb = N->TargetHeld = Ext2ReferDirectory(Vcb, N->Mcb->Parent);
        if (N->TargetMcb == NULL) {
            return STATUS_OBJECT_NAME_INVALID;
        }

    } else {

        PEXT2_FCB Directory = (PEXT2_FCB)TargetObject->FsContext;

        if (Directory == NULL || Directory->Vcb != Vcb) {
            return STATUS_INVALID_PARAMETER;
        }
        N->TargetMcb = Directory->Mcb;
        N->FileName = TargetObject->FileName;
    }

    if (N->FileName.Length == 0 || N->FileName.Length >= EXT2_NAME_LEN * sizeof(WCHAR) ||
        FsRtlDoesNameContainWildCards(&N->FileName)) {
        return STATUS_OBJECT_NAME_INVALID;
    }
    return STATUS_SUCCESS;
}

/* the target directory and the current one, as Fcbs held for the change */
static NTSTATUS
Ext2NewNameDirectories(IN PEXT2_VCB Vcb, IN OUT PEXT2_NEW_NAME N)
{
    N->ParentMcb = N->Mcb->Parent;

    ExAcquireResourceExclusiveLite(&Vcb->FcbLock, TRUE);

    N->TargetDcb = N->TargetMcb->Icb->Fcb;
    if (N->TargetDcb == NULL) {
        N->TargetDcb = Ext2AllocateFcb(Vcb, N->TargetMcb);
    }
    if (N->TargetDcb != NULL) {
        Ext2ReferXcb(&N->TargetDcb->ReferenceCount);
    }

    /* the same directory has its Fcb from just above */
    N->ParentDcb = N->ParentMcb->Icb->Fcb;
    if (N->ParentDcb == NULL && N->TargetMcb->Inode->i_ino != N->ParentMcb->Inode->i_ino) {
        N->ParentDcb = Ext2AllocateFcb(Vcb, N->ParentMcb);
    }
    if (N->ParentDcb != NULL) {
        Ext2ReferXcb(&N->ParentDcb->ReferenceCount);
    }

    ExReleaseResourceLite(&Vcb->FcbLock);

    return N->TargetDcb != NULL && N->ParentDcb != NULL ? STATUS_SUCCESS :
                                                          STATUS_INSUFFICIENT_RESOURCES;
}

/*
 * The steps a rename and a new link share, up to the change: the target,
 * its directory and the current one held. *Same: the new name is the
 * name itself - nothing to do.
 */
static NTSTATUS
Ext2NewNamePrepare(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb,
                   IN ULONG NameLength, IN PWCHAR Name, IN OUT PEXT2_NEW_NAME N, OUT PBOOLEAN Same)
{
    PIO_STACK_LOCATION  IrpSp = IoGetCurrentIrpStackLocation(IrpContext->Irp);
    NTSTATUS            Status;

    *Same = FALSE;
    Status = Ext2NewNameTarget(Vcb, IrpSp->Parameters.SetFile.FileObject, NameLength, Name, N);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    if (N->TargetMcb->Inode->i_ino == N->Mcb->Parent->Inode->i_ino &&
        FsRtlAreNamesEqual(&N->FileName, &N->Mcb->ShortName, FALSE, NULL)) {
        *Same = TRUE;
        return STATUS_SUCCESS;
    }

    Status = Ext2NewNameDirectories(Vcb, N);
    DEBUG(DL_RES, ("Ext2NewName: %wZ -> %wZ\\%wZ: %08x\n",
                   &N->Mcb->ShortName, &N->TargetMcb->ShortName, &N->FileName, Status));
    return Status;
}

/*
 * A name already in the target's place: refused, unless the caller asked
 * to replace it; then it goes first, if it can (a directory must be empty,
 * a file not mapped ...). The name itself in another spelling is no other.
 * A lookup that fails for another reason than an absent name fails the
 * change (it used to go on as if the name were free).
 */
static NTSTATUS
Ext2NewNameReplace(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PEXT2_CCB Ccb,
                   IN OUT PEXT2_NEW_NAME N)
{
    PIO_STACK_LOCATION  IrpSp = IoGetCurrentIrpStackLocation(IrpContext->Irp);
    PEXT2_FCB           ExistingFcb;
    NTSTATUS            Status;

    Status = Ext2LookupFile(IrpContext, Vcb, &N->FileName, N->TargetMcb, &N->ExistingMcb, 0);
    if (Status == STATUS_NO_SUCH_FILE || Status == STATUS_OBJECT_NAME_NOT_FOUND ||
        Status == STATUS_OBJECT_NAME_INVALID) {
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(Status) || N->ExistingMcb == N->Mcb) {
        return Status;
    }

    if (!IrpSp->Parameters.SetFile.ReplaceIfExists) {
        DEBUG(DL_RES, ("Ext2NewName: %wZ exists\n", &N->ExistingMcb->ShortName));
        return STATUS_OBJECT_NAME_COLLISION;
    }

    ExistingFcb = N->ExistingMcb->Icb->Fcb;
    if (ExistingFcb != NULL && !IsMcbSymLink(N->ExistingMcb)) {
        Status = Ext2IsFileRemovable(IrpContext, Vcb, ExistingFcb, Ccb);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    Ext2NotifySaveName(Vcb, N->ExistingMcb, &N->Existing);
    Status = Ext2DeleteFile(IrpContext, Vcb, ExistingFcb, N->ExistingMcb);
    if (NT_SUCCESS(Status)) {
        N->Replaced = TRUE;
    }
    return Status;
}

static VOID
Ext2NewNameRelease(IN PEXT2_NEW_NAME N)
{
    Ext2FreeFullName(&N->Existing.Path);
    if (N->TargetDcb) {
        Ext2ReleaseFcb(N->TargetDcb);
    }
    if (N->ParentDcb) {
        Ext2ReleaseFcb(N->ParentDcb);
    }
    if (N->ExistingMcb) {
        Ext2DerefMcb(N->ExistingMcb);
    }
    if (N->TargetHeld) {
        Ext2DerefMcb(N->TargetHeld);
    }
}

NTSTATUS
Ext2SetRenameInfo(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_FCB         Fcb,
    PEXT2_CCB         Ccb
)
{
    PFILE_RENAME_INFORMATION FRI = (PFILE_RENAME_INFORMATION)IrpContext->Irp->AssociatedIrp.SystemBuffer;
    EXT2_NEW_NAME       N = { 0 };
    UNICODE_STRING      ShortName = { 0 };
    struct dentry      *NewEntry = NULL;
    __u16               nlink;
    BOOLEAN             Same, Move;
    NTSTATUS            Status, Undo;

    N.Mcb = Ext2CcbName(Fcb, Ccb);
    if (N.Mcb->Inode->i_ino == EXT2_ROOT_INO) {
        return STATUS_INVALID_PARAMETER;
    }

    Status = Ext2NewNamePrepare(IrpContext, Vcb, FRI->FileNameLength, FRI->FileName, &N, &Same);
    if (!NT_SUCCESS(Status) || Same) {
        goto out;
    }
    Move = N.TargetMcb->Inode->i_ino != N.ParentMcb->Inode->i_ino;

    /* the names the cache will need, before anything changes on disk: a
       rename done there and refused here, for want of memory, left the
       cache with the old name of a file the disk had renamed */
    if (!Ext2BuildName(&ShortName, &N.FileName)) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }

    Status = Ext2NewNameReplace(IrpContext, Vcb, Ccb, &N);
    if (!NT_SUCCESS(Status)) {
        goto out;
    }

    /* the old name's entry goes */
    nlink = N.Mcb->Inode->i_nlink;
    Status = Ext2RemoveEntry(IrpContext, Vcb, N.ParentDcb, N.Mcb, NULL);
    if (!NT_SUCCESS(Status)) {
        goto out;
    }

    /* the new one comes */
    Status = Ext2AddEntry(IrpContext, Vcb, N.TargetDcb, N.Mcb->Inode, &N.FileName, &NewEntry);
    if (!NT_SUCCESS(Status)) {
        /* the old name back; failing that, the transaction holds the
           removal alone - the file would be left without a name - so it
           must not commit: the journal stops and the disk keeps the old one */
        Undo = Ext2AddEntry(IrpContext, Vcb, N.ParentDcb, N.Mcb->Inode, &N.Mcb->ShortName, &NewEntry);
        if (!NT_SUCCESS(Undo)) {
            Ext2JournalAbandon(Vcb, Undo);
        }
        goto out;
    }

    if (IsMcbDirectory(N.Mcb)) {
        /* a rename never changes the link count of the inode that moves:
           the remove/add pair above is only right for files (1 -> 0 -> 1);
           for a directory ext3_dec_count stops at 2 and the add would
           leave 3 */
        if (N.Mcb->Inode->i_nlink != nlink) {
            N.Mcb->Inode->i_nlink = nlink;
            Ext2SaveInodeStatus(IrpContext, Vcb, N.Mcb->Inode, &Status);
            if (!NT_SUCCESS(Status)) {
                goto out;
            }
        }

        /* its ".." names the new parent */
        Status = Ext2SetParentEntry(IrpContext, Vcb, Fcb, N.ParentMcb->Inode->i_ino,
                                    N.TargetMcb->Inode->i_ino);
        if (!NT_SUCCESS(Status)) {
            goto out;
        }
    }

    /* Update current dentry from the newly created one. We need keep the original
       dentry to assure children's links are valid if current entry is a directory */
    if (N.Mcb->de) {
        char *np = N.Mcb->de->d_name.name;
        *(N.Mcb->de) = *NewEntry;
        NewEntry->d_name.name = np;
    }

    if (N.Replaced) {
        Ext2NotifySavedRemoval(Vcb, &N.Existing, TRUE);
    }
    Ext2NotifyNameChange(IrpContext, Vcb, N.Mcb, Move ? FILE_ACTION_REMOVED : FILE_ACTION_RENAMED_OLD_NAME);

    /* the new parent and name at once; ShortName gets the old name, freed below */
    Ext2MoveMcb(Vcb, N.Mcb, N.TargetMcb, &ShortName);

    Ext2NotifyNameChange(IrpContext, Vcb, N.Mcb, Move ? FILE_ACTION_ADDED : FILE_ACTION_RENAMED_NEW_NAME);

out:
    if (NewEntry) {
        Ext2FreeEntry(NewEntry);
    }
    Ext2FreeName(&ShortName);
    Ext2NewNameRelease(&N);
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
    PFILE_LINK_INFORMATION FLI = (PFILE_LINK_INFORMATION)IrpContext->Irp->AssociatedIrp.SystemBuffer;
    EXT2_NEW_NAME       N = { 0 };
    PEXT2_MCB           LinkMcb = NULL;
    BOOLEAN             Same;
    NTSTATUS            Status;

    N.Mcb = Ext2CcbName(Fcb, Ccb);
    if (IsMcbDirectory(N.Mcb)) {
        return STATUS_INVALID_PARAMETER;
    }
    /* an immutable or append-only inode gets no further names (Linux) */
    if (Ext4IsSealed(N.Mcb->Inode)) {
        return STATUS_ACCESS_DENIED;
    }

    Status = Ext2NewNamePrepare(IrpContext, Vcb, FLI->FileNameLength, FLI->FileName, &N, &Same);
    if (!NT_SUCCESS(Status) || Same) {
        goto out;
    }

    Status = Ext2NewNameReplace(IrpContext, Vcb, Ccb, &N);
    if (!NT_SUCCESS(Status)) {
        goto out;
    }

    Status = Ext2AddEntry(IrpContext, Vcb, N.TargetDcb, N.Mcb->Inode, &N.FileName, NULL);
    if (!NT_SUCCESS(Status)) {
        goto out;
    }

    if (N.Replaced) {
        Ext2NotifySavedRemoval(Vcb, &N.Existing, TRUE);
    }

    /* the new name, looked up to be announced (and cached) */
    (void)Ext2LookupFile(IrpContext, Vcb, &N.FileName, N.TargetMcb, &LinkMcb, 0);
    if (LinkMcb != NULL) {
        Ext2NotifyNameChange(IrpContext, Vcb, LinkMcb, FILE_ACTION_ADDED);
        Ext2DerefMcb(LinkMcb);
    }

out:
    Ext2NewNameRelease(&N);
    return Status;
}
