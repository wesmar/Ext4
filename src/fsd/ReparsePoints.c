/**
 * ReparsePoints.c - symbolic links as NTFS-style reparse points.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "fsctl_internal.h"

NTSTATUS
Ext2InspectReparseData(
    IN PREPARSE_DATA_BUFFER RDB,
    IN ULONG InputBufferLength
)
{
    NTSTATUS Status = STATUS_SUCCESS;

    if (!RDB) {
        Status = STATUS_INVALID_PARAMETER;
        goto out;
    }

    if (InputBufferLength < sizeof(REPARSE_DATA_BUFFER)) {
        Status = STATUS_BUFFER_OVERFLOW;
        goto out;
    }

    /* ReparseDataLength counts what follows the 8-byte header */
    if (InputBufferLength < (ULONG)RDB->ReparseDataLength + REPARSE_DATA_BUFFER_HEADER_SIZE) {
        Status = STATUS_BUFFER_OVERFLOW;
        goto out;
    }

    if (RDB->ReparseTag != IO_REPARSE_TAG_SYMLINK) {
        Status = STATUS_NOT_IMPLEMENTED;
        goto out;
    }

    /* names are UTF-16: an odd offset or length would split a character */
    if ((RDB->SymbolicLinkReparseBuffer.SubstituteNameOffset |
         RDB->SymbolicLinkReparseBuffer.SubstituteNameLength |
         RDB->SymbolicLinkReparseBuffer.PrintNameOffset |
         RDB->SymbolicLinkReparseBuffer.PrintNameLength) & (sizeof(WCHAR) - 1)) {
        Status = STATUS_INVALID_PARAMETER;
        goto out;
    }

    if ((PUCHAR)RDB->SymbolicLinkReparseBuffer.PathBuffer
          + RDB->SymbolicLinkReparseBuffer.SubstituteNameOffset
          + RDB->SymbolicLinkReparseBuffer.SubstituteNameLength
        > (PUCHAR)RDB + InputBufferLength ) {
        Status = STATUS_BUFFER_OVERFLOW;
        goto out;
    }

    if ((PUCHAR)RDB->SymbolicLinkReparseBuffer.PathBuffer
          + RDB->SymbolicLinkReparseBuffer.PrintNameOffset
          + RDB->SymbolicLinkReparseBuffer.PrintNameLength
        > (PUCHAR)RDB + InputBufferLength) {
        Status = STATUS_BUFFER_OVERFLOW;
        goto out;
    }

    /* relative links map 1:1; absolute ones are accepted when they point
       into this volume (checked in Ext2SetReparsePoint) */
    if (RDB->SymbolicLinkReparseBuffer.Flags != SYMLINK_FLAG_RELATIVE &&
        RDB->SymbolicLinkReparseBuffer.Flags != 0) {
        Status = STATUS_NOT_IMPLEMENTED;
        goto out;
    }

out:
    return Status;
}

/*
 * The DOS name of this volume ("D:"). The mount manager knows it for the
 * volumes it handed a letter to; for a hidden volume the letter is ours
 * (DriveLetters.c) and the mount manager does not track it. The caller frees
 * Name->Buffer with ExFreePool on success.
 */
static NTSTATUS
Ext2VolumeDosName(IN PEXT2_VCB Vcb, OUT PUNICODE_STRING Name)
{
    NTSTATUS    Status;

    if (!Vcb->Vpb || !Vcb->Vpb->RealDevice) {
        return STATUS_NO_SUCH_DEVICE;
    }

    Status = IoVolumeDeviceToDosName(Vcb->Vpb->RealDevice, Name);
    if (NT_SUCCESS(Status) && Name->Length >= 2 * sizeof(WCHAR)) {
        return Status;
    }
    if (NT_SUCCESS(Status) && Name->Buffer) {
        ExFreePool(Name->Buffer);
    }

    if (Vcb->DrvLetter == 0) {
        return STATUS_NOT_FOUND;
    }
    Name->Buffer = ExAllocatePool2(POOL_FLAG_PAGED, 3 * sizeof(WCHAR), 'ND2E');
    if (Name->Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Name->Buffer[0] = (WCHAR)(Vcb->DrvLetter & 0x7F);
    Name->Buffer[1] = L':';
    Name->Buffer[2] = 0;
    Name->Length = 2 * sizeof(WCHAR);
    Name->MaximumLength = 3 * sizeof(WCHAR);
    return STATUS_SUCCESS;
}

/*
 * An absolute Windows link target ("\??\D:\dir\file") is a Linux absolute
 * path ("/dir/file") only if D: is this very volume. Strips the prefix in
 * place; anything else cannot be expressed on ext4 and is refused.
 */
static NTSTATUS
Ext2AbsoluteLinkToVolumePath(IN PEXT2_VCB Vcb, IN OUT PUNICODE_STRING Path)
{
    UNICODE_STRING  Dos = { 0 };
    USHORT          skip = 0;
    NTSTATUS        Status;

    /* "\??\" or "\\?\" */
    if (Path->Length >= 4 * sizeof(WCHAR) &&
        Path->Buffer[0] == L'\\' && Path->Buffer[3] == L'\\' &&
        ((Path->Buffer[1] == L'?' && Path->Buffer[2] == L'?') ||
         (Path->Buffer[1] == L'\\' && Path->Buffer[2] == L'?'))) {
        skip = 4;
    }

    /* "X:" */
    if (Path->Length < (skip + 2) * sizeof(WCHAR) || Path->Buffer[skip + 1] != L':') {
        return STATUS_NOT_IMPLEMENTED;
    }

    Status = Ext2VolumeDosName(Vcb, &Dos);
    if (!NT_SUCCESS(Status)) {
        return STATUS_NOT_IMPLEMENTED;
    }

    if (Dos.Length != 2 * sizeof(WCHAR) ||
        RtlUpcaseUnicodeChar(Dos.Buffer[0]) != RtlUpcaseUnicodeChar(Path->Buffer[skip])) {
        ExFreePool(Dos.Buffer);
        return STATUS_NOT_IMPLEMENTED;       /* another volume */
    }
    ExFreePool(Dos.Buffer);

    /* keep the part from the backslash after "X:" on; "X:" alone is "/" */
    skip += 2;
    Path->Buffer += skip;
    Path->Length -= skip * sizeof(WCHAR);
    Path->MaximumLength = Path->Length;
    if (Path->Length == 0) {
        static WCHAR root[] = L"\\";
        Path->Buffer = root;
        Path->Length = Path->MaximumLength = sizeof(WCHAR);
    }

    return STATUS_SUCCESS;
}

static_assert(offsetof(REPARSE_DATA_BUFFER, SymbolicLinkReparseBuffer.SubstituteNameOffset) ==
              REPARSE_DATA_BUFFER_HEADER_SIZE, "symlink data follows the reparse header");

/* symlink reparse header: print name first, substitute name after it,
   both lengths in bytes (as the REPARSE_DATA_BUFFER contract wants) */
VOID
Ext2InitializeReparseData(IN PREPARSE_DATA_BUFFER RDB, USHORT PrintLength,
                          USHORT SubstLength, ULONG Flags)
{
    RDB->ReparseTag = IO_REPARSE_TAG_SYMLINK;
    RDB->ReparseDataLength = FIELD_OFFSET(REPARSE_DATA_BUFFER, SymbolicLinkReparseBuffer.PathBuffer) -
                             REPARSE_DATA_BUFFER_HEADER_SIZE +
                             PrintLength + SubstLength;
    RDB->Reserved = 0;
    RDB->SymbolicLinkReparseBuffer.PrintNameOffset = 0;
    RDB->SymbolicLinkReparseBuffer.PrintNameLength = PrintLength;
    RDB->SymbolicLinkReparseBuffer.SubstituteNameOffset = PrintLength;
    RDB->SymbolicLinkReparseBuffer.SubstituteNameLength = SubstLength;
    RDB->SymbolicLinkReparseBuffer.Flags = Flags;
    RtlZeroMemory(&RDB->SymbolicLinkReparseBuffer.PathBuffer, PrintLength + SubstLength);
}

NTSTATUS
Ext2ReadSymlink (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN PVOID                Buffer,
    IN ULONG                Size,
    OUT PULONG              BytesRead
    )
{
    return Ext2ReadInode (  IrpContext,
                            Vcb,
                            Mcb,
                            0,
                            Buffer,
                            Size,
                            FALSE,
                            BytesRead);
}

NTSTATUS
Ext2GetReparsePoint (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PIRP                        Irp = NULL;
    PIO_STACK_LOCATION          IrpSp;

    PDEVICE_OBJECT      DeviceObject;

    PEXT2_VCB           Vcb = NULL;
    PEXT2_CCB           Ccb = NULL;
    PEXT2_MCB           Mcb = NULL;

    NTSTATUS            Status = STATUS_UNSUCCESSFUL;

    PVOID               OutputBuffer;
    ULONG               OutputBufferLength;
    ULONG               BytesRead = 0;

    PREPARSE_DATA_BUFFER RDB;

    UNICODE_STRING  UniName = { 0 };
    UNICODE_STRING  Dos = { 0 };
    OEM_STRING      OemName;

    PCHAR           OemNameBuffer = NULL;
    int             OemNameLength = 0, i;
    ULONG           UniLength, Needed;
    USHORT          PrintLength, SubstLength;
    BOOLEAN         Absolute;
    PUCHAR          Print, Subst;

    Ccb = IrpContext->Ccb;
    ASSERT(Ccb != NULL);
    ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
           (Ccb->Identifier.Size == sizeof(EXT2_CCB)));
    DeviceObject = IrpContext->DeviceObject;
    Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
    Mcb = IrpContext->Fcb->Mcb;
    Irp = IrpContext->Irp;
    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    __try {

        if (!Mcb || !IsInodeSymLink(Mcb->Inode) ||
            !IsFlagOn(Ccb->Flags, CCB_OPEN_REPARSE_POINT)) {
            Status = STATUS_NOT_A_REPARSE_POINT;
            __leave;
        }
        
        OutputBuffer  = (PVOID)Irp->AssociatedIrp.SystemBuffer;
        OutputBufferLength = IrpSp->Parameters.FileSystemControl.OutputBufferLength;

        RDB = (PREPARSE_DATA_BUFFER)OutputBuffer;
        if (!RDB) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }
        if (OutputBufferLength < sizeof(REPARSE_DATA_BUFFER)) {
            Status = STATUS_BUFFER_OVERFLOW;
            __leave;
        }

        OemNameLength = (ULONG)Mcb->Inode->i_size;
        if (OemNameLength > USHRT_MAX) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }
        OemName.Length = (USHORT)OemNameLength;
        OemName.MaximumLength = (USHORT)(OemNameLength + 1);
        OemNameBuffer = OemName.Buffer = Ext2AllocatePool(NonPagedPool,
                                          OemName.MaximumLength,
                                          'NL2E');
        if (!OemNameBuffer) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        Status = Ext2ReadSymlink(IrpContext,
                                 Vcb,
                                 Mcb,
                                 OemNameBuffer,
                                 OemNameLength,
                                 &BytesRead
                                );
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        OemName.Buffer[OemName.Length] = '\0';
        for (i = 0;i < OemName.Length;i++) {
            if (OemName.Buffer[i] == '/') {
                OemName.Buffer[i] = '\\';
            }
        }

        /*
         * The Windows view of the link text. A relative target is handed
         * over as it is; a Linux absolute path ("/etc/passwd") lives on this
         * volume, so it becomes "\??\D:\etc\passwd" for the I/O manager and
         * "D:\etc\passwd" for people (print name).
         */
        Absolute = (OemName.Length > 0 && OemName.Buffer[0] == '\\');
        if (Absolute) {
            Status = Ext2VolumeDosName(Vcb, &Dos);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
        }

        UniLength = Ext2OEMToUnicodeSize(Vcb, &OemName);       /* bytes */
        if (UniLength == 0 || UniLength > USHRT_MAX) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }
        PrintLength = (USHORT)UniLength + (Absolute ? Dos.Length : 0);
        SubstLength = PrintLength + (Absolute ? 4 * sizeof(WCHAR) : 0);
        Needed = FIELD_OFFSET(REPARSE_DATA_BUFFER, SymbolicLinkReparseBuffer.PathBuffer)
                 + PrintLength + SubstLength;
        if (OutputBufferLength < Needed) {
            Status = STATUS_BUFFER_TOO_SMALL;
            __leave;
        }

        UniName.Length = 0;
        UniName.MaximumLength = (USHORT)UniLength;
        UniName.Buffer = Ext2AllocatePool(PagedPool, UniName.MaximumLength, 'NL2E');
        if (!UniName.Buffer) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }
        Status = Ext2OEMToUnicode(Vcb, &UniName, &OemName);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Ext2InitializeReparseData(RDB, PrintLength, SubstLength,
                                  Absolute ? 0 : SYMLINK_FLAG_RELATIVE);
        Print = (PUCHAR)RDB->SymbolicLinkReparseBuffer.PathBuffer
                + RDB->SymbolicLinkReparseBuffer.PrintNameOffset;
        Subst = (PUCHAR)RDB->SymbolicLinkReparseBuffer.PathBuffer
                + RDB->SymbolicLinkReparseBuffer.SubstituteNameOffset;
        if (Absolute) {
            RtlCopyMemory(Print, Dos.Buffer, Dos.Length);
            RtlCopyMemory(Print + Dos.Length, UniName.Buffer, UniName.Length);
            RtlCopyMemory(Subst, L"\\??\\", 4 * sizeof(WCHAR));
            RtlCopyMemory(Subst + 4 * sizeof(WCHAR), Dos.Buffer, Dos.Length);
            RtlCopyMemory(Subst + 4 * sizeof(WCHAR) + Dos.Length, UniName.Buffer, UniName.Length);
        } else {
            RtlCopyMemory(Print, UniName.Buffer, UniName.Length);
            RtlCopyMemory(Subst, UniName.Buffer, UniName.Length);
        }

        Irp->IoStatus.Information = Needed;
        Status = STATUS_SUCCESS;

    } __finally {

        if (OemNameBuffer) {
            Ext2FreePool(OemNameBuffer, 'NL2E');
        }
        if (UniName.Buffer) {
            Ext2FreePool(UniName.Buffer, 'NL2E');
        }
        if (Dos.Buffer) {
            ExFreePool(Dos.Buffer);
        }

        if (!AbnormalTermination()) {
            if (Status == STATUS_PENDING || Status == STATUS_CANT_WAIT) {
                Status = Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }
    
    return Status;
}

NTSTATUS
Ext2WriteSymlink (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN PVOID                Buffer,
    IN ULONG                Size,
    OUT PULONG              BytesWritten
)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PUCHAR   Data = (PUCHAR)(&Mcb->Inode->i_block[0]);

    if (Size >= EXT2_LINKLEN_IN_INODE) {

        /* initialize inode i_block[] */
        if (0 == Mcb->Inode->i_blocks) {
            memset(Data, 0, EXT2_LINKLEN_IN_INODE);
            ClearFlag(Mcb->Inode->i_flags, EXT4_EXTENTS_FL);
            Ext2SaveInode(IrpContext, Vcb, Mcb->Inode);
        }

        Status = Ext2WriteInode(IrpContext, Vcb, Mcb,
                                0, Buffer, Size,
                                FALSE, BytesWritten);
        if (!NT_SUCCESS(Status)) {
            goto out;
        }

    } else {

        /* free inode blocks before writing in line */
        if (Mcb->Inode->i_blocks) {
            LARGE_INTEGER Zero = {0, 0};
            Ext2TruncateFile(IrpContext, Vcb, Mcb, &Zero);
        }

        ClearFlag(Mcb->Inode->i_flags, EXT4_EXTENTS_FL);
        memset(Data, 0, EXT2_LINKLEN_IN_INODE);
        RtlCopyMemory(Data, Buffer, Size);
    }

    Mcb->Inode->i_size = Size;
    Ext2SaveInode(IrpContext, Vcb, Mcb->Inode);

    if (BytesWritten) {
        *BytesWritten = Size;
    }

out:
    return Status;
}

NTSTATUS
Ext2SetReparsePoint (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PIRP                Irp = NULL;
    PIO_STACK_LOCATION  IrpSp;

    PDEVICE_OBJECT      DeviceObject;

    PEXT2_VCB           Vcb = NULL;
    PEXT2_FCB           Fcb = NULL;
    PEXT2_CCB           Ccb = NULL;
    PEXT2_MCB           Mcb = NULL;

    NTSTATUS            Status = STATUS_UNSUCCESSFUL;
    
    PVOID               InputBuffer;
    ULONG               InputBufferLength;
    ULONG               BytesWritten = 0;

    PEXT2_FCB           ParentDcb = NULL;   /* Dcb of it's current parent */
    PEXT2_MCB           ParentMcb = NULL;

    PREPARSE_DATA_BUFFER RDB;

    UNICODE_STRING      UniName;
    OEM_STRING          OemName;
    
    PCHAR               OemNameBuffer = NULL;
    int                 OemNameLength = 0, i;

    BOOLEAN             MainResourceAcquired = FALSE;
    BOOLEAN             FcbLockAcquired = FALSE;
    BOOLEAN             VcbResourceAcquired = FALSE;

    __try {

        Ccb = IrpContext->Ccb;
        ASSERT(Ccb != NULL);
        ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
               (Ccb->Identifier.Size == sizeof(EXT2_CCB)));
        DeviceObject = IrpContext->DeviceObject;
        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        Fcb = IrpContext->Fcb;
        Mcb = Fcb->Mcb;
        Irp = IrpContext->Irp;
        IrpSp = IoGetCurrentIrpStackLocation(Irp);

        /* as on NTFS, a symlink needs SeCreateSymbolicLinkPrivilege (an
           elevated administrator holds it). Checked first, still in the
           caller's thread: a posted request runs in a system worker, whose
           token would pass any check */
        if (Irp->RequestorMode != KernelMode &&
            !SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_CREATE_SYMBOLIC_LINK_PRIVILEGE),
                                    UserMode)) {
            Status = STATUS_PRIVILEGE_NOT_HELD;
            __leave;
        }

        /* turning a file into a symlink, or back, changes what its name
           resolves to: a namespace change, made under the exclusive volume
           resource like create, delete and rename (order: Vcb -> FcbLock
           -> Fcb), so no open can resolve the name halfway through */
        ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);
        VcbResourceAcquired = TRUE;

        ExAcquireResourceExclusiveLite(&Vcb->FcbLock, TRUE);
        FcbLockAcquired = TRUE;

        ParentMcb = Mcb->Parent;
        ParentDcb = ParentMcb->Icb->Fcb;
        if (ParentDcb == NULL) {
            ParentDcb = Ext2AllocateFcb(Vcb, ParentMcb);
        }
        if (ParentDcb) {
            Ext2ReferXcb(&ParentDcb->ReferenceCount);
        }

        if (!Mcb)
            __leave;

        if (FcbLockAcquired) {
            ExReleaseResourceLite(&Vcb->FcbLock);
            FcbLockAcquired = FALSE;
        }

        /* exclusive: the file data is truncated and rewritten below */
        if (!ExAcquireResourceExclusiveLite(
                    &Fcb->MainResource,
                    IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT) )) {
            Status = STATUS_PENDING;
            __leave;
        }
        MainResourceAcquired = TRUE;
        
        InputBuffer  = Irp->AssociatedIrp.SystemBuffer;
        InputBufferLength = IrpSp->Parameters.FileSystemControl.InputBufferLength;

        RDB = (PREPARSE_DATA_BUFFER)InputBuffer;
        Status = Ext2InspectReparseData(RDB, InputBufferLength);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        UniName.Length = RDB->SymbolicLinkReparseBuffer.SubstituteNameLength;
        UniName.MaximumLength = UniName.Length;
        UniName.Buffer =
            (PWCHAR)((PUCHAR)&RDB->SymbolicLinkReparseBuffer.PathBuffer
             + RDB->SymbolicLinkReparseBuffer.SubstituteNameOffset);

        if (RDB->SymbolicLinkReparseBuffer.Flags != SYMLINK_FLAG_RELATIVE) {
            Status = Ext2AbsoluteLinkToVolumePath(Vcb, &UniName);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
        }

        OemNameLength = Ext2UnicodeToOEMSize(Vcb, &UniName);
        if (OemNameLength > USHRT_MAX) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }
        OemName.Length = (USHORT)OemNameLength;
        OemName.MaximumLength = (USHORT)(OemNameLength + 1);
        OemNameBuffer = OemName.Buffer = Ext2AllocatePool(PagedPool,
                                          OemName.MaximumLength,
                                          'NL2E');
        if (!OemNameBuffer) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        Status = Ext2UnicodeToOEM(Vcb, &OemName, &UniName);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        /* the converter sets the exact byte count; the size estimate above
           may include room for a terminator, and a symlink's i_size must
           be the text length or e2fsck declares it invalid */
        OemNameLength = OemName.Length;
        OemName.Buffer[OemName.Length] = '\0';
        for (i = 0;i < OemName.Length;i++) {
            if (OemName.Buffer[i] == '\\') {
                OemName.Buffer[i] = '/';
            }
        }

        /* free all data blocks of the inode (to be set as symlink) */
        {
            LARGE_INTEGER zero = {0};
            Status = Ext2TruncateFile(IrpContext, Vcb, Mcb, &zero);
        }

        /* decrease dir count of group desc and vcb stat */
        if (S_ISDIR(Mcb->Inode->i_mode)) {

            ULONG group = (Mcb->Inode->i_ino - 1) / INODES_PER_GROUP;
            Ext2UpdateGroupDirStat(IrpContext, Vcb, group);

            /* a directory (mklink /D creates one first) becomes a symlink:
               its "." is gone, so exactly one link is left - set it, since
               ext3_dec_count deliberately never takes a directory below 2.
               The parent loses the ".." link in Ext2SetFileType below,
               which sees the mode change - not here, or it is taken twice */
            Mcb->Inode->i_nlink = 1;
        }

        /* overwrite inode mode as type SYMLINK */
        Ext2SaveInode(IrpContext, Vcb, Mcb->Inode);
        ClearFlag(Mcb->FileAttr, FILE_ATTRIBUTE_NORMAL);   /* NORMAL stands alone */
        SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_REPARSE_POINT);

        Status = Ext2WriteSymlink(IrpContext, Vcb, Mcb, OemNameBuffer,
                                  OemNameLength, &BytesWritten);
        if (NT_SUCCESS(Status)) {
            Ext2SetFileType(IrpContext, Vcb, ParentDcb, Mcb,
                            S_IFLNK | S_IRWXUGO);

            /* the in-memory Mcb was built for a regular file; make it the
               symlink it now is on disk (resolves the target, or marks it
               special when dangling), or the next open reads the link
               text as file data */
            ClearLongFlag(Mcb->Flags, MCB_TYPE_SPECIAL);
            Ext2FollowLink(IrpContext, Vcb, ParentMcb, Mcb, 0);
        }

    } __finally {

        if (FcbLockAcquired) {
            ExReleaseResourceLite(&Vcb->FcbLock);
            FcbLockAcquired = FALSE;
        }

        if (MainResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (VcbResourceAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }
        
        if (OemNameBuffer) {
            Ext2FreePool(OemNameBuffer, 'NL2E');
        }
        
        if (NT_SUCCESS(Status)) {
            Ext2NotifyReportChange(
                IrpContext,
                Vcb,
                Mcb,
                FILE_NOTIFY_CHANGE_ATTRIBUTES,
                FILE_ACTION_MODIFIED );
        }

        if (!AbnormalTermination()) {
            if (Status == STATUS_PENDING || Status == STATUS_CANT_WAIT) {
                Status = Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }

        if (ParentDcb) {
            Ext2ReleaseFcb(ParentDcb);
        }
    }
    
    return Status;
}

NTSTATUS
Ext2TruncateSymlink(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_MCB         Mcb,
    ULONG             Size
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    PUCHAR   data = (PUCHAR)&Mcb->Inode->i_block;
    ULONG    len = (ULONG)Mcb->Inode->i_size;
    LARGE_INTEGER NewSize;
    
    if (len < EXT2_LINKLEN_IN_INODE && !Mcb->Inode->i_blocks) {

        RtlZeroMemory(data + Size, EXT2_LINKLEN_IN_INODE - Size);
        Mcb->Inode->i_size = Size;
        Ext2SaveInode(IrpContext, Vcb, Mcb->Inode);

    } else {
        NewSize.QuadPart = Size;
        status = Ext2TruncateFile(IrpContext, Vcb, Mcb, &NewSize);
        if (!NT_SUCCESS(status)) {
            goto out;
        }
    }
    
out:
    return status;
}

/* A file carries at most one reparse point here: the symlink target, stored as the file data. */
NTSTATUS
Ext2DeleteReparsePoint (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PIRP                        Irp = NULL;

    PDEVICE_OBJECT      DeviceObject;

    PEXT2_VCB           Vcb = NULL;
    PEXT2_FCB           Fcb = NULL;
    PEXT2_CCB           Ccb = NULL;
    PEXT2_MCB           Mcb = NULL;

    PEXT2_FCB           ParentDcb = NULL;   /* Dcb of it's current parent */
    PEXT2_MCB           ParentMcb = NULL;

    NTSTATUS            Status = STATUS_UNSUCCESSFUL;

    BOOLEAN             FcbLockAcquired = FALSE;
    BOOLEAN             MainResourceAcquired = FALSE;
    BOOLEAN             VcbResourceAcquired = FALSE;

    __try {

        Ccb = IrpContext->Ccb;
        ASSERT(Ccb != NULL);
        ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
               (Ccb->Identifier.Size == sizeof(EXT2_CCB)));
        DeviceObject = IrpContext->DeviceObject;
        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        Mcb = IrpContext->Fcb->Mcb;
        Irp = IrpContext->Irp;

        /* turning a file into a symlink, or back, changes what its name
           resolves to: a namespace change, made under the exclusive volume
           resource like create, delete and rename (order: Vcb -> FcbLock
           -> Fcb), so no open can resolve the name halfway through */
        ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);
        VcbResourceAcquired = TRUE;

        ExAcquireResourceExclusiveLite(&Vcb->FcbLock, TRUE);
        FcbLockAcquired = TRUE;

        ParentMcb = Mcb->Parent;
        ParentDcb = ParentMcb->Icb->Fcb;
        if (ParentDcb == NULL) {
            ParentDcb = Ext2AllocateFcb(Vcb, ParentMcb);
        }
        if (ParentDcb) {
            Ext2ReferXcb(&ParentDcb->ReferenceCount);
        }

        if (!Mcb || !IsInodeSymLink(Mcb->Inode) ||
            !IsFlagOn(Ccb->Flags, CCB_OPEN_REPARSE_POINT)) {
            Status = STATUS_NOT_A_REPARSE_POINT;
            __leave;
        }

        Fcb = Ext2AllocateFcb (Vcb, Mcb);
        if (Fcb) {
            Ext2ReferXcb(&Fcb->ReferenceCount);
        } else {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        if (FcbLockAcquired) {
            ExReleaseResourceLite(&Vcb->FcbLock);
            FcbLockAcquired = FALSE;
        }

        /* exclusive: the file data is truncated and rewritten below */
        if (!ExAcquireResourceExclusiveLite(
                    &Fcb->MainResource,
                    IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT) )) {
            Status = STATUS_PENDING;
            __leave;
        }
        MainResourceAcquired = TRUE;

        Status = Ext2TruncateSymlink(IrpContext, Vcb, Mcb, 0);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* inode is to be removed */
        SetLongFlag(Ccb->Flags, CCB_DELETE_ON_CLOSE);

    } __finally {

        if (FcbLockAcquired) {
            ExReleaseResourceLite(&Vcb->FcbLock);
        }

        if (MainResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (VcbResourceAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }
        
        if (NT_SUCCESS(Status)) {
            Ext2NotifyReportChange(
                IrpContext,
                Vcb,
                Mcb,
                FILE_NOTIFY_CHANGE_ATTRIBUTES,
                FILE_ACTION_MODIFIED );

        }
        
        if (!AbnormalTermination()) {
            if (Status == STATUS_PENDING || Status == STATUS_CANT_WAIT) {
                Status = Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }

        if (ParentDcb) {
            Ext2ReleaseFcb(ParentDcb);
        }

        if (Fcb) {
            Ext2ReleaseFcb(Fcb);
        }
    }
    
    return Status;
}
