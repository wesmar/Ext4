/**
 * SparseFiles.c - holes in files: the sparse mark and the requests that use
 * it, FSCTL_SET_SPARSE, FSCTL_QUERY_ALLOCATED_RANGES, FSCTL_SET_ZERO_DATA.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * ext4 has holes by nature: a block no extent maps reads as zeros and costs
 * nothing. A file is sparse when it was marked so, or when it lacks blocks
 * for its size - Linux makes such files without asking. Only extent-mapped
 * files can be: nothing here can cut a hole in an indirect map.
 *
 * The mark is the attribute system.sparse, which Linux neither lists nor
 * copies and e2fsck leaves alone. It is read with the inode when it lies in
 * the inode's body, so that listing a directory costs nothing; the rare one
 * that found no room there, in the attribute block, by the first open.
 *
 * What being sparse changes elsewhere:
 *  - growth allocates nothing but the blocks of the write that grows the
 *    file (FileWrite.c, FileSet.c): the gap stays a hole;
 *  - a cached write gets its blocks when it is made, as unwritten ones, not
 *    when the lazy writer gets to it, for the volume to be full at a time
 *    somebody can be told (FileWrite.c), and it never goes the fast way;
 *  - queries show what is really allocated (FileQuery.c, FastIo.c).
 */

#include "ext4fs.h"
#include <linux/ext4_xattr.h>

#define EXT4_SPARSE_NAME        "sparse"
#define EXT4_SPARSE_NAME_LEN    (sizeof(EXT4_SPARSE_NAME) - 1)

/* the most done under one hold of a file's locks */
#define EXT2_SPARSE_STEP        (64 * 1024 * 1024LL)

/* the buffers a step of Blocks blocks of a file can dirty at worst: a bitmap
   for each block, a group descriptor block for some, a leaf or two */
#define EXT2_STEP_BUFFERS(Blocks)   ((Blocks) + (Blocks) / 32 + 16)

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2SetSparse)
#pragma alloc_text(PAGE, Ext2QueryAllocatedRanges)
#pragma alloc_text(PAGE, Ext2SetZeroData)
#endif

/* ---------------------------------------------------------------- the state */

BOOLEAN
Ext2InodeHasHoles(IN struct inode *Inode)
{
    ULONGLONG BlockSize = Inode->i_sb->s_blocksize;
    ULONGLONG Size = ((ULONGLONG)Inode->i_size + BlockSize - 1) & ~(BlockSize - 1);

    /* i_blocks counts the extent nodes and the attribute block as well, so
       a hole smaller than those goes unseen: such a file is then only not
       shown as sparse, and is written as any other */
    return S_ISREG(Inode->i_mode) && (Inode->i_blocks << 9) < Size && !Ext4IsInline(Inode);
}

BOOLEAN
Ext2InodeIsSparse(IN struct inode *Inode)
{
    return S_ISREG(Inode->i_mode) && INODE_HAS_EXTENT(Inode) &&
           (Inode->i_sparse || Ext2InodeHasHoles(Inode));
}

ULONG
Ext2SparseAttributes(IN struct inode *Inode, IN ULONG Attributes)
{
    if (Ext2InodeIsSparse(Inode)) {
        ClearFlag(Attributes, FILE_ATTRIBUTE_NORMAL);   /* NORMAL stands alone */
        SetFlag(Attributes, FILE_ATTRIBUTE_SPARSE_FILE);
    }
    return Attributes;
}

LONGLONG
Ext2InodeAllocation(IN struct inode *Inode, IN LONGLONG Allocation)
{
    return Ext2InodeHasHoles(Inode) ? (LONGLONG)(Inode->i_blocks << 9) : Allocation;
}

/* ----------------------------------------------------------------- the mark */

BOOLEAN
Ext2BodyHasSparseMark(IN struct ext4_inode *Raw, IN ULONG Extra, IN ULONG InodeSize)
{
    PUCHAR  Entry = (PUCHAR)Raw + EXT4_GOOD_OLD_INODE_SIZE + Extra;
    PUCHAR  End = (PUCHAR)Raw + InodeSize;

    if (InodeSize <= EXT4_GOOD_OLD_INODE_SIZE ||
        Entry + sizeof(struct ext4_xattr_ibody_header) > End ||
        ((struct ext4_xattr_ibody_header *)Entry)->h_magic != EXT4_XATTR_MAGIC) {
        return FALSE;
    }
    Entry += sizeof(struct ext4_xattr_ibody_header);

    /* the disk's bytes: nothing past the inode is followed */
    while (Entry + sizeof(struct ext4_xattr_entry) <= End && !EXT4_XATTR_IS_LAST_ENTRY(Entry)) {
        struct ext4_xattr_entry *E = EXT4_XATTR_ENTRY(Entry);

        if (Entry + EXT4_XATTR_LEN(E->e_name_len) > End) {
            return FALSE;
        }
        if (E->e_name_index == EXT4_XATTR_INDEX_SYSTEM && E->e_name_len == EXT4_SPARSE_NAME_LEN &&
            RtlEqualMemory(EXT4_XATTR_NAME(E), EXT4_SPARSE_NAME, EXT4_SPARSE_NAME_LEN)) {
            return TRUE;
        }
        Entry += EXT4_XATTR_LEN(E->e_name_len);
    }
    return FALSE;
}

VOID
Ext2LoadSparseMark(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb)
{
    struct inode         *Inode = Mcb->Inode;
    struct ext4_xattr_ref Ref;

    if (Inode->i_sparse || Inode->i_file_acl == 0 || !S_ISREG(Inode->i_mode) || !INODE_HAS_EXTENT(Inode)) {
        return;
    }
    if (ext4_fs_get_xattr_ref(IrpContext, Vcb, Mcb, &Ref) == 0) {
        if (ext4_fs_get_xattr(&Ref, EXT4_XATTR_INDEX_SYSTEM, EXT4_SPARSE_NAME, EXT4_SPARSE_NAME_LEN,
                              NULL, 0, NULL) == 0) {
            Inode->i_sparse = 1;
        }
        (void)ext4_fs_put_xattr_ref(&Ref);      /* read only: nothing to write */
    }
}

static NTSTATUS
Ext2WriteSparseMark(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb, IN BOOLEAN Mark)
{
    struct ext4_xattr_ref Ref;
    UCHAR                 Value = 1;
    int                   rc;

    rc = ext4_fs_get_xattr_ref(IrpContext, Vcb, Mcb, &Ref);
    if (rc != 0) {
        return Ext2WinntError(rc);
    }
    rc = Mark ? ext4_fs_set_xattr(&Ref, EXT4_XATTR_INDEX_SYSTEM, EXT4_SPARSE_NAME, EXT4_SPARSE_NAME_LEN,
                                  &Value, sizeof(Value), FALSE) :
                ext4_fs_remove_xattr(&Ref, EXT4_XATTR_INDEX_SYSTEM, EXT4_SPARSE_NAME, EXT4_SPARSE_NAME_LEN);
    if (rc == 0) {
        Ref.dirty = TRUE;
        rc = ext4_fs_put_xattr_ref(&Ref);
    } else {
        Ref.dirty = FALSE;
        (void)ext4_fs_put_xattr_ref(&Ref);      /* a release: nothing written */
        if (rc == -EEXIST || rc == -ENOENT) {
            rc = 0;                             /* it was as it is to be */
        }
    }
    if (rc == 0) {
        Mcb->Inode->i_sparse = Mark ? 1 : 0;
    }
    return Ext2WinntError(rc);
}

NTSTATUS
Ext2ClearSparseMark(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb)
{
    return Mcb->Inode->i_sparse ? Ext2WriteSparseMark(IrpContext, Vcb, Mcb, FALSE) : STATUS_SUCCESS;
}

/* ------------------------------------------------------------- the requests */

/* the volume, file and handle of a request about a file's data */
static NTSTATUS
Ext2SparseTarget(IN PEXT2_IRP_CONTEXT IrpContext, OUT PEXT2_VCB *Vcb, OUT PEXT2_FCB *Fcb, OUT PEXT2_CCB *Ccb)
{
    PDEVICE_OBJECT  DeviceObject = IrpContext->DeviceObject;
    PFILE_OBJECT    FileObject = IrpContext->FileObject;

    if (IsExt2FsDevice(DeviceObject)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    *Vcb = (PEXT2_VCB)DeviceObject->DeviceExtension;
    ASSERT((*Vcb)->Identifier.Type == EXT2VCB && (*Vcb)->Identifier.Size == sizeof(EXT2_VCB));
    if (!IsMounted(*Vcb) || IsFlagOn((*Vcb)->Flags, VCB_DISMOUNT_PENDING)) {
        return STATUS_VOLUME_DISMOUNTED;
    }

    *Fcb = (PEXT2_FCB)FileObject->FsContext;
    *Ccb = (PEXT2_CCB)FileObject->FsContext2;
    if (*Fcb == NULL || (*Fcb)->Identifier.Type != EXT2FCB || *Ccb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!S_ISREG((*Fcb)->Inode->i_mode) || IsFlagOn((*Fcb)->Flags, FCB_PAGE_FILE)) {
        return STATUS_INVALID_PARAMETER;
    }
    return IsInodeDeleted(*Fcb) ? STATUS_FILE_DELETED : STATUS_SUCCESS;
}

/* may this handle's caller change the data of the file at all? */
static NTSTATUS
Ext2SparseCheckWrite(IN PEXT2_VCB Vcb, IN PEXT2_FCB Fcb)
{
    if (IsVcbReadOnly(Vcb)) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    if (Ext4IsSealed(Fcb->Inode) || !Ext2CheckFileAccess(Vcb, Fcb->Mcb, Ext2FileCanWrite)) {
        return STATUS_ACCESS_DENIED;
    }
    return STATUS_SUCCESS;
}

/* how far one step goes, in bytes */
static LONGLONG
Ext2SparseStep(IN PEXT2_VCB Vcb)
{
    ULONG Blocks = Ext2JournalStepBlocks(Vcb);

    return (LONGLONG)Blocks > (EXT2_SPARSE_STEP >> BLOCK_BITS) ? EXT2_SPARSE_STEP :
                                                                  (LONGLONG)Blocks << BLOCK_BITS;
}

/* Every hole of the file allocated, unwritten: what FSCTL_SET_SPARSE owes a
   file that stops being sparse. Failing with STATUS_DISK_FULL, the holes
   filled stay so: nothing asks for them back. */
static NTSTATUS
Ext2FillHoles(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PEXT2_FCB Fcb)
{
    ULONG       Blocks = (ULONG)(Ext2SparseStep(Vcb) >> BLOCK_BITS);
    ULONGLONG   Block = 0;
    NTSTATUS    Status = STATUS_SUCCESS;

    while (NT_SUCCESS(Status)) {

        ULONGLONG       End;
        LARGE_INTEGER   Reached;
        BOOLEAN         Main = FALSE, Paging = FALSE;

        if (PsIsThreadTerminating(PsGetCurrentThread())) {
            return STATUS_CANCELLED;
        }

        __try {
            ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
            Main = TRUE;

            End = ((ULONGLONG)Fcb->Header.FileSize.QuadPart + BLOCK_SIZE - 1) >> BLOCK_BITS;
            if (Block >= End) {
                End = 0;
                __leave;
            }
            End = min(End, Block + Blocks);

            Ext2JournalJoinRoom(Vcb, EXT2_STEP_BUFFERS(Blocks));
            ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
            Paging = TRUE;

            Status = Ext2ExpandExtent(IrpContext, Vcb, Fcb->Mcb, (ULONG)Block, (ULONG)End, &Reached);

        } __finally {
            if (Paging) {
                ExReleaseResourceLite(&Fcb->PagingIoResource);
            }
            if (Main) {
                ExReleaseResourceLite(&Fcb->MainResource);
            }
            Ext2JournalYield(Vcb);
        }

        if (End == 0) {
            break;
        }
        Block = End;
    }
    return Status;
}

NTSTATUS
Ext2SetSparse(IN PEXT2_IRP_CONTEXT IrpContext)
{
    PIRP                        Irp = IrpContext->Irp;
    PEXTENDED_IO_STACK_LOCATION IrpSp = (PEXTENDED_IO_STACK_LOCATION)IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT                FileObject = IrpContext->FileObject;
    ULONG                       InputSize = IrpSp->Parameters.FileSystemControl.InputBufferLength;
    PEXT2_VCB                   Vcb = NULL;
    PEXT2_FCB                   Fcb = NULL;
    PEXT2_CCB                   Ccb = NULL;
    BOOLEAN                     Sparse = TRUE;
    BOOLEAN                     Main = FALSE, Paging = FALSE, Changed = FALSE;
    NTSTATUS                    Status = STATUS_SUCCESS;

    __try {

        Status = Ext2SparseTarget(IrpContext, &Vcb, &Fcb, &Ccb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        if (IsVcbReadOnly(Vcb)) {
            Status = STATUS_MEDIA_WRITE_PROTECTED;
            __leave;
        }
        if (!BooleanFlagOn(Ccb->Access, FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | MAXIMUM_ALLOWED)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }
        Status = Ext2SparseCheckWrite(Vcb, Fcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* no buffer at all asks for sparse */
        if (InputSize >= sizeof(FILE_SET_SPARSE_BUFFER)) {
            Sparse = ((PFILE_SET_SPARSE_BUFFER)Irp->AssociatedIrp.SystemBuffer)->SetSparse != FALSE;
        }

        if (!IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT)) {
            Status = STATUS_PENDING;
            __leave;
        }

        if (!Sparse && Ext2InodeIsSparse(Fcb->Inode)) {
            /* blocks only a commit can free are waited for here, before any
               lock (ext4\FreedBlocks.c); what is read is a hint */
            if (IrpContext->IsTopLevel) {
                Ext2WaitForFreedBlocks(Vcb, Ext2InodeAllocation(Fcb->Inode, Fcb->Header.AllocationSize.QuadPart),
                                       Fcb->Header.FileSize.QuadPart);
            }
            Status = Ext2FillHoles(IrpContext, Vcb, Fcb);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
        }

        ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
        Main = TRUE;

        if (IsInodeDeleted(Fcb)) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        /* a file Linux keeps in its inode goes to a block and a tree first */
        if (Sparse && Ext4IsInline(Fcb->Inode)) {
            ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
            Paging = TRUE;
            Status = Ext4UninlineFile(IrpContext, Vcb, Fcb->Mcb, TRUE);
            ExReleaseResourceLite(&Fcb->PagingIoResource);
            Paging = FALSE;
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
        }
        if (Sparse && !INODE_HAS_EXTENT(Fcb->Inode)) {
            Status = STATUS_INVALID_DEVICE_REQUEST;     /* an indirect map has no way to punch */
            __leave;
        }

        if (Sparse != (Fcb->Inode->i_sparse != 0)) {
            Status = Ext2WriteSparseMark(IrpContext, Vcb, Fcb->Mcb, Sparse);
            Changed = NT_SUCCESS(Status);
        }

    } __finally {

        if (Paging) {
            ExReleaseResourceLite(&Fcb->PagingIoResource);
        }
        if (Main) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (!AbnormalTermination()) {
            if (Changed) {
                SetFlag(FileObject->Flags, FO_FILE_MODIFIED);
                Ext2NotifyReportChange(IrpContext, Vcb, Fcb->Mcb, FILE_NOTIFY_CHANGE_ATTRIBUTES,
                                       FILE_ACTION_MODIFIED);
            }
            if (Status == STATUS_PENDING) {
                Status = Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }

    return Status;
}

/* one range into the caller's buffer, in its own __try: the buffer is a user
   address (METHOD_NEITHER) another thread may unmap at any moment */
static NTSTATUS
Ext2PutRange(IN PFILE_ALLOCATED_RANGE_BUFFER Out, IN ULONG OutputSize, IN OUT PULONG Count,
             IN LONGLONG Offset, IN LONGLONG Length)
{
    if ((*Count + 1) * sizeof(FILE_ALLOCATED_RANGE_BUFFER) > OutputSize) {
        return STATUS_BUFFER_OVERFLOW;
    }
    __try {
        Out[*Count].FileOffset.QuadPart = Offset;
        Out[*Count].Length.QuadPart = Length;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return STATUS_INVALID_USER_BUFFER;
    }
    (*Count)++;
    return STATUS_SUCCESS;
}

/* the written runs of [Start, End) of a sparse file, adjacent ones as one:
   an unwritten extent reads as zeros and is a hole to whoever asks */
static NTSTATUS
Ext2WalkRanges(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PEXT2_FCB Fcb,
               IN LONGLONG Start, IN LONGLONG End, IN PFILE_ALLOCATED_RANGE_BUFFER Out,
               IN ULONG OutputSize, IN OUT PULONG Count)
{
    ULONGLONG   Block = (ULONGLONG)Start >> BLOCK_BITS;
    LONGLONG    RunStart = 0, RunEnd = 0;
    NTSTATUS    Status = STATUS_SUCCESS;

    while ((LONGLONG)(Block << BLOCK_BITS) < End) {

        ULONG       Kind;
        ULONGLONG   Next;

        Status = Ext2ExtentRun(IrpContext, Fcb->Mcb, (ULONG)Block, &Kind, &Next);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }

        if (Kind == EXT2_RUN_WRITTEN) {
            LONGLONG From = max((LONGLONG)(Block << BLOCK_BITS), Start);
            LONGLONG To = min((LONGLONG)(Next << BLOCK_BITS), End);

            if (RunEnd != From) {
                if (RunEnd > RunStart) {
                    Status = Ext2PutRange(Out, OutputSize, Count, RunStart, RunEnd - RunStart);
                    if (Status != STATUS_SUCCESS) {
                        return Status;
                    }
                }
                RunStart = From;
            }
            RunEnd = To;
        }
        Block = Next;
    }

    if (RunEnd > RunStart) {
        Status = Ext2PutRange(Out, OutputSize, Count, RunStart, RunEnd - RunStart);
    }
    return Status;
}

NTSTATUS
Ext2QueryAllocatedRanges(IN PEXT2_IRP_CONTEXT IrpContext)
{
    PIRP                         Irp = IrpContext->Irp;
    PEXTENDED_IO_STACK_LOCATION  IrpSp = (PEXTENDED_IO_STACK_LOCATION)IoGetCurrentIrpStackLocation(Irp);
    ULONG                        InputSize = IrpSp->Parameters.FileSystemControl.InputBufferLength;
    ULONG                        OutputSize = IrpSp->Parameters.FileSystemControl.OutputBufferLength;
    PFILE_ALLOCATED_RANGE_BUFFER Input = IrpSp->Parameters.FileSystemControl.Type3InputBuffer;
    PFILE_ALLOCATED_RANGE_BUFFER Out = NULL;
    FILE_ALLOCATED_RANGE_BUFFER  In = { 0 };
    PEXT2_VCB                    Vcb = NULL;
    PEXT2_FCB                    Fcb = NULL;
    PEXT2_CCB                    Ccb = NULL;
    LONGLONG                     Start, End;
    ULONG                        Count = 0;
    BOOLEAN                      Main = FALSE;
    NTSTATUS                     Status = STATUS_SUCCESS;

    __try {

        Status = Ext2SparseTarget(IrpContext, &Vcb, &Fcb, &Ccb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        if (InputSize < sizeof(FILE_ALLOCATED_RANGE_BUFFER)) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }
        if (Input == NULL) {
            Status = STATUS_INVALID_USER_BUFFER;
            __leave;
        }

        /* Never posted: the buffers are the caller's own addresses
           (METHOD_NEITHER), valid in its process only. */
        SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);

        __try {
            if (Irp->RequestorMode != KernelMode) {
                ProbeForRead(Input, InputSize, sizeof(UCHAR));
            }
            In = *Input;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Status = STATUS_INVALID_USER_BUFFER;
        }
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (In.FileOffset.QuadPart < 0 || In.Length.QuadPart < 0 ||
            In.Length.QuadPart > MAXLONGLONG - In.FileOffset.QuadPart) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }
        if (In.Length.QuadPart == 0) {
            __leave;
        }
        if (OutputSize < sizeof(FILE_ALLOCATED_RANGE_BUFFER)) {
            Status = STATUS_BUFFER_TOO_SMALL;
            __leave;
        }

        Out = (PFILE_ALLOCATED_RANGE_BUFFER)Ext2GetUserBuffer(Irp);
        if (Out == NULL) {
            Status = STATUS_INVALID_USER_BUFFER;
            __leave;
        }
        __try {
            if (Irp->RequestorMode != KernelMode) {
                ProbeForWrite(Out, OutputSize, sizeof(UCHAR));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Status = STATUS_INVALID_USER_BUFFER;
        }
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* Exclusive: a flush takes this very resource for its paging writes
           (Ext2AcquireFileForCcFlush), and holding it shared would wait for
           itself. It keeps the tree still as well: whatever changes a tree
           holds it exclusively. */
        ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
        Main = TRUE;

        Start = In.FileOffset.QuadPart;
        End = min(Start + In.Length.QuadPart, Fcb->Header.FileSize.QuadPart);
        if (Start >= End) {
            __leave;
        }

        if (!Ext2InodeIsSparse(Fcb->Inode)) {
            Status = Ext2PutRange(Out, OutputSize, &Count, Start, End - Start);
            __leave;
        }

        /* the map shows what reached the disk: what a cached write left in
           the cache, on unwritten blocks or (a mapped view) on none, first */
        if (Fcb->SectionObject.DataSectionObject != NULL && !IsVcbReadOnly(Vcb)) {
            IO_STATUS_BLOCK Iosb;

            CcFlushCache(&Fcb->SectionObject, NULL, 0, &Iosb);
            if (!NT_SUCCESS(Iosb.Status)) {
                Status = Iosb.Status;
                __leave;
            }
        }

        Status = Ext2WalkRanges(IrpContext, Vcb, Fcb, Start, End, Out, OutputSize, &Count);

    } __finally {

        if (Main) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (!AbnormalTermination()) {
            if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW) {
                Irp->IoStatus.Information = Count * sizeof(FILE_ALLOCATED_RANGE_BUFFER);
            }
            Ext2CompleteIrpContext(IrpContext, Status);
        }
    }

    return Status;
}

/* zeros over [Start, End), inside one block, if the block has anything to
   clear: a hole or an unwritten block reads as zeros, and zeroing it through
   the cache would only allocate it */
static NTSTATUS
Ext2ZeroPartial(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PFILE_OBJECT FileObject,
                IN PEXT2_FCB Fcb, IN LONGLONG Start, IN LONGLONG End, OUT PBOOLEAN Zeroed)
{
    LARGE_INTEGER   From, To;
    ULONG           Kind;
    ULONGLONG       Next;
    NTSTATUS        Status;

    Status = Ext2ExtentRun(IrpContext, Fcb->Mcb, (ULONG)(Start >> BLOCK_BITS), &Kind, &Next);
    if (!NT_SUCCESS(Status) || Kind != EXT2_RUN_WRITTEN) {
        return Status;
    }
    From.QuadPart = Start;
    To.QuadPart = End;
    *Zeroed = TRUE;
    return Ext2OverwriteZeros(IrpContext, FileObject, &From, &To);
}

/*
 * [Start, Stop) of a sparse file, Stop at most the end of file, MainResource
 * held exclusively: the blocks wholly inside go, the parts of a block at
 * either end are zeroed. The last block of the file goes whole when the range
 * reaches the end of file, its tail past it never holding data.
 *
 * A view of the file mapped by somebody cannot be purged, and an extent
 * across the range may have no node to split into: the bytes then are zeroed
 * through the cache, which is all the request promises.
 */
static NTSTATUS
Ext2PunchStep(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PFILE_OBJECT FileObject,
              IN PEXT2_FCB Fcb, IN LONGLONG Start, IN LONGLONG Stop)
{
    PSECTION_OBJECT_POINTERS Section = Fcb->SectionObject.DataSectionObject ? &Fcb->SectionObject : NULL;
    LONGLONG        First = CEILING_ALIGNED(LONGLONG, Start, (LONGLONG)BLOCK_SIZE);
    LONGLONG        Last = Stop == Fcb->Header.FileSize.QuadPart ?
                           CEILING_ALIGNED(LONGLONG, Stop, (LONGLONG)BLOCK_SIZE) :
                           Stop & ~((LONGLONG)BLOCK_SIZE - 1);
    LARGE_INTEGER   Pages;
    ULONG           PageBytes;
    IO_STATUS_BLOCK Iosb;
    BOOLEAN         Zeroed = FALSE, Paging = FALSE;
    NTSTATUS        Status = STATUS_SUCCESS;

    /* whole pages around the range: a block smaller than a page shares its
       page with blocks that stay. The pages are written first, so that a
       purge drops nothing but copies, and the map tells the truth. */
    Pages.QuadPart = Start & ~((LONGLONG)PAGE_SIZE - 1);
    PageBytes = (ULONG)(CEILING_ALIGNED(LONGLONG, max(Stop, Last), (LONGLONG)PAGE_SIZE) - Pages.QuadPart);

    if (Section != NULL) {
        CcFlushCache(Section, &Pages, PageBytes, &Iosb);
        if (!NT_SUCCESS(Iosb.Status)) {
            return Iosb.Status;
        }
    }

    /* the range inside one block */
    if (First > Last) {
        return Ext2ZeroPartial(IrpContext, Vcb, FileObject, Fcb, Start, Stop, &Zeroed);
    }
    if (Start < First) {
        Status = Ext2ZeroPartial(IrpContext, Vcb, FileObject, Fcb, Start, First, &Zeroed);
    }
    if (NT_SUCCESS(Status) && Last < Stop) {
        Status = Ext2ZeroPartial(IrpContext, Vcb, FileObject, Fcb, Last, Stop, &Zeroed);
    }
    if (!NT_SUCCESS(Status) || First == Last) {
        return Status;
    }

    if (Section == NULL && Fcb->SectionObject.DataSectionObject != NULL) {
        Section = &Fcb->SectionObject;      /* made by the zeroing just now */
    }
    if (Zeroed && Section != NULL) {
        CcFlushCache(Section, &Pages, PageBytes, &Iosb);
        if (!NT_SUCCESS(Iosb.Status)) {
            return Iosb.Status;
        }
    }

    Ext2JournalJoinRoom(Vcb, EXT2_STEP_BUFFERS((ULONG)((Last - First) >> BLOCK_BITS)));

    __try {
        ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
        Paging = TRUE;

        if (Section != NULL && !CcPurgeCacheSection(Section, &Pages, PageBytes, FALSE)) {
            Status = STATUS_USER_MAPPED_FILE;
        } else {
            Status = Ext2PunchExtent(IrpContext, Vcb, Fcb->Mcb, (ULONG)(First >> BLOCK_BITS),
                                     (ULONGLONG)Last >> BLOCK_BITS);
        }
    } __finally {
        if (Paging) {
            ExReleaseResourceLite(&Fcb->PagingIoResource);
        }
    }

    if (Status == STATUS_USER_MAPPED_FILE || Status == STATUS_DISK_FULL) {
        LARGE_INTEGER From, To;

        From.QuadPart = First;
        To.QuadPart = min(Last, Stop);
        Status = Ext2OverwriteZeros(IrpContext, FileObject, &From, &To);
    }
    return Status;
}

NTSTATUS
Ext2SetZeroData(IN PEXT2_IRP_CONTEXT IrpContext)
{
    PIRP                        Irp = IrpContext->Irp;
    PEXTENDED_IO_STACK_LOCATION IrpSp = (PEXTENDED_IO_STACK_LOCATION)IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT                FileObject = IrpContext->FileObject;
    ULONG                       InputSize = IrpSp->Parameters.FileSystemControl.InputBufferLength;
    PFILE_ZERO_DATA_INFORMATION Zero = Irp->AssociatedIrp.SystemBuffer;
    PEXT2_VCB                   Vcb = NULL;
    PEXT2_FCB                   Fcb = NULL;
    PEXT2_CCB                   Ccb = NULL;
    LONGLONG                    Start, End, Step;
    BOOLEAN                     Main = FALSE, OpPostIrp = FALSE, Changed = FALSE, Flushed = FALSE;
    NTSTATUS                    Status = STATUS_SUCCESS;

    __try {

        Status = Ext2SparseTarget(IrpContext, &Vcb, &Fcb, &Ccb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        if (InputSize < sizeof(FILE_ZERO_DATA_INFORMATION) || Zero == NULL) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }
        Start = Zero->FileOffset.QuadPart;
        End = Zero->BeyondFinalZero.QuadPart;
        if (Start < 0 || End < 0 || Start > End) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }
        Status = Ext2SparseCheckWrite(Vcb, Fcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        if (!IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT)) {
            Status = STATUS_PENDING;
            __leave;
        }

        /* Step by step, the locks let go between steps: a range may be the
           whole of a very large file, and whoever asked may be gone before
           it is through. After an oplock break the request starts again from
           its offset: zeroing is the same done twice. */
        Step = Ext2SparseStep(Vcb);

        while (Start < End) {

            LARGE_INTEGER   Length;
            LONGLONG        Stop;

            if (PsIsThreadTerminating(PsGetCurrentThread())) {
                Status = STATUS_CANCELLED;
                __leave;
            }

            /* zeros through the cache are writes like any other: they wait
               for the cache to have room for them, with no lock held */
            if (!Ext2InodeIsSparse(Fcb->Inode) && FileObject->PrivateCacheMap != NULL) {
                CcCanIWrite(FileObject, (ULONG)Step, TRUE, FALSE);
            }

            ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
            Main = TRUE;

            if (IsInodeDeleted(Fcb)) {
                Status = STATUS_FILE_DELETED;
                __leave;
            }
            if (Start >= Fcb->Header.FileSize.QuadPart) {
                break;                      /* nothing to do past the end */
            }

            Status = FsRtlCheckOplock(&Fcb->Oplock, Irp, IrpContext, Ext2OplockComplete, Ext2LockIrp);
            if (Status != STATUS_SUCCESS) {
                OpPostIrp = TRUE;
                __leave;
            }
            Fcb->Header.IsFastIoPossible = Ext2IsFastIoPossible(Fcb);

            /* all that is left of the range: what a jump over a hole below
               passes is checked as well */
            Stop = min(End, Fcb->Header.FileSize.QuadPart);
            Length.QuadPart = Stop - Start;
            if (!FsRtlFastCheckLockForWrite(&Fcb->FileLockAnchor, (PLARGE_INTEGER)&Start, &Length, 0,
                                            FileObject, PsGetCurrentProcess())) {
                Status = STATUS_FILE_LOCK_CONFLICT;
                __leave;
            }

            /* A hole has nothing to zero, and it may be terabytes long: over
               it in one jump. Only what the cache holds could lie there, dirty
               pages of a mapped view, so that goes to the disk first, once. */
            if (Ext2InodeIsSparse(Fcb->Inode)) {
                ULONG       Kind;
                ULONGLONG   Next;

                if (!Flushed && Fcb->SectionObject.DataSectionObject != NULL) {
                    IO_STATUS_BLOCK Iosb;

                    CcFlushCache(&Fcb->SectionObject, NULL, 0, &Iosb);
                    if (!NT_SUCCESS(Iosb.Status)) {
                        Status = Iosb.Status;
                        __leave;
                    }
                }
                Flushed = TRUE;

                Status = Ext2ExtentRun(IrpContext, Fcb->Mcb, (ULONG)(Start >> BLOCK_BITS), &Kind, &Next);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
                if (Kind == EXT2_RUN_HOLE) {
                    ExReleaseResourceLite(&Fcb->MainResource);
                    Main = FALSE;
                    Start = (LONGLONG)(Next << BLOCK_BITS);
                    continue;
                }
            }

            Stop = min(Stop, (Start + Step) & ~((LONGLONG)BLOCK_SIZE - 1));
            if (Ext2InodeIsSparse(Fcb->Inode)) {
                Status = Ext2PunchStep(IrpContext, Vcb, FileObject, Fcb, Start, Stop);
            } else {
                LARGE_INTEGER From, To;

                From.QuadPart = Start;
                To.QuadPart = Stop;
                Status = Ext2OverwriteZeros(IrpContext, FileObject, &From, &To);
            }
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
            Changed = TRUE;

            ExReleaseResourceLite(&Fcb->MainResource);
            Main = FALSE;
            Ext2JournalYield(Vcb);

            Start = Stop;
        }

        /* a handle that writes through wants it on the disk when this
           returns; the commit, with no resource held */
        if (Changed && FlagOn(FileObject->Flags, FO_WRITE_THROUGH | FO_NO_INTERMEDIATE_BUFFERING)) {

            IO_STATUS_BLOCK Iosb;

            Iosb.Status = STATUS_SUCCESS;
            if (!Main) {
                ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
                Main = TRUE;
            }
            if (Fcb->SectionObject.DataSectionObject != NULL) {
                CcFlushCache(&Fcb->SectionObject, NULL, 0, &Iosb);
            }
            ExReleaseResourceLite(&Fcb->MainResource);
            Main = FALSE;

            Status = NT_SUCCESS(Iosb.Status) ? Ext2JournalCommit(Vcb, TRUE) : Iosb.Status;
        }

    } __finally {

        if (Main) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (!AbnormalTermination() && !OpPostIrp) {
            if (Changed) {
                SetFlag(FileObject->Flags, FO_FILE_MODIFIED);
            }
            if (Status == STATUS_PENDING) {
                Status = Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }

    return Status;
}
