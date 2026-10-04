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

NTSTATUS
Ext2CreateFile(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PBOOLEAN          OpPostIrp
)
{
    NTSTATUS            Status = STATUS_UNSUCCESSFUL;
    PIO_STACK_LOCATION  IrpSp;
    PEXT2_FCB           Fcb = NULL;
    PEXT2_MCB           Mcb = NULL;
    PEXT2_MCB           OpenMcb = NULL;
    PEXT2_MCB           SymLink = NULL;
    PEXT2_CCB           Ccb = NULL;

    PEXT2_FCB           ParentFcb = NULL;
    PEXT2_MCB           ParentMcb = NULL;

    UNICODE_STRING      FileName;
    PIRP                Irp;

    ULONG               Options;
    ULONG               CreateDisposition;

    BOOLEAN             bFcbAllocated = FALSE;
    BOOLEAN             bCreated = FALSE;

    BOOLEAN             bMainResourceAcquired = FALSE;
    BOOLEAN             bFcbLockAcquired = FALSE;



    BOOLEAN             OpenDirectory;
    BOOLEAN             OpenTargetDirectory;
    BOOLEAN             CreateDirectory;
    BOOLEAN             SequentialOnly;
    BOOLEAN             NoIntermediateBuffering;
    BOOLEAN             IsPagingFile;
    BOOLEAN             DirectoryFile;
    BOOLEAN             NonDirectoryFile;
    BOOLEAN             NoEaKnowledge;
    BOOLEAN             DeleteOnClose;
    BOOLEAN             TemporaryFile;
    BOOLEAN             CaseSensitive;
    BOOLEAN             OpenReparsePoint;

    ACCESS_MASK         DesiredAccess;
    ULONG               ShareAccess;
    ULONG               CcbFlags = 0;

    RtlZeroMemory(&FileName, sizeof(UNICODE_STRING));

    Irp = IrpContext->Irp;
    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    Options  = IrpSp->Parameters.Create.Options;

    DirectoryFile = IsFlagOn(Options, FILE_DIRECTORY_FILE);
    OpenTargetDirectory = IsFlagOn(IrpSp->Flags, SL_OPEN_TARGET_DIRECTORY);

    NonDirectoryFile = IsFlagOn(Options, FILE_NON_DIRECTORY_FILE);
    SequentialOnly = IsFlagOn(Options, FILE_SEQUENTIAL_ONLY);
    NoIntermediateBuffering = IsFlagOn( Options, FILE_NO_INTERMEDIATE_BUFFERING );
    NoEaKnowledge = IsFlagOn(Options, FILE_NO_EA_KNOWLEDGE);
    DeleteOnClose = IsFlagOn(Options, FILE_DELETE_ON_CLOSE);

    /* Try to open reparse point (symlink) itself ? */
    OpenReparsePoint = IsFlagOn(Options, FILE_OPEN_REPARSE_POINT);

    CaseSensitive = IsFlagOn(IrpSp->Flags, SL_CASE_SENSITIVE);

    TemporaryFile = IsFlagOn(IrpSp->Parameters.Create.FileAttributes,
                             FILE_ATTRIBUTE_TEMPORARY );

    CreateDisposition = (Options >> 24) & 0x000000ff;

    IsPagingFile = IsFlagOn(IrpSp->Flags, SL_OPEN_PAGING_FILE);

    CreateDirectory = (BOOLEAN)(DirectoryFile &&
                                ((CreateDisposition == FILE_CREATE) ||
                                 (CreateDisposition == FILE_OPEN_IF)));

    OpenDirectory   = (BOOLEAN)(DirectoryFile &&
                                ((CreateDisposition == FILE_OPEN) ||
                                 (CreateDisposition == FILE_OPEN_IF)));

    DesiredAccess = IrpSp->Parameters.Create.SecurityContext->DesiredAccess;
    ShareAccess   = IrpSp->Parameters.Create.ShareAccess;

    *OpPostIrp = FALSE;

    __try {

        FileName.MaximumLength = IrpSp->FileObject->FileName.MaximumLength;
        FileName.Length = IrpSp->FileObject->FileName.Length;

        if (IrpSp->FileObject->RelatedFileObject) {
            ParentFcb = (PEXT2_FCB)(IrpSp->FileObject->RelatedFileObject->FsContext);
            /* relative to a handle on the volume itself (open by id does
               that): there is no directory behind it, the root is */
            if (ParentFcb && ParentFcb->Identifier.Type == EXT2VCB) {
                ParentFcb = NULL;
            }
        }

        if (ParentFcb) {
            ParentMcb = ParentFcb->Mcb;
            Ext2ReferMcb(ParentMcb);
            ParentFcb = NULL;
        }

        if (FileName.Length == 0) {

            if (ParentMcb) {
                Mcb = ParentMcb;
                Ext2ReferMcb(Mcb);
                Status = STATUS_SUCCESS;
                goto McbExisting;
            } else {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
        }

        FileName.Buffer = Ext2AllocatePool(
                              PagedPool,
                              FileName.MaximumLength,
                              EXT2_FNAME_MAGIC
                          );

        if (!FileName.Buffer) {
            DEBUG(DL_ERR, ( "Ext2CreateFile: failed to allocate FileName.\n"));
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        INC_MEM_COUNT(PS_FILE_NAME, FileName.Buffer, FileName.MaximumLength);

        RtlZeroMemory(FileName.Buffer, FileName.MaximumLength);
        RtlCopyMemory(FileName.Buffer, IrpSp->FileObject->FileName.Buffer, FileName.Length);

        if (IsFlagOn(Options, FILE_OPEN_BY_FILE_ID)) {

            LONGLONG    Id;

            /* the "name" is the inode number: 8 bytes (FileInternalInformation)
               or a 16-byte FILE_ID_128 (FileIdInformation), low part first */
            if (FileName.Length != sizeof(LONGLONG) && FileName.Length != sizeof(FILE_ID_128)) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            /* relative to a handle on the volume (the usual way): the
               related name plays no part, the id is absolute */
            if (ParentMcb) {
                Ext2DerefMcb(ParentMcb);
                ParentMcb = NULL;
            }
            Id = *(LONGLONG UNALIGNED *)FileName.Buffer;
            if (FileName.Length == sizeof(FILE_ID_128) &&
                *(LONGLONG UNALIGNED *)((PUCHAR)FileName.Buffer + sizeof(LONGLONG)) != 0) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            if (Id <= 0 || Id > (LONGLONG)Vcb->SuperBlock->s_inodes_count) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }

            Mcb = Ext2LookupMcbByInode(Vcb, (ULONG)Id);
            DEBUG(DL_INF, ("Ext2CreateFile: open by id %I64x -> %p\n", Id, Mcb));
            if (Mcb == NULL) {
                /* ext4 cannot find a name from an inode; only cached inodes
                   can be opened this way */
                Status = STATUS_OBJECT_NAME_NOT_FOUND;
                __leave;
            }
            Status = STATUS_SUCCESS;
            goto McbExisting;
        }

        if (IrpSp->FileObject->RelatedFileObject && FileName.Buffer[0] == L'\\') {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        if ((FileName.Length > sizeof(WCHAR)) &&
                (FileName.Buffer[1] == L'\\') &&
                (FileName.Buffer[0] == L'\\')) {

            FileName.Length -= sizeof(WCHAR);

            RtlMoveMemory( &FileName.Buffer[0],
                           &FileName.Buffer[1],
                           FileName.Length );

            /* Bad Name if there are still beginning backslashes. */

            if ((FileName.Length > sizeof(WCHAR)) &&
                    (FileName.Buffer[1] == L'\\') &&
                    (FileName.Buffer[0] == L'\\')) {

                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }
        }

        DEBUG(DL_INF, ( "Ext2CreateFile: %wZ Paging=%d Option: %xh:"
                        "Dir=%d NonDir=%d OpenTarget=%d NC=%d DeleteOnClose=%d\n",
                        &FileName, IsPagingFile, IrpSp->Parameters.Create.Options,
                        DirectoryFile, NonDirectoryFile, OpenTargetDirectory,
                        NoIntermediateBuffering, DeleteOnClose ));

        DEBUG(DL_RES, ("Ext2CreateFile: Lookup 1st: %wZ at %S\n",
                       &FileName, ParentMcb ? ParentMcb->FullName.Buffer : L" "));
        Status = Ext2LookupFile(
                     IrpContext,
                     Vcb,
                     &FileName,
                     ParentMcb,
                     &Mcb,
                     0  /* always follow link */
                    );
McbExisting:

        if (!NT_SUCCESS(Status)) {

            UNICODE_STRING  PathName;
            UNICODE_STRING  RealName;
            UNICODE_STRING  RemainName;


            PathName = FileName;
            Mcb = NULL;

            /* here we've found the target file, but it's not matched. */
            if (STATUS_OBJECT_NAME_NOT_FOUND != Status &&
                STATUS_NO_SUCH_FILE != Status) {
                __leave;
            }

            while (PathName.Length > 0 &&
                   PathName.Buffer[PathName.Length/2 - 1] == L'\\') {
                DirectoryFile = TRUE;
                PathName.Length -= 2;
                PathName.Buffer[PathName.Length / 2] = 0;
            }

            if (!ParentMcb) {
                if (PathName.Buffer[0] != L'\\') {
                    Status = STATUS_OBJECT_PATH_NOT_FOUND;
                    __leave;
                } else {
                    ParentMcb = Vcb->McbTree;
                    Ext2ReferMcb(ParentMcb);
                }
            }

Dissecting:

            FsRtlDissectName(PathName, &RealName, &RemainName);

            if (((RemainName.Length != 0) && (RemainName.Buffer[0] == L'\\')) ||
                    (RealName.Length >= 256 * sizeof(WCHAR))) {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }

            if (RemainName.Length != 0) {

                PEXT2_MCB   RetMcb = NULL;

                DEBUG(DL_RES, ("Ext2CreateFile: Lookup 2nd: %wZ\\%wZ\n",
                               &ParentMcb->FullName, &RealName));

                Status = Ext2LookupFile (
                             IrpContext,
                             Vcb,
                             &RealName,
                             ParentMcb,
                             &RetMcb,
                             0);

                /* quit name resolving loop */
                if (!NT_SUCCESS(Status)) {
                    if (Status == STATUS_NO_SUCH_FILE ||
                        Status == STATUS_OBJECT_NAME_NOT_FOUND) {
                        Status = STATUS_OBJECT_PATH_NOT_FOUND;
                    }
                    __leave;
                }

                /* deref ParentMcb */
                Ext2DerefMcb(ParentMcb);

                /* RetMcb is already referenced */
                ParentMcb = RetMcb;
                PathName  = RemainName;

                /* a symlink stands for its target, read under LinkLock
                   (a dangling one leads nowhere) */
                if (IsMcbSymLink(ParentMcb)) {
                    PEXT2_MCB Target = Ext2ReferDirectory(Vcb, ParentMcb);

                    Ext2DerefMcb(ParentMcb);
                    ParentMcb = Target;
                    if (ParentMcb == NULL) {
                        Status = STATUS_OBJECT_PATH_NOT_FOUND;
                        __leave;
                    }
                }

                goto Dissecting;
            }

            /* is name valid */
            if ( FsRtlDoesNameContainWildCards(&RealName) ||
                    !Ext2IsNameValid(&RealName)) {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }

            /* get the ParentFcb, allocate it if needed ... */
            ParentFcb = Ext2ReferDcb(Vcb, ParentMcb);
            if (!ParentFcb) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }

            /* We need to create a new one ? */
            if ((CreateDisposition == FILE_CREATE ) ||
                    (CreateDisposition == FILE_SUPERSEDE) ||
                    (CreateDisposition == FILE_OPEN_IF) ||
                    (CreateDisposition == FILE_OVERWRITE_IF)) {

                if (IsVcbReadOnly(Vcb)) {
                    Status = STATUS_MEDIA_WRITE_PROTECTED;
                    __leave;
                }

                if (!Ext2CheckFileAccess(Vcb, ParentMcb, Ext2FileCanWrite)) {
                    Status = STATUS_ACCESS_DENIED;
                    __leave;
                }

                if (IsFlagOn(Vcb->Flags, VCB_WRITE_PROTECTED)) {
                    IoSetHardErrorOrVerifyDevice( IrpContext->Irp,
                                                  Vcb->Vpb->RealDevice );
                    SetFlag(Vcb->Vpb->RealDevice->Flags, DO_VERIFY_VOLUME);
                    Ext2RaiseStatus(IrpContext, STATUS_MEDIA_WRITE_PROTECTED);
                }

                if (DirectoryFile) {
                    if (TemporaryFile) {
                        Status = STATUS_INVALID_PARAMETER;
                        __leave;
                    }
                }

                Status = Ext2CreateNewName(IrpContext, Vcb, ParentFcb, &RealName, DirectoryFile,
                                           IrpSp->Parameters.Create.FileAttributes,
                                           &Mcb, &bCreated);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
                if (!bCreated) {
                    /* another create made it meanwhile: open what is there */
                    goto McbExisting;
                }
                Irp->IoStatus.Information = FILE_CREATED;

            } else if (OpenTargetDirectory) {

                if (IsVcbReadOnly(Vcb)) {
                    Status = STATUS_MEDIA_WRITE_PROTECTED;
                    __leave;
                }

                if (!ParentFcb) {
                    Status = STATUS_OBJECT_PATH_NOT_FOUND;
                    __leave;
                }

                Ext2SetTargetName(IrpSp->FileObject, &RealName);

                Fcb = ParentFcb;
                Mcb = Fcb->Mcb;
                Ext2ReferMcb(Mcb);

                Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
                Status = STATUS_SUCCESS;

            } else {

                Status = STATUS_OBJECT_NAME_NOT_FOUND;
                __leave;
            }

        } else { /* File / Dir already exists. */

            /* here already get Mcb referred */
            if (OpenTargetDirectory) {

                UNICODE_STRING  Last = Ext2LastComponent(&FileName);
                PEXT2_MCB       Dir;

                if (IsVcbReadOnly(Vcb)) {
                    Status = STATUS_MEDIA_WRITE_PROTECTED;
                    Ext2DerefMcb(Mcb);
                    __leave;
                }

                /* the target's directory is what is opened; its name may
                   be going at this moment (Ext2ReferParent) */
                Dir = Ext2ReferParent(Vcb, Mcb);
                Ext2DerefMcb(Mcb);
                Mcb = Dir;
                if (Mcb == NULL) {
                    Status = STATUS_OBJECT_PATH_NOT_FOUND;
                    __leave;
                }

                Ext2SetTargetName(IrpSp->FileObject, &Last);
                Irp->IoStatus.Information = FILE_EXISTS;
                Status = STATUS_SUCCESS;
                goto Openit;
            }

            Status = Ext2CheckExistingName(Mcb, CreateDisposition, DirectoryFile,
                                           NonDirectoryFile, &Irp->IoStatus.Information);
            if (!NT_SUCCESS(Status)) {
                Ext2DerefMcb(Mcb);
                __leave;
            }
        }

Openit:

        /* Shared: an open of a file that has its Fcb already - the common
           case - only reads Icb->Fcb and takes a reference (interlocked),
           and everything that changes either (a new Fcb, the reaper, the
           last close) holds the lock exclusively. Opens of the volume run
           side by side; exclusive was one open at a time. */
        if (!bFcbLockAcquired) {
            ExAcquireResourceSharedLite(&Vcb->FcbLock, TRUE);
            bFcbLockAcquired = TRUE;
        }

        /* Mcb should already be referred and symlink is too */
        if (Mcb) {

            ASSERT(Mcb->Refercount > 0);

            /* Refer the target of a symlink, so both are referenced. Plain
               opens run side by side (shared volume resource): the link's
               type and Target are read and, for a dangling link, rewritten
               under LinkLock, which guards every such change - otherwise two
               opens of one dangling link would both drop its target.
               The flag test outside the lock only decides whether to take
               it: a name becomes a symlink under the exclusive volume
               resource alone (Ext2SetReparsePoint), never while an open
               holds it shared, and the test is repeated inside. */
            if (IsMcbSymLink(Mcb)) {
                KeEnterCriticalRegion();
                ExAcquireResourceExclusiveLite(&Vcb->LinkLock, TRUE);
                if (IsMcbSymLink(Mcb)) {

                    if (OpenReparsePoint) {
                        /* set Ccb flag */
                        CcbFlags = CCB_OPEN_REPARSE_POINT;
                    } else if (Mcb->Target == NULL || IsFileDeleted(Mcb->Target)) {
                        /* dangling now: no longer a directory (or file) of the
                           target's kind, just the link itself */
                        SetLongFlag(Mcb->Flags, MCB_TYPE_SPECIAL);
                        ClearLongFlag(Mcb->Flags, MCB_TYPE_SYMLINK);
                        /* still a link on disk, shown as one (NTFS lists a dangling
                           symlink as a reparse point too); no longer a directory */
                        ClearFlag(Mcb->FileAttr, FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NORMAL);
                        SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_REPARSE_POINT);
                        if (Mcb->Target) {
                            Ext2DerefMcb(Mcb->Target);
                            Mcb->Target = NULL;
                        }
                    } else {
                        SymLink = Mcb;
                        Mcb = Mcb->Target;
                        Ext2ReferMcb(Mcb);
                        ASSERT (!IsMcbSymLink(Mcb));
                    }
                }
                ExReleaseResourceLite(&Vcb->LinkLock);
                KeLeaveCriticalRegion();
            }

            /* A dangling link is demoted to a special file above or at
               lookup, but on disk it is still a link and its text is still
               readable (readlink on Linux, the [target] NTFS lists): the
               reparse-point open is decided by the inode, not by the state
               of the name cache. */
            if (OpenReparsePoint && SymLink == NULL && IsInodeSymLink(Mcb->Inode)) {
                CcbFlags = CCB_OPEN_REPARSE_POINT;
            }

            /* From here on this function owns the reference the lookup (or
               the symlink step above) took on Mcb: the finally block drops
               it on every way out, the Ccb taking its own on success. It
               used to be claimed only after the access and delete-pending
               checks, so each open refused by them leaked one reference -
               the name, its Icb and in the end the volume could never be
               freed, and sc stop waited for ever. The Fcb may be reached
               through another hard link, which is why the name is held
               until the Ccb has taken its reference. */
            OpenMcb = Mcb;

            Status = Ext2CheckOpenAccess(Vcb, Mcb, DesiredAccess, Options, CreateDisposition);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            Fcb = Mcb->Icb->Fcb;

            /* a name already marked for deletion (DeleteFile while other
               handles keep the file alive) takes no new handles */
            if (IsFlagOn(Mcb->Flags, MCB_DELETE_PENDING) ||
                (Fcb && IsFlagOn(Fcb->Flags, FCB_DELETE_PENDING))) {
                Status = STATUS_DELETE_PENDING;
                Fcb = NULL;
                __leave;
            }
            if (Fcb == NULL && !ExIsResourceAcquiredExclusiveLite(&Vcb->FcbLock)) {

                /* none yet: making one takes the lock exclusively, and in
                   between another open may have made it, or the name been
                   marked for deletion */
                ExReleaseResourceLite(&Vcb->FcbLock);
                ExAcquireResourceExclusiveLite(&Vcb->FcbLock, TRUE);
                Fcb = Mcb->Icb->Fcb;
                if (IsFlagOn(Mcb->Flags, MCB_DELETE_PENDING) ||
                    (Fcb && IsFlagOn(Fcb->Flags, FCB_DELETE_PENDING))) {
                    Status = STATUS_DELETE_PENDING;
                    Fcb = NULL;
                    __leave;
                }
            }
            if (Fcb == NULL) {

                /* allocate Fcb for this file */
                Fcb = Ext2AllocateFcb (Vcb, Mcb);
                if (Fcb) {
                    bFcbAllocated = TRUE;
                } else {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                }
            } else {
                if (IsPagingFile) {
                    Status = STATUS_SHARING_VIOLATION;
                    Fcb = NULL;
                }
            }
        }

        if (Fcb) {
            /* grab Fcb's reference first to avoid the race between
               Ext2Close  (it could free the Fcb we are accessing) */
            Ext2ReferXcb(&Fcb->ReferenceCount);
        }

        ExReleaseResourceLite(&Vcb->FcbLock);
        bFcbLockAcquired = FALSE;

        if (Fcb) {

            ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
            bMainResourceAcquired = TRUE;

            /* Open target directory ? */
            if (NULL == Mcb) {
                Mcb = Fcb->Mcb;
            }

            /* check Mcb reference */
            ASSERT(Fcb->Mcb->Refercount > 0);

            /* file deleted ? */
            if (IsInodeDeleted(Fcb)) {
                Status = STATUS_FILE_DELETED;
                __leave;
            }

            if (DeleteOnClose && NULL == SymLink) {
                Status = Ext2IsFileRemovable(IrpContext, Vcb, Fcb, Ccb);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

            /* check access and oplock access for opened files */
            if (!bFcbAllocated  && !IsDirectory(Fcb)) {

                /* whether there's batch oplock grabbed on the file */
                if (FsRtlCurrentBatchOplock(&Fcb->Oplock)) {

                    Irp->IoStatus.Information = FILE_OPBATCH_BREAK_UNDERWAY;

                    /* break the batch lock if the sharing check fails */
                    Status = FsRtlCheckOplock( &Fcb->Oplock,
                                               IrpContext->Irp,
                                               IrpContext,
                                               Ext2OplockComplete,
                                               Ext2LockIrp );

                    if ( Status != STATUS_SUCCESS &&
                            Status != STATUS_OPLOCK_BREAK_IN_PROGRESS) {
                        *OpPostIrp = TRUE;
                        __leave;
                    }
                }
            }

            if (bCreated) {

                /* just made: EAs, label, "." and ".." (a failure takes
                   the name away again, in the finally block below) */
                Status = Ext2InitializeCreatedFile(IrpContext, Vcb, Fcb, Mcb,
                                                   ParentFcb->Mcb, DirectoryFile);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }

            } else {

                /* This file already exists. */

                if (DeleteOnClose) {

                    if (IsVcbReadOnly(Vcb)) {
                        Status = STATUS_MEDIA_WRITE_PROTECTED;
                        __leave;
                    }

                    if (IsFlagOn(Vcb->Flags, VCB_WRITE_PROTECTED)) {
                        Status = STATUS_MEDIA_WRITE_PROTECTED;

                        IoSetHardErrorOrVerifyDevice( IrpContext->Irp,
                                                      Vcb->Vpb->RealDevice );

                        SetFlag(Vcb->Vpb->RealDevice->Flags, DO_VERIFY_VOLUME);

                        Ext2RaiseStatus(IrpContext, STATUS_MEDIA_WRITE_PROTECTED);
                    }

                } else {

                    /* Just to Open file (Open/OverWrite ...) */

                    if ((!IsDirectory(Fcb)) && (IsFlagOn(IrpSp->FileObject->Flags,
                                                         FO_NO_INTERMEDIATE_BUFFERING))) {
                        Fcb->Header.IsFastIoPossible = FastIoIsPossible;

                        if (Fcb->SectionObject.DataSectionObject != NULL) {

                            if (Fcb->NonCachedOpenCount == Fcb->OpenHandleCount) {

                                if (!IsVcbReadOnly(Vcb)) {
                                    CcFlushCache(&Fcb->SectionObject, NULL, 0, NULL);
                                    ClearLongFlag(Fcb->Flags, FCB_FILE_MODIFIED);
                                }

                                CcPurgeCacheSection(&Fcb->SectionObject,
                                                    NULL,
                                                    0,
                                                    FALSE );
                            }
                        }
                    }
                }
            }

            if (!IsDirectory(Fcb)) {

                if (!IsVcbReadOnly(Vcb)) {
                    if ((CreateDisposition == FILE_SUPERSEDE) && !IsPagingFile) {
                        DesiredAccess |= DELETE;
                    } else if (((CreateDisposition == FILE_OVERWRITE) ||
                                (CreateDisposition == FILE_OVERWRITE_IF)) && !IsPagingFile) {
                        DesiredAccess |= (FILE_WRITE_DATA | FILE_WRITE_EA |
                                          FILE_WRITE_ATTRIBUTES );
                    }
                }

                if (!bFcbAllocated) {

                    /* check the oplock state of the file */

                    Status = FsRtlCheckOplock(  &Fcb->Oplock,
                                                IrpContext->Irp,
                                                IrpContext,
                                                Ext2OplockComplete,
                                                Ext2LockIrp );

                    if ( Status != STATUS_SUCCESS &&
                            Status != STATUS_OPLOCK_BREAK_IN_PROGRESS) {
                        *OpPostIrp = TRUE;
                        __leave;
                    }
                }
            }

            if (Fcb->OpenHandleCount > 0) {

                /* check the share access conflicts */
                Status = IoCheckShareAccess( DesiredAccess,
                                             ShareAccess,
                                             IrpSp->FileObject,
                                             &(Fcb->ShareAccess),
                                             TRUE );
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }

            } else {

                /* set share access rights */
                IoSetShareAccess( DesiredAccess,
                                  ShareAccess,
                                  IrpSp->FileObject,
                                  &(Fcb->ShareAccess) );
            }

            Ccb = Ext2AllocateCcb(CcbFlags, Mcb, SymLink);
            if (!Ccb) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }

            if (DeleteOnClose)
                SetLongFlag(Ccb->Flags, CCB_DELETE_ON_CLOSE);

            if (SymLink)
                Ccb->filp.f_dentry = SymLink->de;
            else
                Ccb->filp.f_dentry = Fcb->Mcb->de;

            Ccb->filp.f_version = Fcb->Mcb->Inode->i_version;
            Ext2ReferXcb(&Fcb->OpenHandleCount);
            Ext2ReferXcb(&Fcb->ReferenceCount);
            Ext2ReferXcb(&Fcb->CcbCount);
            Ccb->FileObject = IrpSp->FileObject;
            InsertTailList(&Fcb->CcbList, &Ccb->FcbLink);

            if (!IsDirectory(Fcb)) {
                if (NoIntermediateBuffering) {
                    Fcb->NonCachedOpenCount++;
                    SetFlag(IrpSp->FileObject->Flags, FO_CACHE_SUPPORTED);
                } else {
                    SetFlag(IrpSp->FileObject->Flags, FO_CACHE_SUPPORTED);
                }
            }

            Ext2ReferXcb(&Vcb->OpenHandleCount);
            Ext2ReferXcb(&Vcb->ReferenceCount);

            IrpSp->FileObject->FsContext = (void*) Fcb;
            IrpSp->FileObject->FsContext2 = (void*) Ccb;
            IrpSp->FileObject->PrivateCacheMap = NULL;
            IrpSp->FileObject->SectionObjectPointer = &(Fcb->SectionObject);

            DEBUG(DL_INF, ( "Ext2CreateFile: %wZ OpenCount=%u ReferCount=%u NonCachedCount=%u\n",
                            &Fcb->Mcb->FullName, Fcb->OpenHandleCount, Fcb->ReferenceCount, Fcb->NonCachedOpenCount));

            Status = STATUS_SUCCESS;

            if (bCreated) {

                if (IsDirectory(Fcb)) {
                    Ext2NotifyReportChange(
                        IrpContext,
                        Vcb,
                        Fcb->Mcb,
                        FILE_NOTIFY_CHANGE_DIR_NAME,
                        FILE_ACTION_ADDED );
                } else {
                    Ext2NotifyReportChange(
                        IrpContext,
                        Vcb,
                        Fcb->Mcb,
                        FILE_NOTIFY_CHANGE_FILE_NAME,
                        FILE_ACTION_ADDED );
                }

            } else if (!IsDirectory(Fcb)) {

                if ( DeleteOnClose ||
                        IsFlagOn(DesiredAccess, FILE_WRITE_DATA) ||
                        (CreateDisposition == FILE_OVERWRITE) ||
                        (CreateDisposition == FILE_OVERWRITE_IF)) {
                    if (!MmFlushImageSection( &Fcb->SectionObject,
                                              MmFlushForWrite )) {

                        Status = DeleteOnClose ? STATUS_CANNOT_DELETE :
                                 STATUS_SHARING_VIOLATION;
                        __leave;
                    }
                }

                if ((CreateDisposition == FILE_SUPERSEDE) ||
                        (CreateDisposition == FILE_OVERWRITE) ||
                        (CreateDisposition == FILE_OVERWRITE_IF)) {

                    if (IsDirectory(Fcb)) {
                        Status = STATUS_FILE_IS_A_DIRECTORY;
                        __leave;
                    }

                    if (SymLink != NULL) {
                        Status = STATUS_INVALID_PARAMETER;
                        __leave;
                    }

                    if (IsVcbReadOnly(Vcb)) {
                        Status = STATUS_MEDIA_WRITE_PROTECTED;
                        __leave;
                    }

                    if (IsFlagOn(Vcb->Flags, VCB_WRITE_PROTECTED)) {

                        IoSetHardErrorOrVerifyDevice( IrpContext->Irp,
                                                      Vcb->Vpb->RealDevice );
                        SetFlag(Vcb->Vpb->RealDevice->Flags, DO_VERIFY_VOLUME);
                        Ext2RaiseStatus(IrpContext, STATUS_MEDIA_WRITE_PROTECTED);
                    }

                    Status = Ext2SupersedeOrOverWriteFile(
                                 IrpContext,
                                 IrpSp->FileObject,
                                 Vcb,
                                 Fcb,
                                 &Irp->Overlay.AllocationSize,
                                 CreateDisposition );

                    if (!NT_SUCCESS(Status)) {
                        __leave;
                    }

                    Ext2NotifyReportChange(
                        IrpContext,
                        Vcb,
                        Fcb->Mcb,
                        FILE_NOTIFY_CHANGE_LAST_WRITE |
                        FILE_NOTIFY_CHANGE_ATTRIBUTES |
                        FILE_NOTIFY_CHANGE_SIZE,
                        FILE_ACTION_MODIFIED );


                    if (CreateDisposition == FILE_SUPERSEDE) {
                        Irp->IoStatus.Information = FILE_SUPERSEDED;
                    } else {
                        Irp->IoStatus.Information = FILE_OVERWRITTEN;
                    }
                }
            }

        } else {
            __leave;
        }

    } __finally {


        if (bFcbLockAcquired) {
            ExReleaseResourceLite(&Vcb->FcbLock);
        }

        if (ParentMcb) {
            Ext2DerefMcb(ParentMcb);
        }

        /* cleanup Fcb and Ccb, Mcb if necessary */
        if (!NT_SUCCESS(Status)) {

            if (Ccb != NULL) {


                ASSERT(Fcb != NULL);
                ASSERT(Fcb->Mcb != NULL);

                DEBUG(DL_ERR, ("Ext2CreateFile: failed to create %wZ status = %xh\n",
                               &Fcb->Mcb->FullName, Status));

                Ext2DerefXcb(&Fcb->OpenHandleCount);
                Ext2DerefXcb(&Fcb->ReferenceCount);
                Ext2DerefXcb(&Fcb->CcbCount);
                RemoveEntryList(&Ccb->FcbLink);

                if (!IsDirectory(Fcb)) {
                    if (NoIntermediateBuffering) {
                        Fcb->NonCachedOpenCount--;
                    } else {
                        ClearFlag(IrpSp->FileObject->Flags, FO_CACHE_SUPPORTED);
                    }
                }

                Ext2DerefXcb(&Vcb->OpenHandleCount);
                Ext2DerefXcb(&Vcb->ReferenceCount);

                IoRemoveShareAccess(IrpSp->FileObject, &Fcb->ShareAccess);

                IrpSp->FileObject->FsContext = NULL;
                IrpSp->FileObject->FsContext2 = NULL;
                IrpSp->FileObject->PrivateCacheMap = NULL;
                IrpSp->FileObject->SectionObjectPointer = NULL;

                Ext2FreeCcb(Vcb, Ccb);
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

                if (bCreated) {
                    /* a file the failed create leaves behind is whole, only
                       unwanted: said, not hidden */
                    NTSTATUS Undo = Ext2DeleteFile(IrpContext, Vcb, Fcb, Mcb);

                    if (!NT_SUCCESS(Undo)) {
                        DbgPrint("ext4: a failed create left %wZ behind (%08x)\n",
                                 &Mcb->FullName, Undo);
                    }
                }
            }
        }

        if (bMainResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        /* free file name buffer */
        if (FileName.Buffer) {
            DEC_MEM_COUNT(PS_FILE_NAME, FileName.Buffer, FileName.MaximumLength);
            Ext2FreePool(FileName.Buffer, EXT2_FNAME_MAGIC);
        }

        /* dereference Fcb and parent */
        if (Fcb) {
            Ext2ReleaseFcb(Fcb);
        }
        if (ParentFcb) {
            Ext2ReleaseFcb(ParentFcb);
        }

        /* drop SymLink's refer: If succeeds, Ext2AllocateCcb should refer
           it already. It fails, we need release the refer to let it freed */
        if (SymLink) {
            Ext2DerefMcb(SymLink);
        }

        /* same for the name we opened */
        if (OpenMcb) {
            Ext2DerefMcb(OpenMcb);
        }
    }

    return Status;
}
