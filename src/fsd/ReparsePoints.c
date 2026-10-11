/**
 * ReparsePoints.c - symbolic links as NTFS-style reparse points.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "fsctl_internal.h"

/* what goes before an absolute target for the I/O manager */
static const WCHAR Ext2NtPrefix[] = L"\\??\\";
#define EXT2_NT_PREFIX_BYTES    (sizeof(Ext2NtPrefix) - sizeof(WCHAR))

/* the symlink data before its names */
#define EXT2_SYMLINK_HEADER     ((ULONG)FIELD_OFFSET(REPARSE_DATA_BUFFER, SymbolicLinkReparseBuffer.PathBuffer))

static_assert(offsetof(REPARSE_DATA_BUFFER, SymbolicLinkReparseBuffer.SubstituteNameOffset) ==
              REPARSE_DATA_BUFFER_HEADER_SIZE, "symlink data follows the reparse header");

/*
 * A symlink reparse buffer as FSCTL_SET_REPARSE_POINT hands it over: the
 * data it declares is there, both names lie inside that data and hold
 * whole UTF-16 characters, and there is a target. Other tags have nowhere
 * to go on ext4.
 */
static NTSTATUS
Ext2InspectReparseData(IN PREPARSE_DATA_BUFFER RDB, IN ULONG InputBufferLength)
{
    ULONG   Names;      /* bytes of PathBuffer the data declares */

    if (RDB == NULL || InputBufferLength < REPARSE_DATA_BUFFER_HEADER_SIZE ||
        InputBufferLength < (ULONG)RDB->ReparseDataLength + REPARSE_DATA_BUFFER_HEADER_SIZE) {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    if (RDB->ReparseTag != IO_REPARSE_TAG_SYMLINK) {
        return STATUS_NOT_IMPLEMENTED;
    }

    if ((ULONG)RDB->ReparseDataLength + REPARSE_DATA_BUFFER_HEADER_SIZE < EXT2_SYMLINK_HEADER) {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    Names = (ULONG)RDB->ReparseDataLength + REPARSE_DATA_BUFFER_HEADER_SIZE - EXT2_SYMLINK_HEADER;

    /* an odd offset or length would split a character */
    if ((RDB->SymbolicLinkReparseBuffer.SubstituteNameOffset |
         RDB->SymbolicLinkReparseBuffer.SubstituteNameLength |
         RDB->SymbolicLinkReparseBuffer.PrintNameOffset |
         RDB->SymbolicLinkReparseBuffer.PrintNameLength) & (sizeof(WCHAR) - 1)) {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    if ((ULONG)RDB->SymbolicLinkReparseBuffer.SubstituteNameOffset +
               RDB->SymbolicLinkReparseBuffer.SubstituteNameLength > Names ||
        (ULONG)RDB->SymbolicLinkReparseBuffer.PrintNameOffset +
               RDB->SymbolicLinkReparseBuffer.PrintNameLength > Names) {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    if (RDB->SymbolicLinkReparseBuffer.SubstituteNameLength == 0) {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    /* relative links map 1:1; absolute ones are accepted when they point
       into this volume (Ext2AbsoluteLinkToVolumePath) */
    if (RDB->SymbolicLinkReparseBuffer.Flags != SYMLINK_FLAG_RELATIVE &&
        RDB->SymbolicLinkReparseBuffer.Flags != 0) {
        return STATUS_NOT_IMPLEMENTED;
    }

    return STATUS_SUCCESS;
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
    Name->Buffer[0] = (WCHAR)(Vcb->DrvLetter & EXT2_LETTER_MASK);
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

/* symlink reparse header: print name first, substitute name after it,
   both lengths in bytes (as the REPARSE_DATA_BUFFER contract wants) */
static VOID
Ext2InitializeReparseData(IN PREPARSE_DATA_BUFFER RDB, USHORT PrintLength,
                          USHORT SubstLength, ULONG Flags)
{
    RDB->ReparseTag = IO_REPARSE_TAG_SYMLINK;
    RDB->ReparseDataLength = (USHORT)(EXT2_SYMLINK_HEADER - REPARSE_DATA_BUFFER_HEADER_SIZE +
                                      PrintLength + SubstLength);
    RDB->Reserved = 0;
    RDB->SymbolicLinkReparseBuffer.PrintNameOffset = 0;
    RDB->SymbolicLinkReparseBuffer.PrintNameLength = PrintLength;
    RDB->SymbolicLinkReparseBuffer.SubstituteNameOffset = PrintLength;
    RDB->SymbolicLinkReparseBuffer.SubstituteNameLength = SubstLength;
    RDB->SymbolicLinkReparseBuffer.Flags = Flags;
    RtlZeroMemory(&RDB->SymbolicLinkReparseBuffer.PathBuffer, PrintLength + SubstLength);
}

/*
 * The target of symlink Mcb in the Windows form - '\' for every '/' - still
 * in the volume's code page, NUL-terminated, in a pool buffer the caller
 * frees (Ext2FreePool, 'NL2E'). Always a copy: a fast symlink keeps its
 * target in i_block, and the inode is written back as it is in memory. A
 * length Linux never writes - none, or not under a block - is corruption,
 * refused before any buffer is sized from it.
 */
NTSTATUS
Ext2ReadLinkText(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    OUT POEM_STRING         Text
)
{
    struct inode   *Inode = Mcb->Inode;
    ULONG           Length, i;
    NTSTATUS        Status;

    RtlZeroMemory(Text, sizeof(OEM_STRING));

    if (Inode->i_size == 0 || Inode->i_size >= BLOCK_SIZE || Inode->i_size >= MAXUSHORT) {
        return STATUS_FILE_CORRUPT_ERROR;
    }
    Length = (ULONG)Inode->i_size;

    Text->Buffer = Ext2AllocatePool(NonPagedPool, Length + 1, 'NL2E');
    if (Text->Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Text->Length = (USHORT)Length;
    Text->MaximumLength = (USHORT)(Length + 1);

    if (ext4_inode_is_fast_symlink(Inode)) {
        if (Length >= EXT2_LINKLEN_IN_INODE) {
            Status = STATUS_FILE_CORRUPT_ERROR;
        } else {
            RtlCopyMemory(Text->Buffer, Inode->i_block, Length);
            Status = STATUS_SUCCESS;
        }
    } else {
        Status = Ext2ReadInode(IrpContext, Vcb, Mcb, 0, Text->Buffer, Length, FALSE, NULL);
    }
    if (!NT_SUCCESS(Status)) {
        Ext2FreePool(Text->Buffer, 'NL2E');
        RtlZeroMemory(Text, sizeof(OEM_STRING));
        return Status;
    }

    Text->Buffer[Length] = '\0';
    for (i = 0; i < Length; i++) {
        if (Text->Buffer[i] == '/') {
            Text->Buffer[i] = '\\';
        }
    }
    return STATUS_SUCCESS;
}

/*
 * The handle a reparse request came on: a file or a directory of a mounted
 * volume. The control device and a volume handle have no inode behind them;
 * a request on them is refused here, before anything is read through them.
 */
static NTSTATUS
Ext2ReparseHandle(IN PEXT2_IRP_CONTEXT IrpContext, OUT PEXT2_VCB *Vcb)
{
    PEXT2_FCB   Fcb = IrpContext->Fcb;

    if (IsExt2FsDevice(IrpContext->DeviceObject)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    *Vcb = (PEXT2_VCB)IrpContext->DeviceObject->DeviceExtension;
    if (!IsMounted(*Vcb) || IsFlagOn((*Vcb)->Flags, VCB_DISMOUNT_PENDING)) {
        return STATUS_VOLUME_DISMOUNTED;
    }

    /* a volume handle carries the Vcb, whose header has the same layout */
    if (Fcb == NULL || Fcb->Identifier.Type != EXT2FCB || IrpContext->Ccb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (IsInodeDeleted(Fcb)) {
        return STATUS_FILE_DELETED;
    }
    return STATUS_SUCCESS;
}

/*
 * May this handle change what its file is? Asked under the exclusive volume
 * resource. As NTFS asks (MS-FSA): a volume that is written to, a handle
 * granted write access to the data or to the attributes. As Linux asks:
 * the caller may write the inode (its mode bits), and neither the file nor
 * its directory is sealed (chattr +i / +a) - the directory entry changes as
 * well. The root never becomes a link.
 */
static NTSTATUS
Ext2ReparseMayChange(IN PEXT2_VCB Vcb, IN PEXT2_FCB Fcb, IN PEXT2_CCB Ccb)
{
    PEXT2_MCB   Mcb = Fcb->Mcb;

    if (!IsMounted(Vcb) || IsFlagOn(Vcb->Flags, VCB_DISMOUNT_PENDING)) {
        return STATUS_VOLUME_DISMOUNTED;
    }
    if (IsVcbReadOnly(Vcb)) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    if (IsFlagOn(Vcb->Flags, VCB_VOLUME_LOCKED)) {
        return STATUS_ACCESS_DENIED;
    }
    if (!BooleanFlagOn(Ccb->Access, FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | MAXIMUM_ALLOWED)) {
        return STATUS_ACCESS_DENIED;
    }
    if (Mcb->Parent == NULL || Mcb->Inode->i_ino == EXT2_ROOT_INO) {
        return STATUS_ACCESS_DENIED;
    }
    if (!Ext2CheckFileAccess(Vcb, Mcb, Ext2FileCanWrite) || Ext4IsSealed(Mcb->Inode) ||
        (Mcb->Parent->Inode && Ext4IsSealed(Mcb->Parent->Inode))) {
        return STATUS_ACCESS_DENIED;
    }
    return STATUS_SUCCESS;
}

/*
 * What the inode may be before it becomes a link. ext4 keeps the target
 * where a file keeps its data, and the type of a name in its directory
 * entry: a link takes the place of another link, of an empty directory -
 * mklink /D makes one first - or of an empty file with one name, since the
 * entries of its other names would go on saying "file". Data is never
 * thrown away for a link, nor the entries of a directory.
 */
static NTSTATUS
Ext2ReparseMayBecomeLink(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb)
{
    struct inode *Inode = Mcb->Inode;

    if (S_ISLNK(Inode->i_mode)) {
        return STATUS_SUCCESS;
    }
    if (S_ISDIR(Inode->i_mode)) {
        return Ext2IsDirectoryEmpty(IrpContext, Vcb, Mcb) ? STATUS_SUCCESS :
                                                            STATUS_DIRECTORY_NOT_EMPTY;
    }
    if (!S_ISREG(Inode->i_mode) || Inode->i_nlink > 1 || Inode->i_size != 0) {
        return STATUS_NOT_SUPPORTED;
    }
    return STATUS_SUCCESS;
}

/*
 * The target as ext4 keeps it, from the substitute name: in the volume's
 * code page, '/' for '\', an absolute target made a path from this
 * volume's root ("\??\D:\dir" is "/dir"). A NUL inside is refused - Linux
 * would read the link to it, e2fsck calls such a link invalid - and so is a
 * target that fills a block. The caller frees Target->Buffer.
 */
static NTSTATUS
Ext2ReparseTarget(IN PEXT2_VCB Vcb, IN PREPARSE_DATA_BUFFER RDB, OUT POEM_STRING Target)
{
    UNICODE_STRING  Name;
    ULONG           Length;
    USHORT          i;
    NTSTATUS        Status;

    RtlZeroMemory(Target, sizeof(OEM_STRING));

    Name.Length = RDB->SymbolicLinkReparseBuffer.SubstituteNameLength;
    Name.MaximumLength = Name.Length;
    Name.Buffer = (PWCHAR)((PUCHAR)RDB->SymbolicLinkReparseBuffer.PathBuffer +
                           RDB->SymbolicLinkReparseBuffer.SubstituteNameOffset);

    if (RDB->SymbolicLinkReparseBuffer.Flags != SYMLINK_FLAG_RELATIVE) {
        Status = Ext2AbsoluteLinkToVolumePath(Vcb, &Name);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    /* an estimate, a terminator possibly included */
    Length = Ext2UnicodeToOEMSize(Vcb, &Name);
    if (Length >= MAXUSHORT) {
        return STATUS_NAME_TOO_LONG;
    }
    Target->MaximumLength = (USHORT)(Length + 1);
    Target->Buffer = Ext2AllocatePool(PagedPool, Target->MaximumLength, 'NL2E');
    if (Target->Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* the converter sets the exact byte count: a link's i_size is the
       length of its text, or e2fsck declares it invalid */
    Status = Ext2UnicodeToOEM(Vcb, Target, &Name);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    if (Target->Length == 0 || Target->Length >= BLOCK_SIZE) {
        return STATUS_NAME_TOO_LONG;
    }

    Target->Buffer[Target->Length] = '\0';
    for (i = 0; i < Target->Length; i++) {
        if (Target->Buffer[i] == '\0') {
            return STATUS_OBJECT_NAME_INVALID;
        }
        if (Target->Buffer[i] == '\\') {
            Target->Buffer[i] = '/';
        }
    }
    return STATUS_SUCCESS;
}

/*
 * FSCTL_GET_REPARSE_POINT: the link as Windows reads it. A relative target
 * goes as it is; a Linux absolute path ("/etc/passwd") is on this volume,
 * so it becomes "\??\D:\etc\passwd" for the I/O manager and
 * "D:\etc\passwd" for people (the print name). As MS-FSA has it, a buffer
 * too small for the whole of it takes what fits - the header says the size
 * needed - with STATUS_BUFFER_OVERFLOW, and one too small for the header
 * takes nothing.
 */
NTSTATUS
Ext2GetReparsePoint (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PIRP                    Irp = IrpContext->Irp;
    PIO_STACK_LOCATION      IrpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG                   OutputLength = IrpSp->Parameters.FileSystemControl.OutputBufferLength;
    PEXT2_VCB               Vcb = NULL;
    PEXT2_FCB               Fcb = IrpContext->Fcb;
    PREPARSE_DATA_BUFFER    Reparse = NULL;
    OEM_STRING              Text = { 0 };
    UNICODE_STRING          Dos = { 0 };
    UNICODE_STRING          Name;
    ULONG                   NameLength, PrintLength, SubstLength, Needed;
    BOOLEAN                 FcbAcquired = FALSE;
    BOOLEAN                 Absolute;
    PUCHAR                  Print, Subst;
    NTSTATUS                Status = STATUS_UNSUCCESSFUL;

    __try {

        Status = Ext2ReparseHandle(IrpContext, &Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        if (!IsInodeSymLink(Fcb->Inode) ||
            !IsFlagOn(IrpContext->Ccb->Flags, CCB_OPEN_REPARSE_POINT)) {
            Status = STATUS_NOT_A_REPARSE_POINT;
            __leave;
        }
        if (Irp->AssociatedIrp.SystemBuffer == NULL ||
            OutputLength < REPARSE_DATA_BUFFER_HEADER_SIZE) {
            Status = STATUS_BUFFER_TOO_SMALL;
            __leave;
        }

        /* the text under the Fcb: a new target is written under it */
        ExAcquireResourceSharedLite(&Fcb->MainResource, TRUE);
        FcbAcquired = TRUE;
        Status = Ext2ReadLinkText(IrpContext, Vcb, Fcb->Mcb, &Text);
        ExReleaseResourceLite(&Fcb->MainResource);
        FcbAcquired = FALSE;
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Absolute = (Text.Buffer[0] == '\\');
        if (Absolute) {
            Status = Ext2VolumeDosName(Vcb, &Dos);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
        }

        NameLength = Ext2OEMToUnicodeSize(Vcb, &Text);       /* bytes */
        if (NameLength == 0 || NameLength > MAXUSHORT) {
            Status = STATUS_UNMAPPABLE_CHARACTER;
            __leave;
        }
        PrintLength = Dos.Length + NameLength;
        SubstLength = PrintLength + (Absolute ? EXT2_NT_PREFIX_BYTES : 0);
        Needed = EXT2_SYMLINK_HEADER + PrintLength + SubstLength;
        if (Needed - REPARSE_DATA_BUFFER_HEADER_SIZE > MAXUSHORT) {
            Status = STATUS_NAME_TOO_LONG;
            __leave;
        }

        Reparse = Ext2AllocatePool(PagedPool, Needed, 'NL2E');
        if (Reparse == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }
        Ext2InitializeReparseData(Reparse, (USHORT)PrintLength, (USHORT)SubstLength,
                                  Absolute ? 0 : SYMLINK_FLAG_RELATIVE);
        Print = (PUCHAR)Reparse->SymbolicLinkReparseBuffer.PathBuffer;
        Subst = Print + PrintLength;

        /* the print name: "D:" before an absolute target, then the text */
        if (Absolute) {
            RtlCopyMemory(Print, Dos.Buffer, Dos.Length);
        }
        /* RtlOemStringToUnicodeString, the converter for a text the code
           page cannot map, wants room for a terminator: it lands on the
           substitute name, written next */
        Name.Buffer = (PWCHAR)(Print + Dos.Length);
        Name.Length = 0;
        Name.MaximumLength = (USHORT)(NameLength + sizeof(WCHAR));
        Status = Ext2OEMToUnicode(Vcb, &Name, &Text);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        if (Name.Length != NameLength) {
            Status = STATUS_UNMAPPABLE_CHARACTER;
            __leave;
        }

        /* the substitute name: the same, "\??\" before an absolute one */
        if (Absolute) {
            RtlCopyMemory(Subst, Ext2NtPrefix, EXT2_NT_PREFIX_BYTES);
            Subst += EXT2_NT_PREFIX_BYTES;
        }
        RtlCopyMemory(Subst, Print, PrintLength);

        Irp->IoStatus.Information = min(Needed, OutputLength);
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Reparse, Irp->IoStatus.Information);
        Status = (OutputLength < Needed) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;

    } __finally {

        if (FcbAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }
        if (Reparse) {
            Ext2FreePool(Reparse, 'NL2E');
        }
        if (Text.Buffer) {
            Ext2FreePool(Text.Buffer, 'NL2E');
        }
        if (Dos.Buffer) {
            ExFreePool(Dos.Buffer);
        }

        if (!AbnormalTermination()) {
            Ext2CompleteIrpContext(IrpContext, Status);
        }
    }

    return Status;
}

/*
 * The inode becomes a symlink to Target: its blocks go (a directory, which
 * mklink /D makes first, also leaves the directory count and keeps one
 * link), the target is written, then the type - in the inode and in the
 * parent's entry. A failure leaves an inode that is neither one nor the
 * other: the caller stops the journal.
 */
static NTSTATUS
Ext2MakeSymlink(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_FCB            ParentDcb,
    IN PEXT2_MCB            Mcb,
    IN PCHAR                Target,
    IN ULONG                Length
)
{
    LARGE_INTEGER   Zero = {0};
    ULONG           Written = 0;
    NTSTATUS        Status;

    Status = Ext2TruncateFile(IrpContext, Vcb, Mcb, &Zero);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    if (S_ISDIR(Mcb->Inode->i_mode)) {
        Status = Ext2UpdateGroupDirStat(IrpContext, Vcb,
                                        (Mcb->Inode->i_ino - 1) / INODES_PER_GROUP);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        /* its "." is gone, so exactly one link is left - set it, since
           ext3_dec_count deliberately never takes a directory below 2. The
           parent loses the ".." link in Ext2SetFileType below, which sees
           the mode change - not here, or it is taken twice */
        Mcb->Inode->i_nlink = 1;
    }
    if (!Ext2SaveInode(IrpContext, Vcb, Mcb->Inode)) {
        return STATUS_UNEXPECTED_IO_ERROR;
    }

    Status = Ext2WriteSymlink(IrpContext, Vcb, Mcb, Target, Length, &Written);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    return Ext2SetFileType(IrpContext, Vcb, ParentDcb, Mcb, S_IFLNK | S_IRWXUGO);
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
    PUCHAR   BlockData = NULL;

    if (BytesWritten)
        *BytesWritten = 0;

    /* A block-backed symlink occupies one block, including its terminator. */
    if (!Size || Size >= BLOCK_SIZE)
        return STATUS_NAME_TOO_LONG;

    if (Size >= EXT2_LINKLEN_IN_INODE) {
        ULONGLONG Block = 0;
        ULONG Mapped = 1;

        BlockData = Ext2AllocatePool(PagedPool, BLOCK_SIZE, 'NL4E');
        if (!BlockData) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto out;
        }
        RtlZeroMemory(BlockData, BLOCK_SIZE);
        RtlCopyMemory(BlockData, Buffer, Size);

        /* i_block holds no map yet (a short target, or nothing): an empty
           one, before the block is mapped */
        if (Ext2InodeHoldsNoData(Mcb->Inode)) {
            memset(Data, 0, EXT2_LINKLEN_IN_INODE);
            ClearFlag(Mcb->Inode->i_flags, EXT4_EXTENTS_FL);
            if (!Ext2SaveInode(IrpContext, Vcb, Mcb->Inode)) {
                Status = STATUS_UNEXPECTED_IO_ERROR;
                goto out;
            }
        }

        /* Allocate explicitly: the original inode may still be a directory.
           Write the whole zeroed block so reuse cannot leave a stale suffix. */
        Mcb->Inode->i_size = Size;
        Status = Ext2BlockMap(IrpContext, Vcb, Mcb, 0, TRUE, &Block, &Mapped);
        if (NT_SUCCESS(Status) && (!Block || Block >= TOTAL_BLOCKS || Mapped != 1))
            Status = STATUS_DISK_CORRUPT_ERROR;
        if (NT_SUCCESS(Status) &&
            !Ext2SaveBuffer(IrpContext, Vcb, (LONGLONG)(Block << BLOCK_BITS),
                            BLOCK_SIZE, BlockData))
            Status = STATUS_UNEXPECTED_IO_ERROR;
        if (!NT_SUCCESS(Status)) {
            LARGE_INTEGER Zero = {0};
            /* Remove a partially allocated target; preserve the write error
               (a release that fails has stopped the volume already). */
            (void)Ext2TruncateFile(IrpContext, Vcb, Mcb, &Zero);
            if (BytesWritten)
                *BytesWritten = 0;
            goto out;
        }

    } else {

        /* free inode blocks before writing in line */
        if (!Ext2InodeHoldsNoData(Mcb->Inode)) {
            LARGE_INTEGER Zero = {0, 0};
            Status = Ext2TruncateFile(IrpContext, Vcb, Mcb, &Zero);
            if (!NT_SUCCESS(Status))
                goto out;
        }

        ClearFlag(Mcb->Inode->i_flags, EXT4_EXTENTS_FL);
        memset(Data, 0, EXT2_LINKLEN_IN_INODE);
        RtlCopyMemory(Data, Buffer, Size);
    }

    Mcb->Inode->i_size = Size;
    if (!Ext2SaveInode(IrpContext, Vcb, Mcb->Inode)) {
        Status = STATUS_UNEXPECTED_IO_ERROR;
        if (BytesWritten)
            *BytesWritten = 0;
        goto out;
    }

    if (BytesWritten) {
        *BytesWritten = Size;
    }

out:
    if (BlockData)
        Ext2FreePool(BlockData, 'NL4E');
    return Status;
}

/*
 * FSCTL_SET_REPARSE_POINT: the file becomes a symlink, or a link gets a new
 * target. Turning a file into a link changes what its name resolves to - a
 * namespace change, made under the exclusive volume resource like create,
 * delete and rename (order: Vcb -> FcbLock -> Fcb), so that no open
 * resolves the name halfway through. Never posted: the privilege is the
 * caller's own, checked in its thread.
 */
NTSTATUS
Ext2SetReparsePoint (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PIRP                Irp = IrpContext->Irp;
    PIO_STACK_LOCATION  IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PREPARSE_DATA_BUFFER RDB = Irp->AssociatedIrp.SystemBuffer;
    PEXT2_VCB           Vcb = NULL;
    PEXT2_FCB           Fcb = IrpContext->Fcb;
    PEXT2_MCB           Mcb = NULL;
    PEXT2_FCB           ParentDcb = NULL;
    OEM_STRING          Target = { 0 };
    BOOLEAN             VcbAcquired = FALSE;
    BOOLEAN             FcbAcquired = FALSE;
    NTSTATUS            Status = STATUS_UNSUCCESSFUL;

    __try {

        Status = Ext2ReparseHandle(IrpContext, &Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        Mcb = Fcb->Mcb;

        /* as on NTFS, a symlink needs SeCreateSymbolicLinkPrivilege (an
           elevated administrator holds it) */
        if (Irp->RequestorMode != KernelMode &&
            !SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_CREATE_SYMBOLIC_LINK_PRIVILEGE),
                                    UserMode)) {
            Status = STATUS_PRIVILEGE_NOT_HELD;
            __leave;
        }

        Status = Ext2InspectReparseData(RDB, IrpSp->Parameters.FileSystemControl.InputBufferLength);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* before any lock: an absolute target asks the mount manager for
           the letter of this volume */
        Status = Ext2ReparseTarget(Vcb, RDB, &Target);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);
        VcbAcquired = TRUE;

        Status = Ext2ReparseMayChange(Vcb, Fcb, IrpContext->Ccb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        Status = Ext2ReparseMayBecomeLink(IrpContext, Vcb, Mcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* the parent's entry takes the new type; no rename moves the name
           under the exclusive volume resource */
        ParentDcb = Ext2ReferDcb(Vcb, Mcb->Parent);
        if (ParentDcb == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        /* exclusive: the file data is truncated and rewritten below */
        ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
        FcbAcquired = TRUE;

        /* From the truncate on the inode is neither what it was nor yet a
           link: a directory without "." or a file whose i_block holds the
           link's text (read as a block map) is no valid inode. A failure
           past this point leaves such a one: the transaction must not
           commit. */
        Status = Ext2MakeSymlink(IrpContext, Vcb, ParentDcb, Mcb,
                                 Target.Buffer, Target.Length);
        if (!NT_SUCCESS(Status)) {
            Ext2JournalAbandon(Vcb, Status);
            __leave;
        }
        ClearFlag(Mcb->FileAttr, FILE_ATTRIBUTE_NORMAL);   /* NORMAL stands alone */
        SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_REPARSE_POINT);

        /* the in-memory Mcb was built for a regular file; make it the
           symlink it now is on disk (resolves the target, or marks it
           special when dangling), or the next open reads the link text as
           file data. A target that cannot be resolved leaves it special. */
        ClearLongFlag(Mcb->Flags, MCB_TYPE_SPECIAL);
        (void)Ext2FollowLink(IrpContext, Vcb, Mcb->Parent, Mcb, 0);

    } __finally {

        if (FcbAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }
        if (VcbAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }
        if (ParentDcb) {
            Ext2ReleaseFcb(ParentDcb);
        }
        if (Target.Buffer) {
            Ext2FreePool(Target.Buffer, 'NL2E');
        }

        if (NT_SUCCESS(Status)) {
            Ext2NotifyReportChange(IrpContext, Vcb, Mcb,
                                   FILE_NOTIFY_CHANGE_ATTRIBUTES,
                                   FILE_ACTION_MODIFIED);
        }

        if (!AbnormalTermination()) {
            Ext2CompleteIrpContext(IrpContext, Status);
        }
    }

    return Status;
}

/*
 * FSCTL_DELETE_REPARSE_POINT. On NTFS the file stays, without its reparse
 * point; an ext4 symlink is nothing but its target, so the link itself
 * goes - set to be deleted, as a disposition sets it, and removed when the
 * handle closes. Until then it is the link it was: nothing is written now,
 * and nothing half made is left behind if the removal never comes.
 */
NTSTATUS
Ext2DeleteReparsePoint (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PIRP                Irp = IrpContext->Irp;
    PIO_STACK_LOCATION  IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PREPARSE_DATA_BUFFER RDB = Irp->AssociatedIrp.SystemBuffer;
    PEXT2_VCB           Vcb = NULL;
    PEXT2_FCB           Fcb = IrpContext->Fcb;
    PEXT2_CCB           Ccb = IrpContext->Ccb;
    BOOLEAN             VcbAcquired = FALSE;
    BOOLEAN             FcbAcquired = FALSE;
    NTSTATUS            Status = STATUS_UNSUCCESSFUL;

    __try {

        Status = Ext2ReparseHandle(IrpContext, &Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* the request names the tag it removes, and carries no data */
        if (RDB == NULL ||
            IrpSp->Parameters.FileSystemControl.InputBufferLength < REPARSE_DATA_BUFFER_HEADER_SIZE ||
            RDB->ReparseDataLength != 0) {
            Status = STATUS_IO_REPARSE_DATA_INVALID;
            __leave;
        }

        ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);
        VcbAcquired = TRUE;

        Status = Ext2ReparseMayChange(Vcb, Fcb, Ccb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        if (!IsInodeSymLink(Fcb->Inode) || !IsFlagOn(Ccb->Flags, CCB_OPEN_REPARSE_POINT)) {
            Status = STATUS_NOT_A_REPARSE_POINT;
            __leave;
        }
        if (RDB->ReparseTag != IO_REPARSE_TAG_SYMLINK) {
            Status = STATUS_IO_REPARSE_TAG_MISMATCH;
            __leave;
        }

        ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
        FcbAcquired = TRUE;

        Status = Ext2SetDispositionInfo(IrpContext, Vcb, Fcb, Ccb, TRUE);

    } __finally {

        if (FcbAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }
        if (VcbAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }

        if (!AbnormalTermination()) {
            Ext2CompleteIrpContext(IrpContext, Status);
        }
    }

    return Status;
}
