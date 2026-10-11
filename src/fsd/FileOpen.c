/**
 * FileOpen.c - IRP_MJ_CREATE for files and directories: open, create, disposition handling.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/ext4_xattr.h>
#include "create_internal.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2CreateFile)
#endif

/*
 * The name exists: may this create have it? FILE_CREATE may not; a
 * directory is only opened, never superseded, overwritten or opened as a
 * file; a file is not opened as a directory.
 */
static NTSTATUS
Ext2CheckExistingName(IN PEXT2_MCB Mcb, IN ULONG Disposition, IN BOOLEAN DirectoryFile,
                      IN BOOLEAN NonDirectoryFile, OUT PULONG_PTR Information)
{
    if (Disposition == FILE_CREATE) {
        *Information = FILE_EXISTS;
        return STATUS_OBJECT_NAME_COLLISION;
    }
    if (IsMcbDirectory(Mcb)) {
        if (Disposition != FILE_OPEN && Disposition != FILE_OPEN_IF) {
            return STATUS_OBJECT_NAME_COLLISION;
        }
        if (NonDirectoryFile) {
            return STATUS_FILE_IS_A_DIRECTORY;
        }
    } else if (DirectoryFile) {
        return STATUS_NOT_A_DIRECTORY;
    }
    *Information = FILE_OPENED;
    return STATUS_SUCCESS;
}

/* the last component of a path, separators at its end left out */
static UNICODE_STRING
Ext2LastComponent(IN PUNICODE_STRING Path)
{
    UNICODE_STRING  Last;
    USHORT          End = Path->Length / sizeof(WCHAR), Start;

    while (End > 0 && Path->Buffer[End - 1] == L'\\') {
        End--;
    }
    for (Start = End; Start > 0 && Path->Buffer[Start - 1] != L'\\'; Start--) {
    }
    Last.Buffer = Path->Buffer + Start;
    Last.Length = Last.MaximumLength = (USHORT)((End - Start) * sizeof(WCHAR));
    return Last;
}

/*
 * The open of a rename's or link's target directory (SL_OPEN_TARGET_DIRECTORY)
 * leaves the file object named after the last component alone - what the
 * rename then reads, as on NTFS. Name is a part of the file object's own
 * name, so it fits.
 */
static VOID
Ext2SetTargetName(IN PFILE_OBJECT FileObject, IN PUNICODE_STRING Name)
{
    RtlZeroMemory(FileObject->FileName.Buffer, FileObject->FileName.MaximumLength);
    FileObject->FileName.Length = Name->Length;
    RtlCopyMemory(FileObject->FileName.Buffer, Name->Buffer, Name->Length);
}

/*
 * What the inode lets this open do, before any Fcb is touched: its mode
 * bits for the caller, the read-only attribute and the chattr flags.
 */
static NTSTATUS
Ext2CheckOpenAccess(IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb, IN ACCESS_MASK DesiredAccess,
                    IN ULONG Options, IN ULONG Disposition)
{
    if (BooleanFlagOn(DesiredAccess, FILE_GENERIC_READ) &&
        !Ext2CheckFileAccess(Vcb, Mcb, Ext2FileCanRead)) {
        return STATUS_ACCESS_DENIED;
    }
    if (!Ext2CheckFileAccess(Vcb, Mcb, Ext2FileCanWrite)) {
        if (BooleanFlagOn(DesiredAccess, FILE_WRITE_DATA | FILE_APPEND_DATA |
                          FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD)) {
            return STATUS_ACCESS_DENIED;
        }
        if (IsFlagOn(Options, FILE_DELETE_ON_CLOSE)) {
            return STATUS_CANNOT_DELETE;
        }
    }

    /* the read-only attribute (owner write bit clear) holds for everybody,
       root included, as on NTFS: no write handle, no delete-on-close. A
       read-only directory still takes new entries - Windows treats that
       attribute as decoration. */
    if (!IsMcbDirectory(Mcb) && !Ext2IsOwnerWritable(Mcb->Inode->i_mode)) {
        if (BooleanFlagOn(DesiredAccess, FILE_WRITE_DATA | FILE_APPEND_DATA)) {
            return STATUS_ACCESS_DENIED;
        }
        if (IsFlagOn(Options, FILE_DELETE_ON_CLOSE)) {
            return STATUS_CANNOT_DELETE;
        }
    }

    /* chattr +i: no data, attribute or EA is written and the file does not
       go (for a directory: nothing is added or removed); chattr +a: a file
       grows at its end only - FILE_APPEND_DATA without FILE_WRITE_DATA - and
       does not go, a directory only takes new entries. Linux refuses these
       to root as well. */
    if (Ext4IsSealed(Mcb->Inode)) {
        ACCESS_MASK Refused = DELETE | FILE_DELETE_CHILD;
        BOOLEAN     Rewrite = Disposition == FILE_SUPERSEDE ||
                              Disposition == FILE_OVERWRITE ||
                              Disposition == FILE_OVERWRITE_IF;

        if (Ext4IsImmutable(Mcb->Inode)) {
            Refused |= FILE_WRITE_DATA | FILE_APPEND_DATA |
                       FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA;
        } else if (!IsMcbDirectory(Mcb)) {
            Refused |= FILE_WRITE_DATA;
        }
        if (BooleanFlagOn(DesiredAccess, Refused) || Rewrite) {
            return STATUS_ACCESS_DENIED;
        }
        if (IsFlagOn(Options, FILE_DELETE_ON_CLOSE)) {
            return STATUS_CANNOT_DELETE;
        }
    }

    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- one create, step by step */

/*
 * One IRP_MJ_CREATE as it goes: what it asks for, then what it found or
 * made. Each pointer below holds a reference of its own, dropped once by
 * Ext2CreateFinish: ParentMcb (the directory looked in), ParentFcb (that
 * directory's Fcb, when a new name is made in it or its open was asked
 * for), OpenMcb (the name opened, from the moment the open claims it),
 * SymLink (a link opened through), Fcb (taken by Ext2CreateClaimFcb). Mcb
 * is the name being worked on: until the open claims it, a step that lets
 * it go drops its reference itself.
 */
typedef struct _EXT2_CREATE {
    PIRP                Irp;
    PIO_STACK_LOCATION  IrpSp;
    ULONG               Options;
    ULONG               Disposition;
    ACCESS_MASK         DesiredAccess;
    ULONG               ShareAccess;
    BOOLEAN             DirectoryFile;
    BOOLEAN             NonDirectoryFile;
    BOOLEAN             OpenTargetDirectory;
    BOOLEAN             NoIntermediateBuffering;
    BOOLEAN             DeleteOnClose;
    BOOLEAN             TemporaryFile;
    BOOLEAN             IsPagingFile;
    BOOLEAN             OpenReparsePoint;

    UNICODE_STRING      FileName;       /* a copy of the file object's name, pool */

    PEXT2_MCB           ParentMcb;
    PEXT2_FCB           ParentFcb;
    PEXT2_MCB           Mcb;
    PEXT2_MCB           OpenMcb;
    PEXT2_MCB           SymLink;
    PEXT2_FCB           Fcb;
    PEXT2_CCB           Ccb;
    ULONG               CcbFlags;

    BOOLEAN             Created;        /* the name was made by this create */
    BOOLEAN             FcbAllocated;   /* and so was the Fcb */
    BOOLEAN             FcbLockHeld;
    BOOLEAN             MainResourceHeld;
} EXT2_CREATE, *PEXT2_CREATE;

/* a medium that turned write-protected: the I/O manager is told to verify it */
static VOID
Ext2RaiseIfWriteProtected(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb)
{
    if (IsFlagOn(Vcb->Flags, VCB_WRITE_PROTECTED)) {
        IoSetHardErrorOrVerifyDevice(IrpContext->Irp, Vcb->Vpb->RealDevice);
        SetFlag(Vcb->Vpb->RealDevice->Flags, DO_VERIFY_VOLUME);
        Ext2RaiseStatus(IrpContext, STATUS_MEDIA_WRITE_PROTECTED);
    }
}

static VOID
Ext2CreateParameters(IN PEXT2_IRP_CONTEXT IrpContext, OUT PEXT2_CREATE C)
{
    RtlZeroMemory(C, sizeof(*C));
    C->Irp = IrpContext->Irp;
    C->IrpSp = IoGetCurrentIrpStackLocation(C->Irp);
    C->Options = C->IrpSp->Parameters.Create.Options;
    C->Disposition = EXT2_CREATE_DISPOSITION(C->Options);
    C->DesiredAccess = C->IrpSp->Parameters.Create.SecurityContext->DesiredAccess;
    C->ShareAccess = C->IrpSp->Parameters.Create.ShareAccess;
    C->DirectoryFile = IsFlagOn(C->Options, FILE_DIRECTORY_FILE);
    C->NonDirectoryFile = IsFlagOn(C->Options, FILE_NON_DIRECTORY_FILE);
    C->NoIntermediateBuffering = IsFlagOn(C->Options, FILE_NO_INTERMEDIATE_BUFFERING);
    C->DeleteOnClose = IsFlagOn(C->Options, FILE_DELETE_ON_CLOSE);
    C->OpenReparsePoint = IsFlagOn(C->Options, FILE_OPEN_REPARSE_POINT);
    C->OpenTargetDirectory = IsFlagOn(C->IrpSp->Flags, SL_OPEN_TARGET_DIRECTORY);
    C->IsPagingFile = IsFlagOn(C->IrpSp->Flags, SL_OPEN_PAGING_FILE);
    C->TemporaryFile = IsFlagOn(C->IrpSp->Parameters.Create.FileAttributes,
                                FILE_ATTRIBUTE_TEMPORARY);
}

/* an open by file id: the "name" is the inode number, of a cached inode */
static NTSTATUS
Ext2CreateLookupId(IN PEXT2_VCB Vcb, IN OUT PEXT2_CREATE C)
{
    PUNICODE_STRING Name = &C->FileName;
    LONGLONG        Id;

    /* 8 bytes (FileInternalInformation) or a 16-byte FILE_ID_128
       (FileIdInformation), low part first */
    if (Name->Length != sizeof(LONGLONG) && Name->Length != sizeof(FILE_ID_128)) {
        return STATUS_INVALID_PARAMETER;
    }
    /* relative to a handle on the volume (the usual way): the related name
       plays no part, the id is absolute */
    if (C->ParentMcb) {
        Ext2DerefMcb(C->ParentMcb);
        C->ParentMcb = NULL;
    }
    Id = *(LONGLONG UNALIGNED *)Name->Buffer;
    if (Name->Length == sizeof(FILE_ID_128) &&
        *(LONGLONG UNALIGNED *)((PUCHAR)Name->Buffer + sizeof(LONGLONG)) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Id <= 0 || Id > (LONGLONG)Vcb->SuperBlock->s_inodes_count) {
        return STATUS_INVALID_PARAMETER;
    }

    /* ext4 cannot find a name from an inode: only cached inodes open this way */
    C->Mcb = Ext2LookupMcbByInode(Vcb, (ULONG)Id);
    DEBUG(DL_INF, ("Ext2CreateFile: open by id %I64x -> %p\n", Id, C->Mcb));
    return C->Mcb ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
}

/*
 * The name looked up as given - relative to the related file object, or
 * by file id. STATUS_SUCCESS with C->Mcb referenced; STATUS_OBJECT_NAME_
 * NOT_FOUND or STATUS_NO_SUCH_FILE when a name is not there; anything else
 * fails the create.
 */
static NTSTATUS
Ext2CreateLookupName(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN OUT PEXT2_CREATE C)
{
    PFILE_OBJECT    FileObject = C->IrpSp->FileObject;
    PUNICODE_STRING Name = &C->FileName;

    if (FileObject->RelatedFileObject) {
        PEXT2_FCB Related = (PEXT2_FCB)FileObject->RelatedFileObject->FsContext;

        /* relative to a handle on the volume itself (open by id does
           that): there is no directory behind it, the root is */
        if (Related && Related->Identifier.Type != EXT2VCB) {
            C->ParentMcb = Related->Mcb;
            Ext2ReferMcb(C->ParentMcb);
        }
    }

    /* no name: the related file object is opened again */
    if (FileObject->FileName.Length == 0) {
        if (C->ParentMcb == NULL) {
            return STATUS_INVALID_PARAMETER;
        }
        C->Mcb = C->ParentMcb;
        Ext2ReferMcb(C->Mcb);
        return STATUS_SUCCESS;
    }

    Name->MaximumLength = FileObject->FileName.MaximumLength;
    Name->Length = FileObject->FileName.Length;
    Name->Buffer = Ext2AllocatePool(PagedPool, Name->MaximumLength, EXT2_FNAME_MAGIC);
    if (!Name->Buffer) {
        DEBUG(DL_ERR, ( "Ext2CreateFile: failed to allocate FileName.\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    INC_MEM_COUNT(PS_FILE_NAME, Name->Buffer, Name->MaximumLength);
    RtlZeroMemory(Name->Buffer, Name->MaximumLength);
    RtlCopyMemory(Name->Buffer, FileObject->FileName.Buffer, Name->Length);

    if (IsFlagOn(C->Options, FILE_OPEN_BY_FILE_ID)) {
        return Ext2CreateLookupId(Vcb, C);
    }

    if (FileObject->RelatedFileObject && Name->Buffer[0] == L'\\') {
        return STATUS_INVALID_PARAMETER;
    }

    /* a name starting with two separators stands for one; more are not a name */
    if (Name->Length > sizeof(WCHAR) && Name->Buffer[1] == L'\\' && Name->Buffer[0] == L'\\') {
        Name->Length -= sizeof(WCHAR);
        RtlMoveMemory(&Name->Buffer[0], &Name->Buffer[1], Name->Length);
        if (Name->Length > sizeof(WCHAR) && Name->Buffer[1] == L'\\' && Name->Buffer[0] == L'\\') {
            return STATUS_OBJECT_NAME_INVALID;
        }
    }

    DEBUG(DL_INF, ( "Ext2CreateFile: %wZ Paging=%d Option: %xh:"
                    "Dir=%d NonDir=%d OpenTarget=%d NC=%d DeleteOnClose=%d\n",
                    Name, C->IsPagingFile, C->Options, C->DirectoryFile,
                    C->NonDirectoryFile, C->OpenTargetDirectory,
                    C->NoIntermediateBuffering, C->DeleteOnClose ));
    DEBUG(DL_RES, ("Ext2CreateFile: Lookup 1st: %wZ at %S\n",
                   Name, C->ParentMcb ? C->ParentMcb->ShortName.Buffer : L" "));

    return Ext2LookupFile(IrpContext, Vcb, Name, C->ParentMcb, &C->Mcb, 0 /* follow links */);
}

/*
 * The name is not there. Its directory is walked to - every component but
 * the last must be one, a symlink standing for its target - and then the
 * last component is made, or, for the open of a rename's or link's target
 * directory, that directory is what is opened. *Exists: another create
 * made the name meanwhile; C->Mcb is then that name, to be opened as an
 * existing one.
 */
static NTSTATUS
Ext2CreateMissingName(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb,
                      IN OUT PEXT2_CREATE C, OUT PBOOLEAN Exists)
{
    UNICODE_STRING  PathName = C->FileName;
    UNICODE_STRING  RealName, RemainName;
    NTSTATUS        Status;

    *Exists = FALSE;
    C->Mcb = NULL;

    /* a name that ends in separators is a directory's */
    while (PathName.Length > 0 && PathName.Buffer[PathName.Length / sizeof(WCHAR) - 1] == L'\\') {
        C->DirectoryFile = TRUE;
        PathName.Length -= sizeof(WCHAR);
        PathName.Buffer[PathName.Length / sizeof(WCHAR)] = 0;
    }

    if (!C->ParentMcb) {
        if (PathName.Buffer[0] != L'\\') {
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        C->ParentMcb = Vcb->McbTree;
        Ext2ReferMcb(C->ParentMcb);
    }

    for (;;) {
        PEXT2_MCB Next = NULL;

        FsRtlDissectName(PathName, &RealName, &RemainName);
        if ((RemainName.Length != 0 && RemainName.Buffer[0] == L'\\') ||
            RealName.Length > EXT2_NAME_LEN * sizeof(WCHAR)) {
            return STATUS_OBJECT_NAME_INVALID;
        }
        if (RemainName.Length == 0) {
            break;
        }

        DEBUG(DL_RES, ("Ext2CreateFile: Lookup 2nd: %wZ\\%wZ\n",
                       &C->ParentMcb->ShortName, &RealName));
        Status = Ext2LookupFile(IrpContext, Vcb, &RealName, C->ParentMcb, &Next, 0);
        if (!NT_SUCCESS(Status)) {
            if (Status == STATUS_NO_SUCH_FILE || Status == STATUS_OBJECT_NAME_NOT_FOUND) {
                Status = STATUS_OBJECT_PATH_NOT_FOUND;
            }
            return Status;
        }
        Ext2DerefMcb(C->ParentMcb);
        C->ParentMcb = Next;
        PathName = RemainName;

        /* a symlink stands for its target, read under LinkLock (a dangling
           one leads nowhere) */
        if (IsMcbSymLink(C->ParentMcb)) {
            PEXT2_MCB Target = Ext2ReferDirectory(Vcb, C->ParentMcb);

            Ext2DerefMcb(C->ParentMcb);
            C->ParentMcb = Target;
            if (C->ParentMcb == NULL) {
                return STATUS_OBJECT_PATH_NOT_FOUND;
            }
        }
    }

    if (FsRtlDoesNameContainWildCards(&RealName) || !Ext2IsNameValid(&RealName)) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    C->ParentFcb = Ext2ReferDcb(Vcb, C->ParentMcb);
    if (!C->ParentFcb) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (C->Disposition == FILE_CREATE || C->Disposition == FILE_SUPERSEDE ||
        C->Disposition == FILE_OPEN_IF || C->Disposition == FILE_OVERWRITE_IF) {

        if (IsVcbReadOnly(Vcb)) {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }
        if (!Ext2CheckFileAccess(Vcb, C->ParentMcb, Ext2FileCanWrite)) {
            return STATUS_ACCESS_DENIED;
        }
        Ext2RaiseIfWriteProtected(IrpContext, Vcb);
        if (C->DirectoryFile && C->TemporaryFile) {
            return STATUS_INVALID_PARAMETER;
        }

        Status = Ext2CreateNewName(IrpContext, Vcb, C->ParentFcb, &RealName, C->DirectoryFile,
                                   C->IrpSp->Parameters.Create.FileAttributes,
                                   &C->Mcb, &C->Created);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        if (!C->Created) {
            *Exists = TRUE;     /* another create made it meanwhile */
            return STATUS_SUCCESS;
        }
        C->Irp->IoStatus.Information = FILE_CREATED;
        return STATUS_SUCCESS;
    }

    if (C->OpenTargetDirectory) {
        if (IsVcbReadOnly(Vcb)) {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }

        /* The directory is opened; the open takes its Fcb as its own (a
           reference of its own, Ext2CreateClaimFcb). Taking ParentFcb for
           it instead left a refused open releasing one reference twice. */
        Ext2SetTargetName(C->IrpSp->FileObject, &RealName);
        C->Mcb = C->ParentFcb->Mcb;
        Ext2ReferMcb(C->Mcb);
        C->Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
        return STATUS_SUCCESS;
    }

    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/*
 * The name exists, C->Mcb referenced. The open of a rename's or link's
 * target directory opens the directory above it; any other open must suit
 * the disposition and the kind of file. A refusal lets the name go.
 */
static NTSTATUS
Ext2CreateExistingName(IN PEXT2_VCB Vcb, IN OUT PEXT2_CREATE C)
{
    NTSTATUS Status;

    if (C->OpenTargetDirectory) {
        UNICODE_STRING  Last = Ext2LastComponent(&C->FileName);
        PEXT2_MCB       Dir;

        if (IsVcbReadOnly(Vcb)) {
            Ext2DerefMcb(C->Mcb);
            C->Mcb = NULL;
            return STATUS_MEDIA_WRITE_PROTECTED;
        }

        /* the target's directory; its name may be going at this moment
           (Ext2ReferParent) */
        Dir = Ext2ReferParent(Vcb, C->Mcb);
        Ext2DerefMcb(C->Mcb);
        C->Mcb = Dir;
        if (Dir == NULL) {
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        Ext2SetTargetName(C->IrpSp->FileObject, &Last);
        C->Irp->IoStatus.Information = FILE_EXISTS;
        return STATUS_SUCCESS;
    }

    Status = Ext2CheckExistingName(C->Mcb, C->Disposition, C->DirectoryFile,
                                   C->NonDirectoryFile, &C->Irp->IoStatus.Information);
    if (!NT_SUCCESS(Status)) {
        Ext2DerefMcb(C->Mcb);
        C->Mcb = NULL;
    }
    return Status;
}

/* the name to open: found, made, or found made by another create meanwhile */
static NTSTATUS
Ext2CreateFindName(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN OUT PEXT2_CREATE C)
{
    NTSTATUS    Status = Ext2CreateLookupName(IrpContext, Vcb, C);
    BOOLEAN     Exists;

    if ((Status == STATUS_OBJECT_NAME_NOT_FOUND || Status == STATUS_NO_SUCH_FILE) &&
        !IsFlagOn(C->Options, FILE_OPEN_BY_FILE_ID)) {
        Status = Ext2CreateMissingName(IrpContext, Vcb, C, &Exists);
        if (!NT_SUCCESS(Status) || !Exists) {
            return Status;
        }
    } else if (!NT_SUCCESS(Status)) {
        return Status;
    }
    return Ext2CreateExistingName(Vcb, C);
}

/*
 * The open claims its name and finds or makes its Fcb, under FcbLock. A
 * symlink is opened as its target, or as the link with FILE_OPEN_REPARSE_
 * POINT. The name's reference becomes the create's (C->OpenMcb) before any
 * check can refuse the open; the Fcb is referenced before the lock goes.
 */
static NTSTATUS
Ext2CreateClaimFcb(IN PEXT2_VCB Vcb, IN OUT PEXT2_CREATE C)
{
    PEXT2_MCB   Mcb;
    PEXT2_FCB   Fcb;
    NTSTATUS    Status;

    ASSERT(C->Mcb != NULL && C->Mcb->Refercount > 0);

    /* Shared: an open of a file that has its Fcb already - the common
       case - only reads Icb->Fcb and takes a reference (interlocked), and
       everything that changes either (a new Fcb, the reaper, the last
       close) holds the lock exclusively. Opens of the volume run side by
       side; exclusive was one open at a time. */
    ExAcquireResourceSharedLite(&Vcb->FcbLock, TRUE);
    C->FcbLockHeld = TRUE;

    /* Refer the target of a symlink, so both are referenced. Plain opens
       run side by side (shared volume resource): the link's type and
       Target are read and, for a dangling link, rewritten under LinkLock,
       which guards every such change - otherwise two opens of one dangling
       link would both drop its target. The flag test outside the lock only
       decides whether to take it: a name becomes a symlink under the
       exclusive volume resource alone (Ext2SetReparsePoint), never while
       an open holds it shared, and the test is repeated inside. */
    if (IsMcbSymLink(C->Mcb)) {
        KeEnterCriticalRegion();
        ExAcquireResourceExclusiveLite(&Vcb->LinkLock, TRUE);
        if (IsMcbSymLink(C->Mcb)) {
            Mcb = C->Mcb;
            if (C->OpenReparsePoint) {
                C->CcbFlags = CCB_OPEN_REPARSE_POINT;
            } else if (Mcb->Target == NULL || IsFileDeleted(Mcb->Target)) {
                /* dangling now: no longer a directory (or file) of the
                   target's kind, just the link itself - still a link on
                   disk, shown as one (NTFS lists a dangling symlink as a
                   reparse point too) */
                SetLongFlag(Mcb->Flags, MCB_TYPE_SPECIAL);
                ClearLongFlag(Mcb->Flags, MCB_TYPE_SYMLINK);
                ClearFlag(Mcb->FileAttr, FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NORMAL);
                SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_REPARSE_POINT);
                if (Mcb->Target) {
                    Ext2DerefMcb(Mcb->Target);
                    Mcb->Target = NULL;
                }
            } else {
                C->SymLink = Mcb;
                C->Mcb = Mcb->Target;
                Ext2ReferMcb(C->Mcb);
                ASSERT(!IsMcbSymLink(C->Mcb));
            }
        }
        ExReleaseResourceLite(&Vcb->LinkLock);
        KeLeaveCriticalRegion();
    }
    Mcb = C->Mcb;

    /* A dangling link is demoted to a special file above or at lookup, but
       on disk it is still a link and its text is still readable (readlink
       on Linux, the [target] NTFS lists): the reparse-point open is decided
       by the inode, not by the state of the name cache. */
    if (C->OpenReparsePoint && C->SymLink == NULL && IsInodeSymLink(Mcb->Inode)) {
        C->CcbFlags = CCB_OPEN_REPARSE_POINT;
    }

    /* From here on the create owns the reference the lookup (or the symlink
       step above) took on Mcb: Ext2CreateFinish drops it on every way out,
       the Ccb taking its own on success. Claimed only after the access and
       delete-pending checks, each open they refused leaked one reference -
       the name, its Icb and in the end the volume could never be freed. The
       Fcb may be reached through another hard link, which is why the name
       is held until the Ccb has taken its reference. */
    C->OpenMcb = Mcb;

    Status = Ext2CheckOpenAccess(Vcb, Mcb, C->DesiredAccess, C->Options, C->Disposition);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    /* a name already marked for deletion (DeleteFile while other handles
       keep the file alive) takes no new handles */
    Fcb = Mcb->Icb->Fcb;
    if (IsFlagOn(Mcb->Flags, MCB_DELETE_PENDING) ||
        (Fcb && IsFlagOn(Fcb->Flags, FCB_DELETE_PENDING))) {
        return STATUS_DELETE_PENDING;
    }
    if (Fcb == NULL) {
        /* none yet: making one takes the lock exclusively, and in between
           another open may have made it, or the name been marked for
           deletion */
        ExReleaseResourceLite(&Vcb->FcbLock);
        ExAcquireResourceExclusiveLite(&Vcb->FcbLock, TRUE);
        Fcb = Mcb->Icb->Fcb;
        if (IsFlagOn(Mcb->Flags, MCB_DELETE_PENDING) ||
            (Fcb && IsFlagOn(Fcb->Flags, FCB_DELETE_PENDING))) {
            return STATUS_DELETE_PENDING;
        }
    }
    if (Fcb == NULL) {
        Fcb = Ext2AllocateFcb(Vcb, Mcb);
        if (Fcb == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        C->FcbAllocated = TRUE;
    } else if (C->IsPagingFile) {
        return STATUS_SHARING_VIOLATION;
    }

    /* referenced before the lock goes: a close could free it otherwise */
    Ext2ReferXcb(&Fcb->ReferenceCount);
    C->Fcb = Fcb;
    ExReleaseResourceLite(&Vcb->FcbLock);
    C->FcbLockHeld = FALSE;
    return STATUS_SUCCESS;
}

/*
 * With the Fcb exclusive: the file is still there, may go on close if
 * asked, oplocks are broken or waited for (*OpPostIrp: the request waits
 * for a break), a new file gets set up, and the sharing modes agree.
 */
static NTSTATUS
Ext2CreateCheckFcb(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb,
                   IN OUT PEXT2_CREATE C, OUT PBOOLEAN OpPostIrp)
{
    PEXT2_FCB       Fcb = C->Fcb;
    PFILE_OBJECT    FileObject = C->IrpSp->FileObject;
    NTSTATUS        Status;

    if (IsInodeDeleted(Fcb)) {
        return STATUS_FILE_DELETED;
    }

    if (C->DeleteOnClose && C->SymLink == NULL) {
        Status = Ext2IsFileRemovable(IrpContext, Vcb, Fcb, NULL);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    /* a batch oplock on a file open already is broken if the sharing
       check fails */
    if (!C->FcbAllocated && !IsDirectory(Fcb) && FsRtlCurrentBatchOplock(&Fcb->Oplock)) {
        C->Irp->IoStatus.Information = FILE_OPBATCH_BREAK_UNDERWAY;
        Status = FsRtlCheckOplock(&Fcb->Oplock, IrpContext->Irp, IrpContext,
                                  Ext2OplockComplete, Ext2LockIrp);
        if (Status != STATUS_SUCCESS && Status != STATUS_OPLOCK_BREAK_IN_PROGRESS) {
            *OpPostIrp = TRUE;
            return Status;
        }
    }

    if (C->Created) {
        /* just made: EAs, label, "." and ".." (a failure takes the name
           away again, in Ext2CreateFinish) */
        Status = Ext2InitializeCreatedFile(IrpContext, Vcb, Fcb, C->Mcb,
                                           C->ParentFcb->Mcb, C->DirectoryFile);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    } else if (C->DeleteOnClose) {
        if (IsVcbReadOnly(Vcb)) {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }
        Ext2RaiseIfWriteProtected(IrpContext, Vcb);
    } else if (!IsDirectory(Fcb) && IsFlagOn(FileObject->Flags, FO_NO_INTERMEDIATE_BUFFERING)) {
        /* the first non-cached open of a file only cached so far: what the
           cache holds is written and dropped */
        Fcb->Header.IsFastIoPossible = FastIoIsPossible;
        if (Fcb->SectionObject.DataSectionObject != NULL &&
            Fcb->NonCachedOpenCount == Fcb->OpenHandleCount) {
            if (!IsVcbReadOnly(Vcb)) {
                CcFlushCache(&Fcb->SectionObject, NULL, 0, NULL);
                ClearLongFlag(Fcb->Flags, FCB_FILE_MODIFIED);
            }
            CcPurgeCacheSection(&Fcb->SectionObject, NULL, 0, FALSE);
        }
    }

    if (!IsDirectory(Fcb)) {
        if (!IsVcbReadOnly(Vcb) && !C->IsPagingFile) {
            if (C->Disposition == FILE_SUPERSEDE) {
                C->DesiredAccess |= DELETE;
            } else if (C->Disposition == FILE_OVERWRITE || C->Disposition == FILE_OVERWRITE_IF) {
                C->DesiredAccess |= FILE_WRITE_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
            }
        }
        if (!C->FcbAllocated) {
            Status = FsRtlCheckOplock(&Fcb->Oplock, IrpContext->Irp, IrpContext,
                                      Ext2OplockComplete, Ext2LockIrp);
            if (Status != STATUS_SUCCESS && Status != STATUS_OPLOCK_BREAK_IN_PROGRESS) {
                *OpPostIrp = TRUE;
                return Status;
            }
        }
    }

    if (Fcb->OpenHandleCount > 0) {
        return IoCheckShareAccess(C->DesiredAccess, C->ShareAccess, FileObject,
                                  &Fcb->ShareAccess, TRUE);
    }
    IoSetShareAccess(C->DesiredAccess, C->ShareAccess, FileObject, &Fcb->ShareAccess);
    return STATUS_SUCCESS;
}

/* the handle: a Ccb on the Fcb, counted, the file object pointing at both */
static NTSTATUS
Ext2CreateAttachCcb(IN PEXT2_VCB Vcb, IN OUT PEXT2_CREATE C)
{
    PEXT2_FCB       Fcb = C->Fcb;
    PFILE_OBJECT    FileObject = C->IrpSp->FileObject;
    PEXT2_CCB       Ccb;

    Ccb = Ext2AllocateCcb(C->CcbFlags, C->Mcb, C->SymLink);
    if (!Ccb) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    C->Ccb = Ccb;

    /* what the open asked for and what a privilege granted it already
       (backup and restore move the rights they cover there) */
    Ccb->Access = C->DesiredAccess;
    if (C->IrpSp->Parameters.Create.SecurityContext->AccessState) {
        Ccb->Access |= C->IrpSp->Parameters.Create.SecurityContext->AccessState->PreviouslyGrantedAccess;
    }

    if (C->DeleteOnClose) {
        SetLongFlag(Ccb->Flags, CCB_DELETE_ON_CLOSE);
    }
    /* The cursor belongs to the opened inode. Htree reads through this
       dentry; the link's text is not the target directory's index. The
       separately referenced Ccb->SymLink retains the name opened through. */
    Ccb->filp.f_dentry = Fcb->Mcb->de;
    ASSERT(Ccb->filp.f_dentry->d_inode == Fcb->Inode);
    Ccb->filp.f_version = Fcb->Mcb->Inode->i_version;
    Ext2ReferXcb(&Fcb->OpenHandleCount);
    Ext2ReferXcb(&Fcb->ReferenceCount);
    Ext2ReferXcb(&Fcb->CcbCount);
    Ccb->FileObject = FileObject;
    InsertTailList(&Fcb->CcbList, &Ccb->FcbLink);

    if (!IsDirectory(Fcb)) {
        if (C->NoIntermediateBuffering) {
            Fcb->NonCachedOpenCount++;
        }
        SetFlag(FileObject->Flags, FO_CACHE_SUPPORTED);
    }

    Ext2ReferXcb(&Vcb->OpenHandleCount);
    Ext2ReferXcb(&Vcb->ReferenceCount);

    FileObject->FsContext = (void *)Fcb;
    FileObject->FsContext2 = (void *)Ccb;
    FileObject->PrivateCacheMap = NULL;
    FileObject->SectionObjectPointer = &(Fcb->SectionObject);

    DEBUG(DL_INF, ( "Ext2CreateFile: %wZ OpenCount=%u ReferCount=%u NonCachedCount=%u\n",
                    &Fcb->Mcb->ShortName, Fcb->OpenHandleCount, Fcb->ReferenceCount,
                    Fcb->NonCachedOpenCount));
    return STATUS_SUCCESS;
}

/*
 * What an open that succeeded brings about: a new name is announced; an
 * existing file has its mapped images flushed before a write handle, and a
 * supersede or overwrite rewrites it.
 */
static NTSTATUS
Ext2CreateAfterOpen(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN OUT PEXT2_CREATE C)
{
    PEXT2_FCB   Fcb = C->Fcb;
    BOOLEAN     Rewrite = C->Disposition == FILE_SUPERSEDE ||
                          C->Disposition == FILE_OVERWRITE ||
                          C->Disposition == FILE_OVERWRITE_IF;
    NTSTATUS    Status;

    if (C->Created) {
        Ext2NotifyNameChange(IrpContext, Vcb, Fcb->Mcb, FILE_ACTION_ADDED);
        return STATUS_SUCCESS;
    }
    if (IsDirectory(Fcb)) {
        return STATUS_SUCCESS;
    }

    if ((C->DeleteOnClose || IsFlagOn(C->DesiredAccess, FILE_WRITE_DATA) ||
         C->Disposition == FILE_OVERWRITE || C->Disposition == FILE_OVERWRITE_IF) &&
        !MmFlushImageSection(&Fcb->SectionObject, MmFlushForWrite)) {
        return C->DeleteOnClose ? STATUS_CANNOT_DELETE : STATUS_SHARING_VIOLATION;
    }

    if (!Rewrite) {
        return STATUS_SUCCESS;
    }
    if (C->SymLink != NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (IsVcbReadOnly(Vcb)) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    Ext2RaiseIfWriteProtected(IrpContext, Vcb);

    Status = Ext2SupersedeOrOverWriteFile(IrpContext, C->IrpSp->FileObject, Vcb, Fcb,
                                          &C->Irp->Overlay.AllocationSize, C->Disposition);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Ext2NotifyReportChange(IrpContext, Vcb, Fcb->Mcb,
                           FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES |
                           FILE_NOTIFY_CHANGE_SIZE,
                           FILE_ACTION_MODIFIED);
    C->Irp->IoStatus.Information = C->Disposition == FILE_SUPERSEDE ? FILE_SUPERSEDED :
                                                                      FILE_OVERWRITTEN;
    return STATUS_SUCCESS;
}

/* the open itself, the Fcb exclusive: checks, the handle, what follows */
static NTSTATUS
Ext2CreateOpenFcb(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb,
                  IN OUT PEXT2_CREATE C, OUT PBOOLEAN OpPostIrp)
{
    NTSTATUS Status;

    ExAcquireResourceExclusiveLite(&C->Fcb->MainResource, TRUE);
    C->MainResourceHeld = TRUE;
    ASSERT(C->Fcb->Mcb->Refercount > 0);

    Status = Ext2CreateCheckFcb(IrpContext, Vcb, C, OpPostIrp);
    if (*OpPostIrp || !NT_SUCCESS(Status)) {
        return Status;
    }
    Status = Ext2CreateAttachCcb(Vcb, C);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    return Ext2CreateAfterOpen(IrpContext, Vcb, C);
}

/*
 * Every way out of a create. The locks go; on a failure - an exception
 * included, which leaves the status as it was before it - what this create
 * attached or made is taken back: the handle, the allocation it asked for,
 * the name it made. Then each reference it holds is dropped, once.
 */
static VOID
Ext2CreateFinish(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN OUT PEXT2_CREATE C,
                 IN BOOLEAN Failed, IN NTSTATUS Status)
{
    PFILE_OBJECT    FileObject = C->IrpSp->FileObject;
    PEXT2_FCB       Fcb = C->Fcb;

    UNREFERENCED_PARAMETER(Status);     /* traced only */

    if (C->FcbLockHeld) {
        ExReleaseResourceLite(&Vcb->FcbLock);
    }
    if (C->ParentMcb) {
        Ext2DerefMcb(C->ParentMcb);
    }

    if (Failed) {

        if (C->Ccb != NULL) {
            ASSERT(Fcb != NULL && Fcb->Mcb != NULL);
            DEBUG(DL_ERR, ("Ext2CreateFile: failed to create %wZ status = %xh\n",
                           &Fcb->Mcb->ShortName, Status));

            Ext2DerefXcb(&Fcb->OpenHandleCount);
            Ext2DerefXcb(&Fcb->ReferenceCount);
            Ext2DerefXcb(&Fcb->CcbCount);
            RemoveEntryList(&C->Ccb->FcbLink);

            if (!IsDirectory(Fcb)) {
                if (C->NoIntermediateBuffering) {
                    Fcb->NonCachedOpenCount--;
                } else {
                    ClearFlag(FileObject->Flags, FO_CACHE_SUPPORTED);
                }
            }

            Ext2DerefXcb(&Vcb->OpenHandleCount);
            Ext2DerefXcb(&Vcb->ReferenceCount);

            IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);

            FileObject->FsContext = NULL;
            FileObject->FsContext2 = NULL;
            FileObject->PrivateCacheMap = NULL;
            FileObject->SectionObjectPointer = NULL;

            Ext2FreeCcb(Vcb, C->Ccb);
            C->Ccb = NULL;
        }

        if (Fcb != NULL) {
            if (IsFlagOn(Fcb->Flags, FCB_ALLOC_IN_CREATE)) {
                LARGE_INTEGER Size;

                ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
                __try {
                    /* the allocation of a create that failed; what stays
                       past the end is legal and freed with the file */
                    Size.QuadPart = 0;
                    (void)Ext2TruncateFile(IrpContext, Vcb, Fcb->Mcb, &Size);
                } __finally {
                    ExReleaseResourceLite(&Fcb->PagingIoResource);
                }
            }

            if (C->Created) {
                /* a file the failed create leaves behind is whole, only
                   unwanted: said, not hidden */
                NTSTATUS Undo = Ext2DeleteFile(IrpContext, Vcb, Fcb, C->Mcb);

                if (!NT_SUCCESS(Undo)) {
                    DbgPrint("ext4: a failed create left %wZ behind (%08x)\n",
                             &C->Mcb->ShortName, Undo);
                }
            }
        }
    }

    if (C->MainResourceHeld) {
        ExReleaseResourceLite(&Fcb->MainResource);
    }

    if (C->FileName.Buffer) {
        DEC_MEM_COUNT(PS_FILE_NAME, C->FileName.Buffer, C->FileName.MaximumLength);
        Ext2FreePool(C->FileName.Buffer, EXT2_FNAME_MAGIC);
    }

    if (Fcb) {
        Ext2ReleaseFcb(Fcb);
    }
    if (C->ParentFcb) {
        Ext2ReleaseFcb(C->ParentFcb);
    }

    /* a link opened through, and the name opened: the Ccb took references
       of its own on success */
    if (C->SymLink) {
        Ext2DerefMcb(C->SymLink);
    }
    if (C->OpenMcb) {
        Ext2DerefMcb(C->OpenMcb);
    }
}

NTSTATUS
Ext2CreateFile(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PBOOLEAN          OpPostIrp
)
{
    EXT2_CREATE C;
    NTSTATUS    Status = STATUS_UNSUCCESSFUL;

    Ext2CreateParameters(IrpContext, &C);
    *OpPostIrp = FALSE;

    __try {

        Status = Ext2CreateFindName(IrpContext, Vcb, &C);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        Status = Ext2CreateClaimFcb(Vcb, &C);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        Status = Ext2CreateOpenFcb(IrpContext, Vcb, &C, OpPostIrp);

    } __finally {

        Ext2CreateFinish(IrpContext, Vcb, &C, !NT_SUCCESS(Status) || AbnormalTermination(), Status);
    }

    return Status;
}
