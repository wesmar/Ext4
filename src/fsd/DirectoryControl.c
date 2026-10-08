/**
 * DirectoryControl.c - IRP_MJ_DIRECTORY_CONTROL: entry formatting and change notification.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "dirctl_internal.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2GetInfoLength)
#pragma alloc_text(PAGE, Ext2ProcessEntry)
#pragma alloc_text(PAGE, Ext2NotifyChangeDirectory)
#pragma alloc_text(PAGE, Ext2DirectoryControl)
#pragma alloc_text(PAGE, Ext2IsDirectoryEmpty)
#endif

ULONG
Ext2GetInfoLength(IN FILE_INFORMATION_CLASS  FileInformationClass)
{
    switch (FileInformationClass) {

    case FileDirectoryInformation:
        return FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName[0]);

    case FileFullDirectoryInformation:
        return FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName[0]);

    case FileBothDirectoryInformation:
        return FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName[0]);

    case FileNamesInformation:
        return FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName[0]);

    case FileIdFullDirectoryInformation:
        return FIELD_OFFSET(FILE_ID_FULL_DIR_INFORMATION, FileName[0]);

    case FileIdBothDirectoryInformation:
        return FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName[0]);

    default:
        break;
    }

    return 0;
}

/* What the directory classes share: the same fields up to the name's length
   (FILE_DIRECTORY_INFORMATION's), the EA size right after it, and the name
   at the end (Ext2GetInfoLength). FILE_NAMES_INFORMATION shares the first
   two fields only. */
C_ASSERT(FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileNameLength) == FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileNameLength) &&
         FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileNameLength) == FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileNameLength) &&
         FIELD_OFFSET(FILE_ID_FULL_DIR_INFORMATION, FileNameLength) == FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileNameLength) &&
         FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileNameLength) == FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileNameLength));
C_ASSERT(FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, EaSize) == FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, EaSize) &&
         FIELD_OFFSET(FILE_ID_FULL_DIR_INFORMATION, EaSize) == FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, EaSize) &&
         FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, EaSize) == FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, EaSize));
C_ASSERT(FIELD_OFFSET(FILE_NAMES_INFORMATION, FileIndex) == FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileIndex));

/* what an entry shows of its file */
typedef struct _EXT2_ENTRY_INFO {
    LARGE_INTEGER   CreationTime;
    LARGE_INTEGER   LastAccessTime;
    LARGE_INTEGER   LastWriteTime;
    LARGE_INTEGER   ChangeTime;
    LONGLONG        FileSize;
    ULONG           Attributes;
    BOOLEAN         Symlink;
} EXT2_ENTRY_INFO, *PEXT2_ENTRY_INFO;

/* a cached name: its node's times and attributes; a symlink shows its
   target's size and kind */
static VOID
Ext2EntryFromName(IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb, OUT PEXT2_ENTRY_INFO E)
{
    PEXT2_MCB Target;

    E->CreationTime = Mcb->Icb->CreationTime;
    E->LastAccessTime = Mcb->Icb->LastAccessTime;
    E->LastWriteTime = Mcb->Icb->LastWriteTime;
    E->ChangeTime = Mcb->Icb->ChangeTime;
    E->Attributes = Mcb->FileAttr;
    E->Symlink = IsInodeSymLink(Mcb->Inode);

    if (!IsMcbSymLink(Mcb)) {
        E->FileSize = IsMcbDirectory(Mcb) ? 0 : Mcb->Inode->i_size;
        return;
    }

    /* the target, held while it is looked at (a demotion of the link may
       drop it meanwhile) */
    Target = Ext2ReferLinkTarget(Vcb, Mcb);
    if (Target == NULL || IsFileDeleted(Target)) {
        ClearFlag(E->Attributes, FILE_ATTRIBUTE_DIRECTORY);
    } else if (IsMcbDirectory(Target)) {
        SetFlag(E->Attributes, FILE_ATTRIBUTE_DIRECTORY);
    } else {
        E->FileSize = Target->Inode->i_size;
    }
    if (Target != NULL) {
        Ext2DerefMcb(Target);
    }
}

/* an inode read for the listing alone */
static VOID
Ext2EntryFromInode(IN PEXT2_VCB Vcb, IN struct inode *Inode, OUT PEXT2_ENTRY_INFO E)
{
    E->LastAccessTime = Ext2GetInodeTime(Inode->i_atime, Inode->i_atime_extra);
    E->LastWriteTime = Ext2GetInodeTime(Inode->i_mtime, Inode->i_mtime_extra);
    E->ChangeTime = Ext2GetInodeTime(Inode->i_ctime, Inode->i_ctime_extra);
    E->CreationTime = Inode->i_crtime ? Ext2GetInodeTime(Inode->i_crtime, Inode->i_crtime_extra) :
                                        E->ChangeTime;

    if (S_ISDIR(Inode->i_mode)) {
        E->Attributes = FILE_ATTRIBUTE_DIRECTORY;
    } else if (S_ISLNK(Inode->i_mode)) {
        E->Attributes = FILE_ATTRIBUTE_REPARSE_POINT;
        E->Symlink = TRUE;
        E->FileSize = Inode->i_size;
    } else {
        E->Attributes = FILE_ATTRIBUTE_NORMAL;
        /* a device node, a fifo, a socket: no size to show */
        E->FileSize = S_ISREG(Inode->i_mode) ? Inode->i_size : 0;
    }

    if (!Ext2IsOwnerWritable(Inode->i_mode) || Ext4IsImmutable(Inode) ||
        !Ext2CheckInodeAccess(Vcb, Inode, Ext2FileCanWrite)) {
        SetFlag(E->Attributes, FILE_ATTRIBUTE_READONLY);
    }
}

/* the entry named Name, inode Ino, of directory Dcb */
static VOID
Ext2EntryInfo(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PEXT2_FCB Dcb,
              IN ULONG Ino, IN PUNICODE_STRING Name, OUT PEXT2_ENTRY_INFO E)
{
    struct inode    Inode = { 0 };
    PEXT2_MCB       Mcb;

    RtlZeroMemory(E, sizeof(EXT2_ENTRY_INFO));

    Mcb = Ext2SearchMcb(Vcb, Dcb->Mcb, Name);
    if (Mcb != NULL) {
        /* a link not followed yet, or demoted: its target may exist now
           (under LinkLock, like every change of a link) */
        if (S_ISLNK(Mcb->Inode->i_mode)) {
            Ext2ResolveLink(IrpContext, Vcb, Mcb, 0, FALSE);
        }
        Ext2EntryFromName(Vcb, Mcb, E);
        Ext2DerefMcb(Mcb);
        return;
    }

    /* an open hard link of this inode may be ahead of the disk */
    Inode.i_ino = Ino;
    Inode.i_sb = &Vcb->sb;
    if (!Ext2PeekCachedInode(Vcb, Ino, &Inode) && !Ext2LoadInode(Vcb, &Inode)) {
        /* An inode that cannot be read (corrupt, and reported) is listed
           by its name all the same, as Linux lists it - an empty read-only
           file; opening it fails. It used to be left out: the damage was
           hidden. */
        DEBUG(DL_ERR, ("Ext2EntryInfo: inode %xh (%wZ) cannot be read\n", Ino, Name));
        RtlZeroMemory(&Inode, sizeof(Inode));
        Inode.i_ino = Ino;
        Inode.i_sb = &Vcb->sb;
        Inode.i_mode = S_IFREG;
    }

    /* a symlink is shown as its target is: the lookup resolves it */
    if (S_ISLNK(Inode.i_mode)) {
        Mcb = NULL;
        Ext2LookupFile(IrpContext, Vcb, Name, Dcb->Mcb, &Mcb, 0);
        if (Mcb != NULL) {
            if (!IsMcbSpecialFile(Mcb)) {
                Ext2EntryFromName(Vcb, Mcb, E);
                Ext2DerefMcb(Mcb);
                return;
            }
            Ext2DerefMcb(Mcb);
        }
    }

    Ext2EntryFromInode(Vcb, &Inode, E);
}

/*
 * One entry of a directory listing into Buffer at UsedLength (8-aligned),
 * Length bytes left there. A first entry that does not fit whole is
 * returned cut, as STATUS_BUFFER_OVERFLOW; a later one is refused so.
 */
NTSTATUS
Ext2ProcessEntry(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_FCB            Dcb,
    IN FILE_INFORMATION_CLASS  FileInformationClass,
    IN ULONG                in,
    IN PVOID                Buffer,
    IN ULONG                UsedLength,
    IN ULONG                Length,
    IN ULONG                FileIndex,
    IN PUNICODE_STRING      pName,
    OUT PULONG              EntrySize,
    IN BOOLEAN              Single
)
{
    PUCHAR          Entry = (PUCHAR)Buffer + UsedLength;
    ULONG           InfoLength = Ext2GetInfoLength(FileInformationClass);
    ULONG           NameLength = pName->Length;
    ULONG           Copied;
    EXT2_ENTRY_INFO E;
    NTSTATUS        Status = STATUS_SUCCESS;

    *EntrySize = 0;
    ASSERT((UsedLength & 7) == 0);

    if (InfoLength == 0) {
        DEBUG(DL_ERR, ("Ext2ProcessEntry: invalid class %xh for %wZ in %wZ\n",
                       FileInformationClass, pName, &Dcb->Mcb->FullName));
        return STATUS_INVALID_INFO_CLASS;
    }
    if (InfoLength + NameLength > Length) {
        if (UsedLength != 0 || InfoLength > Length) {
            return STATUS_BUFFER_OVERFLOW;
        }
        Status = STATUS_BUFFER_OVERFLOW;
    }

    Ext2EntryInfo(IrpContext, Vcb, Dcb, in, pName, &E);

    if (E.Attributes == 0) {
        E.Attributes = FILE_ATTRIBUTE_NORMAL;
    }
    /* Recycled, RECYCLER, pagefile.sys in the root: hidden and system */
    if (IsRoot(Dcb) && Ext2IsSpecialSystemFile(pName, IsFlagOn(E.Attributes, FILE_ATTRIBUTE_DIRECTORY))) {
        SetFlag(E.Attributes, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    }
    /* a name starting with '.', but not "." or "..": hidden, the Unix
       convention */
    if (pName->Length >= 2 * sizeof(WCHAR) && pName->Buffer[0] == L'.' &&
        (pName->Length > 2 * sizeof(WCHAR) || pName->Buffer[1] != L'.')) {
        SetFlag(E.Attributes, FILE_ATTRIBUTE_HIDDEN);
    }

    /* the fields every class has */
    if (!Single) {
        ((PFILE_DIRECTORY_INFORMATION)Entry)->NextEntryOffset =
            CEILING_ALIGNED(ULONG, InfoLength + NameLength, 8);
    }
    ((PFILE_DIRECTORY_INFORMATION)Entry)->FileIndex = FileIndex;

    if (FileInformationClass == FileNamesInformation) {

        ((PFILE_NAMES_INFORMATION)Entry)->FileNameLength = NameLength;

    } else {

        PFILE_DIRECTORY_INFORMATION FDI = (PFILE_DIRECTORY_INFORMATION)Entry;

        FDI->CreationTime = E.CreationTime;
        FDI->LastAccessTime = E.LastAccessTime;
        FDI->LastWriteTime = E.LastWriteTime;
        FDI->ChangeTime = E.ChangeTime;
        FDI->EndOfFile.QuadPart = E.FileSize;
        FDI->AllocationSize.QuadPart = CEILING_ALIGNED(ULONGLONG, E.FileSize, BLOCK_SIZE);
        FDI->FileAttributes = E.Attributes;
        FDI->FileNameLength = NameLength;

        /* a reparse point's tag stands where its EA size would (NTFS) */
        if (FileInformationClass != FileDirectoryInformation && E.Symlink) {
            ((PFILE_FULL_DIR_INFORMATION)Entry)->EaSize = IO_REPARSE_TAG_SYMLINK;
        }
        if (FileInformationClass == FileIdFullDirectoryInformation) {
            ((PFILE_ID_FULL_DIR_INFORMATION)Entry)->FileId.QuadPart = (LONGLONG)in;
        } else if (FileInformationClass == FileIdBothDirectoryInformation) {
            ((PFILE_ID_BOTH_DIR_INFORMATION)Entry)->FileId.QuadPart = (LONGLONG)in;
        }
    }

    /* the name, cut to what fits */
    Copied = min(NameLength, Length - InfoLength);
    RtlCopyMemory(Entry + InfoLength, pName->Buffer, Copied);
    *EntrySize = InfoLength + Copied;

    DEBUG(DL_CP, ("Ext2ProcessEntry: Status = %xh for %wZ in %wZ\n",
                  Status, pName, &Dcb->Mcb->FullName));
    return Status;
}

BOOLEAN
Ext2IsWearingCloak(
    IN  PEXT2_VCB       Vcb,
    IN  POEM_STRING     OemName
)
{
    size_t  PatLen = 0;

    /* we could not filter the files: "." and ".." */
    if (OemName->Length >= 1 && OemName->Buffer[0] == '.') {

        if ( OemName->Length == 2 && OemName->Buffer[1] == '.') {
            return FALSE;
        } else if (OemName->Length == 1) {
            return FALSE;
        }
    }

    /* checking name prefix */
    if (Vcb->bHidingPrefix) {
        PatLen = strlen(&Vcb->sHidingPrefix[0]);
        if (PatLen > 0 && PatLen <= OemName->Length) {
            if ( _strnicmp( OemName->Buffer,
                            Vcb->sHidingPrefix,
                            PatLen ) == 0) {
                return TRUE;
            }
        }
    }

    /* checking name suffix */
    if (Vcb->bHidingSuffix) {
        PatLen = strlen(&Vcb->sHidingSuffix[0]);
        if (PatLen > 0 && PatLen <= OemName->Length) {
            if ( _strnicmp(&OemName->Buffer[OemName->Length - PatLen],
                           Vcb->sHidingSuffix, PatLen ) == 0) {
                return TRUE;
            }
        }
    }

    return FALSE;
}

/* the entry's name in the query's buffer, which grows when a name needs more */
static NTSTATUS
Ext2FillName(IN PEXT2_VCB Vcb, IN OUT PEXT2_FILLDIR_CONTEXT fc, IN POEM_STRING Oem)
{
    ULONG Bytes = Ext2OEMToUnicodeSize(Vcb, Oem) + sizeof(WCHAR);

    if (Bytes > fc->efc_name.MaximumLength) {
        PWCHAR Buffer = Ext2AllocatePool(PagedPool, Bytes, EXT2_INAME_MAGIC);
        if (Buffer == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        if (fc->efc_name.Buffer != NULL) {
            DEC_MEM_COUNT(PS_INODE_NAME, fc->efc_name.Buffer, fc->efc_name.MaximumLength);
            Ext2FreePool(fc->efc_name.Buffer, EXT2_INAME_MAGIC);
        }
        INC_MEM_COUNT(PS_INODE_NAME, Buffer, Bytes);
        fc->efc_name.Buffer = Buffer;
        fc->efc_name.MaximumLength = (USHORT)Bytes;
    }
    fc->efc_name.Length = 0;
    RtlZeroMemory(fc->efc_name.Buffer, fc->efc_name.MaximumLength);
    return Ext2OEMToUnicode(Vcb, &fc->efc_name, Oem);
}

/* the buffer of names a query used (Ext2FillName) */
VOID
Ext2FreeFillName(IN OUT PEXT2_FILLDIR_CONTEXT fc)
{
    if (fc->efc_name.Buffer != NULL) {
        DEC_MEM_COUNT(PS_INODE_NAME, fc->efc_name.Buffer, fc->efc_name.MaximumLength);
        Ext2FreePool(fc->efc_name.Buffer, EXT2_INAME_MAGIC);
        RtlZeroMemory(&fc->efc_name, sizeof(fc->efc_name));
    }
}

/*
 * One directory entry into a query's buffer, for both the indexed walk
 * (ext3_dx_readdir) and the walk in disk order (Ext2QueryDirectory). 0: go
 * on with the next entry. Not 0: stop before this entry - the buffer is
 * full, one entry was asked for, or the fill failed (efc_status says why);
 * the entry is the next query's.
 */
int Ext2FillEntry(void *context, const char *name, int namlen,
                  ULONG offset, __u32 ino, unsigned int d_type)
{
    PEXT2_FILLDIR_CONTEXT   fc = context;
    PEXT2_IRP_CONTEXT       IrpContext = fc->efc_irp;
    PEXT2_FCB               Fcb = IrpContext->Fcb;
    PEXT2_CCB               Ccb = IrpContext->Ccb;
    PEXT2_VCB               Vcb = Fcb->Vcb;
    PUNICODE_STRING         Pattern = &Ccb->DirectorySearchPattern;
    OEM_STRING              Oem;
    ULONG                   Start, EntrySize;
    NTSTATUS                Status;

    UNREFERENCED_PARAMETER(d_type);

    if (fc->efc_start > 0 &&
        (fc->efc_single ||
         fc->efc_size < fc->efc_start + namlen * sizeof(WCHAR) + Ext2GetInfoLength(fc->efc_fi))) {
        return 1;
    }

    Oem.Buffer = (PCHAR)name;
    Oem.Length = (USHORT)(namlen & 0xFF);
    Oem.MaximumLength = Oem.Length;

    /* "." and ".." are reported for every directory but the root, the way
       NTFS and FAT report them: a first "*" query then always finds
       something, which Win32 callers rely on - skipping them made
       FindFirstFile on an empty directory fail with ERROR_NO_MORE_FILES,
       and .NET's Directory.Delete with it */
    if ((Oem.Length == 1 && name[0] == '.') ||
        (Oem.Length == 2 && name[0] == '.' && name[1] == '.')) {
        if (IsRoot(Fcb)) {
            return 0;
        }
    } else if (Oem.Length == 0 || Ext2IsWearingCloak(Vcb, &Oem)) {
        return 0;
    }

    /* an entry exists that Windows may see, matching the pattern or not:
       an empty directory is told apart from a pattern that matched nothing */
    fc->efc_visible = TRUE;

    Status = Ext2FillName(Vcb, fc, &Oem);
    if (!NT_SUCCESS(Status)) {
        fc->efc_status = Status;
        return 1;
    }

    if (FsRtlDoesNameContainWildCards(Pattern) ?
            !FsRtlIsNameInExpression(Pattern, &fc->efc_name, TRUE, NULL) :
            RtlCompareUnicodeString(Pattern, &fc->efc_name, TRUE) != 0) {
        return 0;
    }

    Start = CEILING_ALIGNED(ULONG, fc->efc_start, 8);
    Status = Ext2ProcessEntry(IrpContext, Vcb, Fcb, fc->efc_fi, ino, fc->efc_buf,
                              Start, fc->efc_size - Start, offset, &fc->efc_name,
                              &EntrySize, fc->efc_single);
    if (NT_SUCCESS(Status)) {
        if (EntrySize > 0) {
            fc->efc_prev = Start;
            fc->efc_start = Start + EntrySize;
        }
        return 0;
    }

    /* an entry that does not fit: the first one is returned cut, as
       STATUS_BUFFER_OVERFLOW; after others it is the next query's */
    if (Status == STATUS_BUFFER_OVERFLOW) {
        if (fc->efc_start == 0) {
            fc->efc_start = EntrySize;
            fc->efc_status = Status;
        }
    } else {
        fc->efc_status = Status;
    }
    return 1;
}

NTSTATUS
Ext2NotifyChangeDirectory (
    IN PEXT2_IRP_CONTEXT IrpContext
)
{
    PDEVICE_OBJECT      DeviceObject;
    BOOLEAN             CompleteRequest = TRUE;
    NTSTATUS            Status = STATUS_UNSUCCESSFUL;
    PEXT2_VCB           Vcb = NULL;
    PEXT2_FCB           Fcb = NULL;
    PEXT2_CCB           Ccb = NULL;
    PIRP                Irp = NULL;
    PIO_STACK_LOCATION  IrpSp;
    PFILE_OBJECT        FileObject;
    ULONG               CompletionFilter;
    BOOLEAN             WatchTree;

    BOOLEAN             bFcbAcquired = FALSE;

    __try {

        ASSERT(IrpContext);
        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        /* Always set the wait flag in the Irp context for the original request. */

        SetFlag( IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT );

        DeviceObject = IrpContext->DeviceObject;

        if (IsExt2FsDevice(DeviceObject)) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;

        ASSERT(Vcb != NULL);
        ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
               (Vcb->Identifier.Size == sizeof(EXT2_VCB)));

        FileObject = IrpContext->FileObject;
        Fcb = (PEXT2_FCB) FileObject->FsContext;
        ASSERT(Fcb);
        if (Fcb->Identifier.Type == EXT2VCB) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }
        ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
               (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

        Ccb = (PEXT2_CCB) FileObject->FsContext2;
        ASSERT(Ccb);
        ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
               (Ccb->Identifier.Size == sizeof(EXT2_CCB)));

        /* do nothing if target file was deleted */
        if (FlagOn(Fcb->Flags, FCB_DELETE_PENDING)) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        if (!IsDirectory(Fcb)) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        if (ExAcquireResourceExclusiveLite(
                    &Fcb->MainResource,
                    TRUE ))  {
            bFcbAcquired = TRUE;
        } else {
            Status = STATUS_PENDING;
            __leave;
        }

        Irp = IrpContext->Irp;

        IrpSp = IoGetCurrentIrpStackLocation(Irp);


        CompletionFilter =
            IrpSp->Parameters.NotifyDirectory.CompletionFilter;


        WatchTree = IsFlagOn(IrpSp->Flags, SL_WATCH_TREE);

        if (FlagOn(Fcb->Flags, FCB_DELETE_PENDING)) {
            Status = STATUS_DELETE_PENDING;
            __leave;
        }

        FsRtlNotifyFullChangeDirectory( Vcb->NotifySync,
                                        &Vcb->NotifyList,
                                        FileObject->FsContext2,
                                        (PSTRING)(&Fcb->Mcb->FullName),
                                        WatchTree,
                                        FALSE,
                                        CompletionFilter,
                                        Irp,
                                        NULL,
                                        NULL );

        CompleteRequest = FALSE;

        Status = STATUS_PENDING;

    } __finally {

        if (bFcbAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (!IrpContext->ExceptionInProgress) {
            if (CompleteRequest) {
                if (Status == STATUS_PENDING) {
                    Ext2QueueRequest(IrpContext);
                } else {
                    Ext2CompleteIrpContext(IrpContext, Status);
                }
            } else {
                IrpContext->Irp = NULL;
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }

    return Status;
}

VOID
Ext2NotifyReportChange (
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PEXT2_VCB         Vcb,
    IN PEXT2_MCB         Mcb,
    IN ULONG             Filter,
    IN ULONG             Action   )
{
    UNREFERENCED_PARAMETER(IrpContext);
    USHORT          Offset;

    Offset = (USHORT) ( Mcb->FullName.Length -
                        Mcb->ShortName.Length);

    FsRtlNotifyFullReportChange( Vcb->NotifySync,
                                 &(Vcb->NotifyList),
                                 (PSTRING) (&Mcb->FullName),
                                 (USHORT) Offset,
                                 (PSTRING)NULL,
                                 (PSTRING) NULL,
                                 (ULONG) Filter,
                                 (ULONG) Action,
                                 (PVOID) NULL );
}

/* a name came, went or changed: a directory's or a file's, as watchers filter them */
VOID
Ext2NotifyNameChange (
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PEXT2_VCB         Vcb,
    IN PEXT2_MCB         Mcb,
    IN ULONG             Action   )
{
    Ext2NotifyReportChange(IrpContext, Vcb, Mcb,
                           IsMcbDirectory(Mcb) ? FILE_NOTIFY_CHANGE_DIR_NAME :
                                                 FILE_NOTIFY_CHANGE_FILE_NAME,
                           Action);
}

NTSTATUS
Ext2DirectoryControl (IN PEXT2_IRP_CONTEXT IrpContext)
{
    NTSTATUS Status;

    ASSERT(IrpContext);

    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    switch (IrpContext->MinorFunction) {

    case IRP_MN_QUERY_DIRECTORY:
        Status = Ext2QueryDirectory(IrpContext);
        break;

    case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
        Status = Ext2NotifyChangeDirectory(IrpContext);
        break;

    default:
        Status = STATUS_INVALID_DEVICE_REQUEST;
        Ext2CompleteIrpContext(IrpContext, Status);
    }

    return Status;
}

BOOLEAN
Ext2IsDirectoryEmpty (
    PEXT2_IRP_CONTEXT   IrpContext,
    PEXT2_VCB           Vcb,
    PEXT2_MCB           Mcb
)
{
    UNREFERENCED_PARAMETER(Vcb);
    /* the inode decides: a link to a directory shows the directory
       attribute, and its i_block holds a target, not entries */
    if (!S_ISDIR(Mcb->Inode->i_mode)) {
        return TRUE;
    }

    return !!ext3_is_dir_empty(IrpContext, Mcb->Inode);
}
