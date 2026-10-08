/**
 * FileQuery.c - IRP_MJ_QUERY_INFORMATION.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "linux\ext4.h"
#include "linux\ext4_xattr.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2QueryFileInformation)
#endif

/*
 * FileHardLinkInformation / FileHardLinkFullIdInformation: every name of
 * this inode that the driver knows. ext4 keeps no back references from an
 * inode to its names, so this is the set of cached names (Icb->Names): the
 * one the handle came through, links created or looked up while the inode
 * stayed cached. Names are paged and the list is under a spin lock, so
 * the names are referenced first and copied afterwards.
 */
NTSTATUS
Ext2QueryHardLinks(
    IN PEXT2_VCB    Vcb,
    IN PEXT2_MCB    Mcb,
    OUT PVOID       Buffer,
    IN ULONG        Length,
    IN BOOLEAN      FullId,
    OUT PULONG_PTR  Information
)
{
    PEXT2_ICB       Icb = Mcb->Icb;
    PEXT2_MCB      *Names = NULL;
    PLIST_ENTRY     List;
    PULONG          PrevNext = NULL;
    ULONG           Count = 0, Max = 0, i;
    ULONG           Header, EntryHeader, Used, Needed, EntrySize;
    KIRQL           Irql;
    NTSTATUS        Status = STATUS_SUCCESS;

    Header = (ULONG)FIELD_OFFSET(FILE_LINKS_INFORMATION, Entry);
    EntryHeader = FullId ?
        (ULONG)FIELD_OFFSET(FILE_LINK_ENTRY_FULL_ID_INFORMATION, FileName) :
        (ULONG)FIELD_OFFSET(FILE_LINK_ENTRY_INFORMATION, FileName);

    if (Length < Header) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    KeAcquireSpinLock(&Vcb->IcbLock, &Irql);
    for (List = Icb->Names.Flink; List != &Icb->Names; List = List->Flink) {
        Max++;
    }
    KeReleaseSpinLock(&Vcb->IcbLock, Irql);

    if (Max) {
        Names = Ext2AllocatePool(NonPagedPool, Max * sizeof(PEXT2_MCB), EXT2_FLIST_MAGIC);
        if (Names == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        KeAcquireSpinLock(&Vcb->IcbLock, &Irql);
        for (List = Icb->Names.Flink; List != &Icb->Names && Count < Max; List = List->Flink) {
            PEXT2_MCB Name = CONTAINING_RECORD(List, EXT2_MCB, IcbLink);
            if (!IsFileDeleted(Name) && Name->Parent && Name->Parent->Inode) {
                Ext2ReferMcb(Name);
                Names[Count++] = Name;
            }
        }
        KeReleaseSpinLock(&Vcb->IcbLock, Irql);
    }

    /* both layouts start with BytesNeeded, EntriesReturned */
    ((PFILE_LINKS_INFORMATION)Buffer)->BytesNeeded = 0;
    ((PFILE_LINKS_INFORMATION)Buffer)->EntriesReturned = 0;
    Used = Needed = Header;

    for (i = 0; i < Count; i++) {

        PEXT2_MCB   Name = Names[i];
        PUCHAR      Entry;

        EntrySize = EntryHeader + Name->ShortName.Length;

        /* entries are quadword aligned; the last one needs no padding */
        if (PrevNext) {
            Used = (Used + 7) & ~7;
            Needed = (Needed + 7) & ~7;
        }
        Needed += EntrySize;

        if (Status != STATUS_SUCCESS || Used + EntrySize > Length) {
            Status = STATUS_BUFFER_OVERFLOW;
            continue;
        }

        Entry = (PUCHAR)Buffer + Used;
        if (FullId) {
            PFILE_LINK_ENTRY_FULL_ID_INFORMATION E = (PFILE_LINK_ENTRY_FULL_ID_INFORMATION)Entry;
            E->NextEntryOffset = 0;
            RtlZeroMemory(&E->ParentFileId, sizeof(E->ParentFileId));
            *(PULONGLONG)E->ParentFileId.Identifier = (ULONGLONG)Name->Parent->Inode->i_ino;
            E->FileNameLength = Name->ShortName.Length / sizeof(WCHAR);
            RtlCopyMemory(E->FileName, Name->ShortName.Buffer, Name->ShortName.Length);
            if (PrevNext) {
                *PrevNext = (ULONG)(Entry - (PUCHAR)CONTAINING_RECORD(PrevNext, FILE_LINK_ENTRY_FULL_ID_INFORMATION, NextEntryOffset));
            }
            PrevNext = &E->NextEntryOffset;
        } else {
            PFILE_LINK_ENTRY_INFORMATION E = (PFILE_LINK_ENTRY_INFORMATION)Entry;
            E->NextEntryOffset = 0;
            E->ParentFileId = (LONGLONG)Name->Parent->Inode->i_ino;
            E->FileNameLength = Name->ShortName.Length / sizeof(WCHAR);
            RtlCopyMemory(E->FileName, Name->ShortName.Buffer, Name->ShortName.Length);
            if (PrevNext) {
                *PrevNext = (ULONG)(Entry - (PUCHAR)CONTAINING_RECORD(PrevNext, FILE_LINK_ENTRY_INFORMATION, NextEntryOffset));
            }
            PrevNext = &E->NextEntryOffset;
        }

        Used += EntrySize;
        ((PFILE_LINKS_INFORMATION)Buffer)->EntriesReturned++;
    }

    ((PFILE_LINKS_INFORMATION)Buffer)->BytesNeeded = Needed;
    *Information = Used;

    for (i = 0; i < Count; i++) {
        Ext2DerefMcb(Names[i]);
    }
    if (Names) {
        Ext2FreePool(Names, EXT2_FLIST_MAGIC);
    }

    return Status;
}

/* ---------------------------------------------------------------- one query */

/* the file a query is about, as Ext2QueryFileInformation found it */
typedef struct _EXT2_QUERY {
    PEXT2_IRP_CONTEXT   IrpContext;
    PEXT2_VCB           Vcb;
    PEXT2_FCB           Fcb;
    PEXT2_MCB           Mcb;        /* the name the handle came through */
    PFILE_OBJECT        FileObject;
    BOOLEAN             Dangling;   /* a symbolic link whose target is gone */
} EXT2_QUERY, *PEXT2_QUERY;

/* the four times lead both structures, in the same order */
C_ASSERT(FIELD_OFFSET(FILE_BASIC_INFORMATION, CreationTime) == 0 &&
         FIELD_OFFSET(FILE_NETWORK_OPEN_INFORMATION, CreationTime) == 0 &&
         FIELD_OFFSET(FILE_BASIC_INFORMATION, ChangeTime) ==
         FIELD_OFFSET(FILE_NETWORK_OPEN_INFORMATION, ChangeTime) &&
         FIELD_OFFSET(FILE_BASIC_INFORMATION, ChangeTime) == 3 * sizeof(LARGE_INTEGER));

/* creation, last access, last write and change time, in that order */
static VOID
Ext2QueryTimes(IN PEXT2_QUERY Q, OUT PLARGE_INTEGER Times)
{
    PEXT2_ICB Icb = Q->Mcb->Icb;

    Times[0] = Icb->CreationTime;
    Times[1] = Icb->LastAccessTime;
    Times[2] = Icb->LastWriteTime;
    Times[3] = Icb->ChangeTime;
}

/* the attributes Windows is shown: a link whose target is gone is no
   directory, and a file with none reads as NORMAL */
static ULONG
Ext2QueryAttributes(IN PEXT2_QUERY Q)
{
    ULONG Attributes = Q->Mcb->FileAttr;

    if (Q->Dangling) {
        ClearFlag(Attributes, FILE_ATTRIBUTE_DIRECTORY);
    }
    return Attributes != 0 ? Attributes : FILE_ATTRIBUTE_NORMAL;
}

/* a directory and a link whose target is gone have no size to show */
static BOOLEAN
Ext2QueryIsDirectory(IN PEXT2_QUERY Q)
{
    return !Q->Dangling && IsMcbDirectory(Q->Mcb);
}

static VOID
Ext2QuerySizes(IN PEXT2_QUERY Q, OUT PLARGE_INTEGER AllocationSize, OUT PLARGE_INTEGER EndOfFile)
{
    if (Q->Dangling || IsMcbDirectory(Q->Mcb)) {
        AllocationSize->QuadPart = 0;
        EndOfFile->QuadPart = 0;
    } else {
        *AllocationSize = Q->Fcb->Header.AllocationSize;
        *EndOfFile = Q->Fcb->Header.FileSize;
    }
}

static VOID
Ext2QueryBasic(IN PEXT2_QUERY Q, OUT PFILE_BASIC_INFORMATION FBI)
{
    Ext2QueryTimes(Q, &FBI->CreationTime);
    FBI->FileAttributes = Ext2QueryAttributes(Q);
}

static VOID
Ext2QueryStandard(IN PEXT2_QUERY Q, OUT PFILE_STANDARD_INFORMATION FSI)
{
    FSI->NumberOfLinks = Q->Mcb->Inode->i_nlink;
    /* nothing is deleted on a volume that stopped writing */
    FSI->DeletePending = !IsVcbReadOnly(Q->Vcb) && IsFlagOn(Q->Fcb->Flags, FCB_DELETE_PENDING);
    FSI->Directory = Ext2QueryIsDirectory(Q);
    Ext2QuerySizes(Q, &FSI->AllocationSize, &FSI->EndOfFile);
}

/* The path from the volume root, as NTFS answers it - built now, from the
   names up the tree as they are (a directory renamed above the file shows
   in it). Room is what the buffer has for the name; what does not fit is
   cut, with STATUS_BUFFER_OVERFLOW. */
static NTSTATUS
Ext2QueryName(IN PEXT2_QUERY Q, OUT PFILE_NAME_INFORMATION FNI, IN ULONG Room, OUT PULONG Copied)
{
    UNICODE_STRING  Name;
    NTSTATUS        Status;

    *Copied = 0;
    Status = Ext2GetFullName(Q->Vcb, Q->Mcb, &Name, NULL);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    *Copied = min(Room, (ULONG)Name.Length);
    FNI->FileNameLength = Name.Length;
    RtlCopyMemory(FNI->FileName, Name.Buffer, *Copied);
    Status = *Copied < Name.Length ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    Ext2FreeFullName(&Name);
    return Status;
}

/* FileAllInformation: the classes above in one. Access, mode and alignment
   are the I/O manager's to fill. The extended attributes are not read
   here: this is the stat of the Win32 API (GetFileInformationByHandle), and
   an attribute block read for every stat costs more than the size is worth;
   FileEaInformation reads it. */
static NTSTATUS
Ext2QueryAll(IN PEXT2_QUERY Q, OUT PFILE_ALL_INFORMATION FAI, IN ULONG Length, OUT PULONG_PTR Information)
{
    const ULONG Header = FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation.FileName);
    ULONG       Copied;
    NTSTATUS    Status;

    if (Length < Header) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    Ext2QueryBasic(Q, &FAI->BasicInformation);
    Ext2QueryStandard(Q, &FAI->StandardInformation);
    FAI->InternalInformation.IndexNumber.QuadPart = (LONGLONG)Q->Mcb->Inode->i_ino;
    FAI->EaInformation.EaSize = 0;
    FAI->PositionInformation.CurrentByteOffset = Q->FileObject->CurrentByteOffset;

    Status = Ext2QueryName(Q, &FAI->NameInformation, Length - Header, &Copied);
    *Information = Header + Copied;
    return Status;
}

/* FileAttributeTagInformation: a symbolic link shows its tag with the
   attribute, as on NTFS. DeleteFileW reads it: a reparse point without a
   name-surrogate tag is deleted through what it points to, so with tag 0
   every symlink was reopened through its target - and one whose target was
   gone could not be deleted at all (access denied). */
static VOID
Ext2QueryAttributeTag(IN PEXT2_QUERY Q, OUT PFILE_ATTRIBUTE_TAG_INFORMATION FATI)
{
    ULONG Attributes = Ext2QueryAttributes(Q);

    if (IsInodeSymLink(Q->Mcb->Inode)) {
        SetFlag(Attributes, FILE_ATTRIBUTE_REPARSE_POINT);
        ClearFlag(Attributes, FILE_ATTRIBUTE_NORMAL);
        FATI->ReparseTag = IO_REPARSE_TAG_SYMLINK;
    } else {
        ClearFlag(Attributes, FILE_ATTRIBUTE_REPARSE_POINT);
        FATI->ReparseTag = IO_REPARSE_TAG_RESERVED_ZERO;
    }
    FATI->FileAttributes = Attributes != 0 ? Attributes : FILE_ATTRIBUTE_NORMAL;
}

/* the size of a class whose answer always has the same size; 0 otherwise */
static ULONG
Ext2QueryFixedSize(IN FILE_INFORMATION_CLASS Class)
{
    switch (Class) {
    case FileBasicInformation:          return sizeof(FILE_BASIC_INFORMATION);
    case FileStandardInformation:       return sizeof(FILE_STANDARD_INFORMATION);
    case FileInternalInformation:       return sizeof(FILE_INTERNAL_INFORMATION);
    case FileEaInformation:             return sizeof(FILE_EA_INFORMATION);
    case FilePositionInformation:       return sizeof(FILE_POSITION_INFORMATION);
    case FileNetworkOpenInformation:    return sizeof(FILE_NETWORK_OPEN_INFORMATION);
    case FileAttributeTagInformation:   return sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);
    case FileStandardLinkInformation:   return sizeof(FILE_STANDARD_LINK_INFORMATION);
    case FileIdInformation:             return sizeof(FILE_ID_INFORMATION);
    default:                            return 0;
    }
}

/* one class into Buffer (zeroed, Length bytes); Information is what was filled */
static NTSTATUS
Ext2QueryClass(IN PEXT2_QUERY Q, IN FILE_INFORMATION_CLASS Class,
               OUT PVOID Buffer, IN ULONG Length, OUT PULONG_PTR Information)
{
    ULONG       Fixed = Ext2QueryFixedSize(Class);
    ULONG       Copied;
    NTSTATUS    Status = STATUS_SUCCESS;

    /* a structure that does not fit is not begun: there is no part of it
       to return (a name is another matter - it is cut) */
    if (Length < Fixed) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    *Information = Fixed;

    switch (Class) {

    case FileBasicInformation:
        Ext2QueryBasic(Q, (PFILE_BASIC_INFORMATION)Buffer);
        break;

    case FileStandardInformation:
        Ext2QueryStandard(Q, (PFILE_STANDARD_INFORMATION)Buffer);
        break;

    case FileInternalInformation:
        /* the inode number is the file's index */
        ((PFILE_INTERNAL_INFORMATION)Buffer)->IndexNumber.QuadPart = (LONGLONG)Q->Mcb->Inode->i_ino;
        break;

    case FileEaInformation:
        Status = Ext2EaListSize(Q->IrpContext, Q->Vcb, Q->Fcb->Mcb, &((PFILE_EA_INFORMATION)Buffer)->EaSize);
        break;

    case FilePositionInformation:
        ((PFILE_POSITION_INFORMATION)Buffer)->CurrentByteOffset = Q->FileObject->CurrentByteOffset;
        break;

    /* The normalized name is the full name: ext4 has no short names, so
       nothing needs expanding. Answering this class matters: without it
       the filter manager normalizes a name for every minifilter (Defender,
       on every create) by querying the directory for each 8.3-looking
       component - a scan of the whole directory per file. */
    case FileNormalizedNameInformation:
    case FileNameInformation:
        if (Length < (ULONG)FIELD_OFFSET(FILE_NAME_INFORMATION, FileName)) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        Status = Ext2QueryName(Q, (PFILE_NAME_INFORMATION)Buffer,
                               Length - FIELD_OFFSET(FILE_NAME_INFORMATION, FileName), &Copied);
        *Information = FIELD_OFFSET(FILE_NAME_INFORMATION, FileName) + Copied;
        break;

    case FileAllInformation:
        Status = Ext2QueryAll(Q, (PFILE_ALL_INFORMATION)Buffer, Length, Information);
        break;

    case FileNetworkOpenInformation:
    {
        PFILE_NETWORK_OPEN_INFORMATION FNOI = (PFILE_NETWORK_OPEN_INFORMATION)Buffer;

        Ext2QueryTimes(Q, &FNOI->CreationTime);
        Ext2QuerySizes(Q, &FNOI->AllocationSize, &FNOI->EndOfFile);
        FNOI->FileAttributes = Ext2QueryAttributes(Q);
        break;
    }

    case FileAttributeTagInformation:
        Ext2QueryAttributeTag(Q, (PFILE_ATTRIBUTE_TAG_INFORMATION)Buffer);
        break;

    case FileHardLinkInformation:
    case FileHardLinkFullIdInformation:
        Status = Ext2QueryHardLinks(Q->Vcb, Q->Mcb, Buffer, Length,
                                    Class == FileHardLinkFullIdInformation, Information);
        break;

    case FileStandardLinkInformation:
    {
        PFILE_STANDARD_LINK_INFORMATION FSLI = (PFILE_STANDARD_LINK_INFORMATION)Buffer;

        FSLI->NumberOfAccessibleLinks = Q->Mcb->Inode->i_nlink;
        FSLI->TotalNumberOfLinks = Q->Mcb->Inode->i_nlink;
        FSLI->DeletePending = IsFlagOn(Q->Fcb->Flags, FCB_DELETE_PENDING) ||
                              IsFlagOn(Q->Mcb->Flags, MCB_DELETE_PENDING);
        FSLI->Directory = Ext2QueryIsDirectory(Q);
        break;
    }

    case FileIdInformation:
    {
        PFILE_ID_INFORMATION FII = (PFILE_ID_INFORMATION)Buffer;

        FII->VolumeSerialNumber = Q->Vcb->Vpb->SerialNumber;
        *(PULONGLONG)FII->FileId.Identifier = (ULONGLONG)Q->Mcb->Inode->i_ino;
        break;
    }

    default:
        /* FileStreamInformation among them: ext4 has no named streams */
        DEBUG(DL_WRN, ("Ext2QueryInformation: invalid class: %d\n", Class));
        *Information = 0;
        return STATUS_INVALID_PARAMETER;
    }

    if (NT_ERROR(Status)) {
        *Information = 0;
    }
    return Status;
}

NTSTATUS
Ext2QueryFileInformation (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT          DeviceObject;
    NTSTATUS                Status = STATUS_UNSUCCESSFUL;
    PFILE_OBJECT            FileObject;
    PEXT2_FCB               Fcb = NULL;
    PEXT2_CCB               Ccb;
    PIRP                    Irp;
    PIO_STACK_LOCATION      IoStackLocation;
    EXT2_QUERY              Q;
    ULONG                   Length;
    BOOLEAN                 FcbResourceAcquired = FALSE;

    __try {

        ASSERT(IrpContext != NULL);
        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        DeviceObject = IrpContext->DeviceObject;

        /* This request is not allowed on the main device object */
        if (IsExt2FsDevice(DeviceObject)) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        FileObject = IrpContext->FileObject;
        Fcb = (PEXT2_FCB) FileObject->FsContext;

        /* nor on volumes */
        if (Fcb == NULL || Fcb->Identifier.Type != EXT2FCB ||
            Fcb->Identifier.Size != sizeof(EXT2_FCB)) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        if (!ExAcquireResourceSharedLite(&Fcb->MainResource,
                                         IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT))) {
            Status = STATUS_PENDING;
            __leave;
        }
        FcbResourceAcquired = TRUE;

        Ccb = (PEXT2_CCB) FileObject->FsContext2;
        ASSERT(Ccb != NULL);
        ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
               (Ccb->Identifier.Size == sizeof(EXT2_CCB)));

        Q.IrpContext = IrpContext;
        Q.Vcb = Fcb->Vcb;
        Q.Fcb = Fcb;
        Q.Mcb = Ext2CcbName(Fcb, Ccb);
        Q.FileObject = FileObject;
        Q.Dangling = IsLinkInvalid(Q.Vcb, Q.Mcb);

        Irp = IrpContext->Irp;
        IoStackLocation = IoGetCurrentIrpStackLocation(Irp);
        Length = IoStackLocation->Parameters.QueryFile.Length;
        RtlZeroMemory(Irp->AssociatedIrp.SystemBuffer, Length);

        Status = Ext2QueryClass(&Q, IoStackLocation->Parameters.QueryFile.FileInformationClass,
                                Irp->AssociatedIrp.SystemBuffer, Length,
                                &Irp->IoStatus.Information);

    } __finally {

        if (FcbResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (!IrpContext->ExceptionInProgress) {
            if (Status == STATUS_PENDING ||
                    Status == STATUS_CANT_WAIT) {
                Status = Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext,  Status);
            }
        }
    }

    return Status;
}
