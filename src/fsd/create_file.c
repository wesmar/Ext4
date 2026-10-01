/**
 * create_file.c - IRP_MJ_CREATE for files and directories: open, create, disposition handling.
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

    BOOLEAN             bParentFcbCreated = FALSE;
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
                DbgBreak();
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
            DEBUG(DL_ERR, ( "Ex2CreateFile: failed to allocate FileName.\n"));
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

            //
            //  Bad Name if there are still beginning backslashes.
            //

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

                /* RetMcb is already refered */
                ParentMcb = RetMcb;
                PathName  = RemainName;

                /* symlink must use it's target */
                if (IsMcbSymLink(ParentMcb)) {
                    Ext2ReferMcb(ParentMcb->Target);
                    Ext2DerefMcb(ParentMcb);
                    ParentMcb = ParentMcb->Target;
                    ASSERT(!IsMcbSymLink(ParentMcb));
                }

                goto Dissecting;
            }

            /* is name valid */
            if ( FsRtlDoesNameContainWildCards(&RealName) ||
                    !Ext2IsNameValid(&RealName)) {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }

            if (!bFcbLockAcquired) {
                ExAcquireResourceExclusiveLite(&Vcb->FcbLock, TRUE);
                bFcbLockAcquired = TRUE;
            }

            /* get the ParentFcb, allocate it if needed ... */
            ParentFcb = ParentMcb->Icb->Fcb;
            if (!ParentFcb) {
                ParentFcb = Ext2AllocateFcb(Vcb, ParentMcb);
                if (!ParentFcb) {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    __leave;
                }
                bParentFcbCreated = TRUE;
            }
            Ext2ReferXcb(&ParentFcb->ReferenceCount);

            if (bFcbLockAcquired) {
                ExReleaseResourceLite(&Vcb->FcbLock);
                bFcbLockAcquired = FALSE;
            }

            // We need to create a new one ?
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
                        DbgBreak();
                        Status = STATUS_INVALID_PARAMETER;
                        __leave;
                    }
                }

                if (!ParentFcb) {
                    Status = STATUS_OBJECT_PATH_NOT_FOUND;
                    __leave;
                }

                /* allocate inode and construct entry for this file */
                Status = Ext2CreateInode(
                             IrpContext,
                             Vcb,
                             ParentFcb,
                             DirectoryFile ? EXT2_FT_DIR : EXT2_FT_REG_FILE,
                             IrpSp->Parameters.Create.FileAttributes,
                             &RealName
                         );

                if (!NT_SUCCESS(Status)) {
                    DbgBreak();
                    __leave;
                }

                bCreated = TRUE;
                DEBUG(DL_RES, ("Ext2CreateFile: Confirm creation: %wZ\\%wZ\n",
                               &ParentMcb->FullName, &RealName));

                Irp->IoStatus.Information = FILE_CREATED;
                Status = Ext2LookupFile (
                             IrpContext,
                             Vcb,
                             &RealName,
                             ParentMcb,
                             &Mcb,
                             0);
                if (!NT_SUCCESS(Status)) {
                    DbgBreak();
                }

            } else if (OpenTargetDirectory) {

                if (IsVcbReadOnly(Vcb)) {
                    Status = STATUS_MEDIA_WRITE_PROTECTED;
                    __leave;
                }

                if (!ParentFcb) {
                    Status = STATUS_OBJECT_PATH_NOT_FOUND;
                    __leave;
                }

                RtlZeroMemory( IrpSp->FileObject->FileName.Buffer,
                               IrpSp->FileObject->FileName.MaximumLength);
                IrpSp->FileObject->FileName.Length = RealName.Length;

                RtlCopyMemory( IrpSp->FileObject->FileName.Buffer,
                               RealName.Buffer,
                               RealName.Length );

                Fcb = ParentFcb;
                Mcb = Fcb->Mcb;
                Ext2ReferMcb(Mcb);

                Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
                Status = STATUS_SUCCESS;

            } else {

                Status = STATUS_OBJECT_NAME_NOT_FOUND;
                __leave;
            }

        } else { // File / Dir already exists.

            /* here already get Mcb referred */
            if (OpenTargetDirectory) {

                UNICODE_STRING  RealName = FileName;
                USHORT          i = 0;

                while (RealName.Buffer[RealName.Length/2 - 1] == L'\\') {
                    RealName.Length -= sizeof(WCHAR);
                    RealName.Buffer[RealName.Length/2] = 0;
                }
                i = RealName.Length/2;
                while (i > 0 && RealName.Buffer[i - 1] != L'\\')
                    i--;

                if (IsVcbReadOnly(Vcb)) {
                    Status = STATUS_MEDIA_WRITE_PROTECTED;
                    Ext2DerefMcb(Mcb);
                    __leave;
                }

                Irp->IoStatus.Information = FILE_EXISTS;
                Status = STATUS_SUCCESS;

                RtlZeroMemory( IrpSp->FileObject->FileName.Buffer,
                               IrpSp->FileObject->FileName.MaximumLength);
                IrpSp->FileObject->FileName.Length = RealName.Length - i * sizeof(WCHAR);
                RtlCopyMemory( IrpSp->FileObject->FileName.Buffer, &RealName.Buffer[i],
                               IrpSp->FileObject->FileName.Length );

                // use's it's parent since it's open-target operation
                Ext2ReferMcb(Mcb->Parent);
                Ext2DerefMcb(Mcb);
                Mcb = Mcb->Parent;

                goto Openit;
            }

            // We can not create if one exists
            if (CreateDisposition == FILE_CREATE) {
                Irp->IoStatus.Information = FILE_EXISTS;
                Status = STATUS_OBJECT_NAME_COLLISION;
                Ext2DerefMcb(Mcb);
                __leave;
            }

            /* directory forbits us to do the followings ... */
            if (IsMcbDirectory(Mcb)) {

                if ((CreateDisposition != FILE_OPEN) &&
                    (CreateDisposition != FILE_OPEN_IF)) {

                    Status = STATUS_OBJECT_NAME_COLLISION;
                    Ext2DerefMcb(Mcb);
                    __leave;
                }

                if (NonDirectoryFile) {
                    Status = STATUS_FILE_IS_A_DIRECTORY;
                    Ext2DerefMcb(Mcb);
                    __leave;
                }

                if (Mcb->Inode->i_ino == EXT2_ROOT_INO) {

                    if (OpenTargetDirectory) {
                        DbgBreak();
                        Status = STATUS_INVALID_PARAMETER;
                        Ext2DerefMcb(Mcb);
                        __leave;
                    }
                }

            } else {

                if (DirectoryFile) {
                    Status = STATUS_NOT_A_DIRECTORY;;
                    Ext2DerefMcb(Mcb);
                    __leave;
                }
            }

            Irp->IoStatus.Information = FILE_OPENED;
        }

Openit:

        if (!bFcbLockAcquired) {
            ExAcquireResourceExclusiveLite(&Vcb->FcbLock, TRUE);
            bFcbLockAcquired = TRUE;
        }

        /* Mcb should already be referred and symlink is too */
        if (Mcb) {

            ASSERT(Mcb->Refercount > 0);

            /* Refer the target of a symlink, so both are referenced. Plain
               opens run side by side (shared volume resource): the link's
               type and Target are read and, for a dangling link, rewritten
               under McbLock, which guards every such change - otherwise two
               opens of one dangling link would both drop its target.
               The flag test outside the lock only decides whether to take
               it: a name becomes a symlink under the exclusive volume
               resource alone (Ext2SetReparsePoint), never while an open
               holds it shared, and the test is repeated inside. */
            if (IsMcbSymLink(Mcb)) {
                ExAcquireResourceExclusiveLite(&Vcb->McbLock, TRUE);
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
                ExReleaseResourceLite(&Vcb->McbLock);
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

            // Check readonly flag
            if (BooleanFlagOn(DesiredAccess,  FILE_GENERIC_READ) &&
                !Ext2CheckFileAccess(Vcb, Mcb, Ext2FileCanRead)) {
                Status = STATUS_ACCESS_DENIED;
                __leave;
            }
            if (!Ext2CheckFileAccess(Vcb, Mcb, Ext2FileCanWrite)) {
                if (BooleanFlagOn(DesiredAccess,  FILE_WRITE_DATA | FILE_APPEND_DATA |
                                  FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD)) {
                    Status = STATUS_ACCESS_DENIED;
                    __leave;
                } else if (IsFlagOn(Options, FILE_DELETE_ON_CLOSE )) {
                    Status = STATUS_CANNOT_DELETE;
                    __leave;
                }
            }

            /* the read-only attribute (owner write bit clear) holds for
               everybody, root included, as on NTFS: no write handle, no
               delete-on-close. A read-only directory still takes new
               entries - Windows treats that attribute as decoration. */
            if (!IsMcbDirectory(Mcb) && !Ext2IsOwnerWritable(Mcb->Inode->i_mode)) {
                if (BooleanFlagOn(DesiredAccess, FILE_WRITE_DATA | FILE_APPEND_DATA)) {
                    Status = STATUS_ACCESS_DENIED;
                    __leave;
                } else if (IsFlagOn(Options, FILE_DELETE_ON_CLOSE)) {
                    Status = STATUS_CANNOT_DELETE;
                    __leave;
                }
            }

            /* chattr +i: no data, attribute or EA is written and the file
               does not go (for a directory: nothing is added or removed);
               chattr +a: a file grows at its end only - FILE_APPEND_DATA
               without FILE_WRITE_DATA - and does not go, a directory only
               takes new entries. Linux refuses these to root as well. */
            if (Ext4IsSealed(Mcb->Inode)) {
                ACCESS_MASK Refused = DELETE | FILE_DELETE_CHILD;
                BOOLEAN     Rewrite = CreateDisposition == FILE_SUPERSEDE ||
                                      CreateDisposition == FILE_OVERWRITE ||
                                      CreateDisposition == FILE_OVERWRITE_IF;

                if (Ext4IsImmutable(Mcb->Inode)) {
                    Refused |= FILE_WRITE_DATA | FILE_APPEND_DATA |
                               FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA;
                } else if (!IsMcbDirectory(Mcb)) {
                    Refused |= FILE_WRITE_DATA;
                }
                if (BooleanFlagOn(DesiredAccess, Refused) || Rewrite) {
                    Status = STATUS_ACCESS_DENIED;
                    __leave;
                } else if (IsFlagOn(Options, FILE_DELETE_ON_CLOSE)) {
                    Status = STATUS_CANNOT_DELETE;
                    __leave;
                }
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
                DbgBreak();
                Mcb = Fcb->Mcb;
            }

            /* check Mcb reference */
            ASSERT(Fcb->Mcb->Refercount > 0);

            /* file delted ? */
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

                /* whether there's batch oplock grabed on the file */
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

                //
                //  This file is just created.
                //

				Status = Ext2OverwriteEa(IrpContext, Vcb, Fcb, &Irp->IoStatus);
				if (!NT_SUCCESS(Status)) {
					Ext2DeleteFile(IrpContext, Vcb, Fcb, Mcb);
					__leave;
				}

                /* the SELinux label of the directory, as Linux would give */
                Ext4InheritSecurityLabel(IrpContext, Vcb,
                                         ParentFcb ? ParentFcb->Mcb : NULL, Mcb);

                if (DirectoryFile) {

                    Status = Ext2AddDotEntries(IrpContext, ParentMcb->Inode, Mcb->Inode);
                    if (!NT_SUCCESS(Status)) {
                        Ext2DeleteFile(IrpContext, Vcb, Fcb, Mcb);
                        __leave;
                    }

                } else {

                    if ((LONGLONG)ext3_free_blocks_count(SUPER_BLOCK) <=
                            Ext2TotalBlocks(Vcb, &Irp->Overlay.AllocationSize, NULL)) {
                        DbgBreak();
                        Status = STATUS_DISK_FULL;
                        __leave;
                    }

                    /* disable data blocks allocation */
                }

            } else {

                //
                //  This file alreayd exists.
                //

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

                    //
                    // Just to Open file (Open/OverWrite ...)
                    //

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

                    //
                    //  check the oplock state of the file
                    //

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

                /* check the shrae access conflicts */
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
                DbgBreak();
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
                        DbgBreak();
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
                        DbgBreak();
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
            DbgBreak();
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

                DbgBreak();

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
                        Size.QuadPart = 0;
                        Ext2TruncateFile(IrpContext, Vcb, Fcb->Mcb, &Size);
                    } __finally {
                        ExReleaseResourceLite(&Fcb->PagingIoResource);
                    }
                }

                if (bCreated) {
                    Ext2DeleteFile(IrpContext, Vcb, Fcb, Mcb);
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
