/**
 * fileinfo_query.c - IRP_MJ_QUERY_INFORMATION.
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

static int Ext2IterateAllEa(struct ext4_xattr_ref *xattr_ref, struct ext4_xattr_item *item, BOOL is_last)
{
    UNREFERENCED_PARAMETER(is_last);
    PULONG EaSize = xattr_ref->iter_arg;
    ULONG EaEntrySize = (ULONG)(4 + 1 + 1 + 2 + item->name_len + 1 + item->data_size);

    /* Windows sees user.* only (see ea.c) */
    if (item->name_index == EXT4_XATTR_INDEX_USER)
        *EaSize += EaEntrySize - 4;
    return EXT4_XATTR_ITERATE_CONT;
}

NTSTATUS
Ext2QueryFileInformation (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT          DeviceObject;
    NTSTATUS                Status = STATUS_UNSUCCESSFUL;
    PFILE_OBJECT            FileObject;
    PEXT2_VCB               Vcb = NULL;
    PEXT2_FCB               Fcb = NULL;
    PEXT2_MCB               Mcb = NULL;
    PEXT2_CCB               Ccb = NULL;
    PIRP                    Irp = NULL;
    PIO_STACK_LOCATION      IoStackLocation;
    FILE_INFORMATION_CLASS  FileInformationClass;
    ULONG                   Length;
    PVOID                   Buffer;
    BOOLEAN                 FcbResourceAcquired = FALSE;

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

        FileObject = IrpContext->FileObject;
        Fcb = (PEXT2_FCB) FileObject->FsContext;
        if (Fcb == NULL) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        //
        // This request is not allowed on volumes
        //
        if (Fcb->Identifier.Type == EXT2VCB) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        if (!((Fcb->Identifier.Type == EXT2FCB) &&
                (Fcb->Identifier.Size == sizeof(EXT2_FCB)))) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        Vcb = Fcb->Vcb;

        {
            if (!ExAcquireResourceSharedLite(
                        &Fcb->MainResource,
                        IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT)
                    )) {

                Status = STATUS_PENDING;
                __leave;
            }

            FcbResourceAcquired = TRUE;
        }

        Ccb = (PEXT2_CCB) FileObject->FsContext2;
        ASSERT(Ccb != NULL);
        ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
               (Ccb->Identifier.Size == sizeof(EXT2_CCB)));
        Mcb = Ext2CcbName(Fcb, Ccb);

        Irp = IrpContext->Irp;
        IoStackLocation = IoGetCurrentIrpStackLocation(Irp);
        FileInformationClass =
            IoStackLocation->Parameters.QueryFile.FileInformationClass;

        Length = IoStackLocation->Parameters.QueryFile.Length;
        Buffer = Irp->AssociatedIrp.SystemBuffer;
        RtlZeroMemory(Buffer, Length);

        switch (FileInformationClass) {

        case FileBasicInformation:
        {
            PFILE_BASIC_INFORMATION FileBasicInformation;

            if (Length < sizeof(FILE_BASIC_INFORMATION)) {
                Status = STATUS_BUFFER_OVERFLOW;
                __leave;
            }

            FileBasicInformation = (PFILE_BASIC_INFORMATION) Buffer;

            FileBasicInformation->CreationTime = Mcb->Icb->CreationTime;
            FileBasicInformation->LastAccessTime = Mcb->Icb->LastAccessTime;
            FileBasicInformation->LastWriteTime = Mcb->Icb->LastWriteTime;
            FileBasicInformation->ChangeTime = Mcb->Icb->ChangeTime;

            FileBasicInformation->FileAttributes = Mcb->FileAttr;
            if (IsLinkInvalid(Mcb)) {
                ClearFlag(FileBasicInformation->FileAttributes, FILE_ATTRIBUTE_DIRECTORY);
            }
            if (FileBasicInformation->FileAttributes == 0) {
                FileBasicInformation->FileAttributes = FILE_ATTRIBUTE_NORMAL;
            }

            Irp->IoStatus.Information = sizeof(FILE_BASIC_INFORMATION);
            Status = STATUS_SUCCESS;
        }
        break;

        case FileStandardInformation:
        {
            PFILE_STANDARD_INFORMATION FSI;

            if (Length < sizeof(FILE_STANDARD_INFORMATION)) {
                Status = STATUS_BUFFER_OVERFLOW;
                __leave;
            }

            FSI = (PFILE_STANDARD_INFORMATION) Buffer;

            FSI->NumberOfLinks = Mcb->Inode->i_nlink;

            if (IsVcbReadOnly(Fcb->Vcb))
                FSI->DeletePending = FALSE;
            else
                FSI->DeletePending = IsFlagOn(Fcb->Flags, FCB_DELETE_PENDING);

            if (IsLinkInvalid(Mcb)) {
                FSI->Directory = FALSE;
                FSI->AllocationSize.QuadPart = 0;
                FSI->EndOfFile.QuadPart = 0;
            } else if (IsMcbDirectory(Mcb)) {
                FSI->Directory = TRUE;
                FSI->AllocationSize.QuadPart = 0;
                FSI->EndOfFile.QuadPart = 0;
            } else {
                FSI->Directory = FALSE;
                FSI->AllocationSize = Fcb->Header.AllocationSize;
                FSI->EndOfFile = Fcb->Header.FileSize;
            }

            Irp->IoStatus.Information = sizeof(FILE_STANDARD_INFORMATION);
            Status = STATUS_SUCCESS;
        }
        break;

        case FileInternalInformation:
        {
            PFILE_INTERNAL_INFORMATION FileInternalInformation;

            if (Length < sizeof(FILE_INTERNAL_INFORMATION)) {
                Status = STATUS_BUFFER_OVERFLOW;
                __leave;
            }

            FileInternalInformation = (PFILE_INTERNAL_INFORMATION) Buffer;

            /* we use the inode number as the internal index */
            FileInternalInformation->IndexNumber.QuadPart = (LONGLONG)Mcb->Inode->i_ino;

            Irp->IoStatus.Information = sizeof(FILE_INTERNAL_INFORMATION);
            Status = STATUS_SUCCESS;
        }
        break;


        case FileEaInformation:
        {
            struct ext4_xattr_ref xattr_ref;
            PFILE_EA_INFORMATION FileEaInformation;

            if (Length < sizeof(FILE_EA_INFORMATION)) {
                Status = STATUS_BUFFER_OVERFLOW;
                __leave;
            }

            FileEaInformation = (PFILE_EA_INFORMATION) Buffer;
            FileEaInformation->EaSize = 0;

            Status = Ext2WinntError(ext4_fs_get_xattr_ref(IrpContext, Vcb, Fcb->Mcb, &xattr_ref));
            if (!NT_SUCCESS(Status))
                __leave;

            xattr_ref.iter_arg = &FileEaInformation->EaSize;
            ext4_fs_xattr_iterate(&xattr_ref, Ext2IterateAllEa);
            ext4_fs_put_xattr_ref(&xattr_ref);

            if (FileEaInformation->EaSize)
                FileEaInformation->EaSize += 4;

            Irp->IoStatus.Information = sizeof(FILE_EA_INFORMATION);
            Status = STATUS_SUCCESS;
        }
        break;

        /* The normalized name is the full name: ext4 has no short names,
           so nothing needs expanding. Answering this class matters: without
           it the filter manager normalizes a name for every minifilter
           (Defender, on every create) by querying the directory for each
           8.3-looking component - a scan of the whole directory per file. */
        case FileNormalizedNameInformation:
        case FileNameInformation:
        {
            PFILE_NAME_INFORMATION FileNameInformation;
            ULONG   BytesToCopy = 0;

            if (Length < (ULONG)FIELD_OFFSET(FILE_NAME_INFORMATION, FileName) +
                    Mcb->FullName.Length) {
                BytesToCopy = Length - FIELD_OFFSET(FILE_NAME_INFORMATION, FileName);
                Status = STATUS_BUFFER_OVERFLOW;
            } else {
                BytesToCopy = Mcb->FullName.Length;
                Status = STATUS_SUCCESS;
            }

            FileNameInformation = (PFILE_NAME_INFORMATION) Buffer;
            FileNameInformation->FileNameLength = Mcb->FullName.Length;

            RtlCopyMemory(
                FileNameInformation->FileName,
                Mcb->FullName.Buffer,
                BytesToCopy );

            Irp->IoStatus.Information = BytesToCopy +
                                        + FIELD_OFFSET(FILE_NAME_INFORMATION, FileName);
        }
        break;

        case FilePositionInformation:
        {
            PFILE_POSITION_INFORMATION FilePositionInformation;

            if (Length < sizeof(FILE_POSITION_INFORMATION)) {
                Status = STATUS_BUFFER_OVERFLOW;
                __leave;
            }

            FilePositionInformation = (PFILE_POSITION_INFORMATION) Buffer;
            FilePositionInformation->CurrentByteOffset =
                FileObject->CurrentByteOffset;

            Irp->IoStatus.Information = sizeof(FILE_POSITION_INFORMATION);
            Status = STATUS_SUCCESS;
        }
        break;

        case FileAllInformation:
        {
            PFILE_ALL_INFORMATION       FileAllInformation;
            PFILE_BASIC_INFORMATION     FileBasicInformation;
            PFILE_STANDARD_INFORMATION  FSI;
            PFILE_INTERNAL_INFORMATION  FileInternalInformation;
            PFILE_EA_INFORMATION        FileEaInformation;
            PFILE_POSITION_INFORMATION  FilePositionInformation;
            PFILE_NAME_INFORMATION      FileNameInformation;

            if (Length < sizeof(FILE_ALL_INFORMATION)) {
                Status = STATUS_BUFFER_OVERFLOW;
                __leave;
            }

            FileAllInformation = (PFILE_ALL_INFORMATION) Buffer;

            /* access, mode and alignment are the I/O manager's to fill; they
               must not carry whatever the buffer held before */
            RtlZeroMemory(FileAllInformation, FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation.FileName));

            FileBasicInformation =
                &FileAllInformation->BasicInformation;

            FSI =
                &FileAllInformation->StandardInformation;

            FileInternalInformation =
                &FileAllInformation->InternalInformation;

            FileEaInformation =
                &FileAllInformation->EaInformation;

            FilePositionInformation =
                &FileAllInformation->PositionInformation;

            FileNameInformation =
                &FileAllInformation->NameInformation;

            FileBasicInformation->CreationTime = Mcb->Icb->CreationTime;
            FileBasicInformation->LastAccessTime = Mcb->Icb->LastAccessTime;
            FileBasicInformation->LastWriteTime = Mcb->Icb->LastWriteTime;
            FileBasicInformation->ChangeTime = Mcb->Icb->ChangeTime;

            FileBasicInformation->FileAttributes = Mcb->FileAttr;
            if (IsMcbSymLink(Mcb) && IsFileDeleted(Mcb->Target)) {
                ClearFlag(FileBasicInformation->FileAttributes, FILE_ATTRIBUTE_DIRECTORY);
            }
            if (FileBasicInformation->FileAttributes == 0) {
                FileBasicInformation->FileAttributes = FILE_ATTRIBUTE_NORMAL;
            }

            FSI->NumberOfLinks = Mcb->Inode->i_nlink;

            if (IsVcbReadOnly(Fcb->Vcb))
                FSI->DeletePending = FALSE;
            else
                FSI->DeletePending = IsFlagOn(Fcb->Flags, FCB_DELETE_PENDING);

            if (IsLinkInvalid(Mcb)) {
                FSI->Directory = FALSE;
                FSI->AllocationSize.QuadPart = 0;
                FSI->EndOfFile.QuadPart = 0;
            } else if (IsDirectory(Fcb)) {
                FSI->Directory = TRUE;
                FSI->AllocationSize.QuadPart = 0;
                FSI->EndOfFile.QuadPart = 0;
            } else {
                FSI->Directory = FALSE;
                FSI->AllocationSize = Fcb->Header.AllocationSize;
                FSI->EndOfFile = Fcb->Header.FileSize;
            }

            // The "inode number"
            FileInternalInformation->IndexNumber.QuadPart = (LONGLONG)Mcb->Inode->i_ino;

            FileEaInformation->EaSize = 0;

            FilePositionInformation->CurrentByteOffset =
                FileObject->CurrentByteOffset;

            /* the path from the volume root, as FileNameInformation and NTFS */
            FileNameInformation->FileNameLength = Mcb->FullName.Length;

            if (Length < FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation.FileName) +
                    (ULONG)Mcb->FullName.Length) {
                Irp->IoStatus.Information = Length;
                Status = STATUS_BUFFER_OVERFLOW;
                RtlCopyMemory(
                    FileNameInformation->FileName,
                    Mcb->FullName.Buffer,
                    Length - FIELD_OFFSET(FILE_ALL_INFORMATION,
                                          NameInformation.FileName)
                );
                __leave;
            }

            RtlCopyMemory(
                FileNameInformation->FileName,
                Mcb->FullName.Buffer,
                Mcb->FullName.Length
            );

            Irp->IoStatus.Information = FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation.FileName) +
                                        Mcb->FullName.Length;

            Status = STATUS_SUCCESS;
        }
        break;

        /*
        case FileAlternateNameInformation:
        {
            // TODO: Handle FileAlternateNameInformation
        }
        */

        case FileNetworkOpenInformation:
        {
            PFILE_NETWORK_OPEN_INFORMATION PFNOI;

            if (Length < sizeof(FILE_NETWORK_OPEN_INFORMATION)) {
                Status = STATUS_BUFFER_OVERFLOW;
                __leave;
            }

            PFNOI = (PFILE_NETWORK_OPEN_INFORMATION) Buffer;

            PFNOI->FileAttributes = Mcb->FileAttr;
            if (IsLinkInvalid(Mcb)) {
                ClearFlag(PFNOI->FileAttributes, FILE_ATTRIBUTE_DIRECTORY);
                PFNOI->AllocationSize.QuadPart = 0;
                PFNOI->EndOfFile.QuadPart = 0;
            } else if (IsDirectory(Fcb)) {
                PFNOI->AllocationSize.QuadPart = 0;
                PFNOI->EndOfFile.QuadPart = 0;
            } else {
                PFNOI->AllocationSize = Fcb->Header.AllocationSize;
                PFNOI->EndOfFile      = Fcb->Header.FileSize;
            }

            if (PFNOI->FileAttributes == 0) {
                PFNOI->FileAttributes = FILE_ATTRIBUTE_NORMAL;
            }

            PFNOI->CreationTime   = Mcb->Icb->CreationTime;
            PFNOI->LastAccessTime = Mcb->Icb->LastAccessTime;
            PFNOI->LastWriteTime  = Mcb->Icb->LastWriteTime;
            PFNOI->ChangeTime     = Mcb->Icb->ChangeTime;


            Irp->IoStatus.Information =
                sizeof(FILE_NETWORK_OPEN_INFORMATION);
            Status = STATUS_SUCCESS;
        }
        break;


        case FileAttributeTagInformation:
        {
            PFILE_ATTRIBUTE_TAG_INFORMATION FATI;

            if (Length < sizeof(FILE_ATTRIBUTE_TAG_INFORMATION)) {
                Status = STATUS_BUFFER_OVERFLOW;
                __leave;
            }

            FATI = (PFILE_ATTRIBUTE_TAG_INFORMATION) Buffer;
            FATI->FileAttributes = Mcb->FileAttr;
            if (IsLinkInvalid(Mcb)) {
                ClearFlag(FATI->FileAttributes, FILE_ATTRIBUTE_DIRECTORY);
            }
            /* the tag goes with the attribute, as on NTFS. DeleteFileW reads
               it: a reparse point without a name-surrogate tag is deleted
               through what it points to, so with tag 0 every symlink was
               reopened through its target - and one whose target was gone
               could not be deleted at all (access denied) */
            if (IsInodeSymLink(Mcb->Inode)) {
                SetFlag(FATI->FileAttributes, FILE_ATTRIBUTE_REPARSE_POINT);
                ClearFlag(FATI->FileAttributes, FILE_ATTRIBUTE_NORMAL);
                FATI->ReparseTag = IO_REPARSE_TAG_SYMLINK;
            } else {
                ClearFlag(FATI->FileAttributes, FILE_ATTRIBUTE_REPARSE_POINT);
                FATI->ReparseTag = IO_REPARSE_TAG_RESERVED_ZERO;
            }
            if (FATI->FileAttributes == 0) {
                FATI->FileAttributes = FILE_ATTRIBUTE_NORMAL;
            }
            Irp->IoStatus.Information = sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);
            Status = STATUS_SUCCESS;
        }
        break;

        case FileStreamInformation:
            Status = STATUS_INVALID_PARAMETER;
            break;

        case FileHardLinkInformation:
        case FileHardLinkFullIdInformation:
            Status = Ext2QueryHardLinks(Vcb, Mcb, Buffer, Length,
                                        FileInformationClass == FileHardLinkFullIdInformation,
                                        &Irp->IoStatus.Information);
            break;

        case FileStandardLinkInformation:
        {
            PFILE_STANDARD_LINK_INFORMATION FSLI = (PFILE_STANDARD_LINK_INFORMATION)Buffer;

            if (Length < sizeof(FILE_STANDARD_LINK_INFORMATION)) {
                Status = STATUS_BUFFER_OVERFLOW;
                __leave;
            }
            FSLI->NumberOfAccessibleLinks = Mcb->Inode->i_nlink;
            FSLI->TotalNumberOfLinks = Mcb->Inode->i_nlink;
            FSLI->DeletePending = IsFlagOn(Fcb->Flags, FCB_DELETE_PENDING) ||
                                  IsFlagOn(Mcb->Flags, MCB_DELETE_PENDING);
            FSLI->Directory = IsMcbDirectory(Mcb) && !IsLinkInvalid(Mcb);
            Irp->IoStatus.Information = sizeof(FILE_STANDARD_LINK_INFORMATION);
            Status = STATUS_SUCCESS;
        }
        break;

        case FileIdInformation:
        {
            PFILE_ID_INFORMATION FII = (PFILE_ID_INFORMATION)Buffer;

            if (Length < sizeof(FILE_ID_INFORMATION)) {
                Status = STATUS_BUFFER_OVERFLOW;
                __leave;
            }
            FII->VolumeSerialNumber = Vcb->Vpb->SerialNumber;
            RtlZeroMemory(&FII->FileId, sizeof(FII->FileId));
            *(PULONGLONG)FII->FileId.Identifier = (ULONGLONG)Mcb->Inode->i_ino;
            Irp->IoStatus.Information = sizeof(FILE_ID_INFORMATION);
            Status = STATUS_SUCCESS;
        }
        break;

        default:
            DEBUG(DL_WRN, ( "Ext2QueryInformation: invalid class: %d\n",
                            FileInformationClass));
            Status = STATUS_INVALID_PARAMETER; /* STATUS_INVALID_INFO_CLASS; */
            break;
        }

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
