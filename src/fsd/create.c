/**
 * create.c - IRP_MJ_CREATE entry point, volume opens, new inodes, supersede/overwrite.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/ext4_xattr.h>
#include "create_internal.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2CreateVolume)
#pragma alloc_text(PAGE, Ext2Create)
#pragma alloc_text(PAGE, Ext2CreateInode)
#pragma alloc_text(PAGE, Ext2SupersedeOrOverWriteFile)
#endif

NTSTATUS Ext2AddDotEntries(struct ext2_icb *icb, struct inode *dir,
                           struct inode *inode)
{
    struct ext3_dir_entry_2 * de;
    struct buffer_head * bh;
    struct ext4_dir_entry_tail *t;
    ext3_lblk_t block = 0;
    unsigned int blocksize = dir->i_sb->s_blocksize;
    int csum_size = 0;
    int rc = 0;

    if (ext4_has_metadata_csum(dir->i_sb))
        csum_size = sizeof(struct ext4_dir_entry_tail);

    bh = ext3_append(icb, inode, &block, &rc);
    if (!bh) {
        goto errorout;
    }

    de = (struct ext3_dir_entry_2 *) bh->b_data;
    de->inode = cpu_to_le32(inode->i_ino);
    de->name_len = 1;
    de->rec_len = cpu_to_le16(EXT3_DIR_REC_LEN(de->name_len));
    strcpy (de->name, ".");
    ext3_set_de_type(inode->i_sb, de, S_IFDIR);
    de = (struct ext3_dir_entry_2 *)
         ((char *) de + le16_to_cpu(de->rec_len));
    de->inode = cpu_to_le32(dir->i_ino);
    de->rec_len = cpu_to_le16(inode->i_sb->s_blocksize-EXT3_DIR_REC_LEN(1)-csum_size);
    de->name_len = 2;
    strcpy (de->name, "..");
    ext3_set_de_type(inode->i_sb, de, S_IFDIR);
    inode->i_nlink = 2;
    if (csum_size) {
        t = EXT4_DIRENT_TAIL(bh->b_data, blocksize);
        initialize_dirent_tail(t, blocksize);
    }
    ext4_dirent_csum_set(inode, (struct ext4_dir_entry *)bh->b_data);
    mark_buffer_dirty(bh);
    ext3_mark_inode_dirty(icb, inode);

errorout:
    if (bh)
        __brelse (bh);

    return Ext2WinntError(rc);
}

//
// Any call to this routine must have Fcb's MainResource and FcbLock acquired.
//

NTSTATUS
Ext2OverwriteEa(
	PEXT2_IRP_CONTEXT    IrpContext,
	PEXT2_VCB Vcb,
	PEXT2_FCB Fcb,
	PIO_STATUS_BLOCK Iosb
)
{
    PEXT2_MCB           Mcb = NULL;
    PIRP				  Irp;
    PIO_STACK_LOCATION  IrpSp;

    struct ext4_xattr_ref xattr_ref;
    BOOLEAN             XattrRefAcquired = FALSE;
    NTSTATUS            Status = STATUS_UNSUCCESSFUL;

    PFILE_FULL_EA_INFORMATION FullEa;
    PCHAR EaBuffer;
    ULONG EaBufferLength;

    __try {

        Irp = IrpContext->Irp;
        IrpSp = IoGetCurrentIrpStackLocation(Irp);
        Mcb = Fcb->Mcb;

        EaBuffer = Irp->AssociatedIrp.SystemBuffer;
        EaBufferLength = IrpSp->Parameters.Create.EaLength;

        if (!Mcb)
            __leave;

        //
        // Return peacefully if there is no EaBuffer provided.
        //
        if (!EaBuffer) {
            Status = STATUS_SUCCESS;
            __leave;
        }

		//
		// If the caller specifies an EaBuffer, but has no knowledge about Ea,
		// we reject the request.
		//
		if (EaBuffer != NULL &&
			FlagOn(IrpSp->Parameters.Create.Options, FILE_NO_EA_KNOWLEDGE)) {
			Status = STATUS_ACCESS_DENIED;
			__leave;
		}

        //
        // Check Ea Buffer validity.
        //
        Status = IoCheckEaBufferValidity((PFILE_FULL_EA_INFORMATION)EaBuffer,
                                          EaBufferLength, (PULONG)&Iosb->Information);
        if (!NT_SUCCESS(Status))
            __leave;

        Status = Ext2WinntError(ext4_fs_get_xattr_ref(IrpContext, Vcb, Fcb->Mcb, &xattr_ref));
        if (!NT_SUCCESS(Status)) {
            DbgPrint("ext4_fs_get_xattr_ref() failed!\n");
            __leave;
        }

        XattrRefAcquired = TRUE;

        //
        // The EAs of Windows are the user namespace of ext4, and that is
        // what the caller's set replaces. security.selinux, the POSIX ACLs
        // and trusted.* belong to Linux and stay: overwriting a file with
        // an EA buffer used to purge every namespace.
        //
        {
            struct rb_node *Node = rb_first(&xattr_ref.root);

            while (Node) {
                struct ext4_xattr_item *Item = container_of(Node, struct ext4_xattr_item, node);

                Node = rb_next(Node);
                if (Item->name_index == EXT4_XATTR_INDEX_USER) {
                    ext4_fs_remove_xattr(&xattr_ref, Item->name_index,
                                         Item->name, Item->name_len);
                }
            }
        }
        xattr_ref.dirty = TRUE;
        Status = STATUS_SUCCESS;

        // Iterate the whole EA buffer to do inspection
        for (FullEa = (PFILE_FULL_EA_INFORMATION)EaBuffer;
            FullEa < (PFILE_FULL_EA_INFORMATION)&EaBuffer[EaBufferLength];
            FullEa = (PFILE_FULL_EA_INFORMATION)(FullEa->NextEntryOffset == 0 ?
                &EaBuffer[EaBufferLength] :
                (PCHAR)FullEa + FullEa->NextEntryOffset)) {

            OEM_STRING EaName;

            EaName.MaximumLength = EaName.Length = FullEa->EaNameLength;
            EaName.Buffer = &FullEa->EaName[0];

            // Check if EA's name is valid
            if (!Ext2IsEaNameValid(EaName)) {
                Status = STATUS_INVALID_EA_NAME;
                __leave;
            }
        }

        // Now add EA entries to the inode
        for (FullEa = (PFILE_FULL_EA_INFORMATION)EaBuffer;
            FullEa < (PFILE_FULL_EA_INFORMATION)&EaBuffer[EaBufferLength];
            FullEa = (PFILE_FULL_EA_INFORMATION)(FullEa->NextEntryOffset == 0 ?
                &EaBuffer[EaBufferLength] :
                (PCHAR)FullEa + FullEa->NextEntryOffset)) {

            int ret;
            OEM_STRING EaName;

            EaName.MaximumLength = EaName.Length = FullEa->EaNameLength;
            EaName.Buffer = &FullEa->EaName[0];

            Status = Ext2WinntError(ret =
                ext4_fs_set_xattr(&xattr_ref,
                    EXT4_XATTR_INDEX_USER,
                    EaName.Buffer,
                    EaName.Length,
                    &FullEa->EaName[0] + FullEa->EaNameLength + 1,
                    FullEa->EaValueLength,
                    TRUE));
            if (!NT_SUCCESS(Status) && ret != -ENODATA)
                __leave;

            if (ret == -ENODATA) {
                Status = Ext2WinntError(
                    ext4_fs_set_xattr(&xattr_ref,
                        EXT4_XATTR_INDEX_USER,
                        EaName.Buffer,
                        EaName.Length,
                        &FullEa->EaName[0] + FullEa->EaNameLength + 1,
                        FullEa->EaValueLength,
                        FALSE));
                if (!NT_SUCCESS(Status))
                    __leave;

            }
        }
    }
    __finally {

        if (XattrRefAcquired) {
            if (!NT_SUCCESS(Status)) {
                xattr_ref.dirty = FALSE;
                ext4_fs_put_xattr_ref(&xattr_ref);
			} else {
				Status = Ext2WinntError(ext4_fs_put_xattr_ref(&xattr_ref));
			}
        }
    }
    return Status;
}

/* security.selinux: the name in its namespace, and the most taken over
   (a context is some 40 bytes: "unconfined_u:object_r:user_home_t:s0") */
#define EXT4_SELINUX_NAME       "selinux"
#define EXT4_SELINUX_MAX        256

/*
 * The SELinux label of a new inode. A Linux system that runs SELinux
 * (Fedora does) labels every inode it makes; one made here had no label,
 * was unlabeled_t there and refused to confined services until a
 * restorecon. The label of the directory it is made in is what the kernel
 * gives when the policy names no transition, and the nearest this side can
 * know. A directory without a label - a system without SELinux - passes
 * nothing on. Best effort: a label that cannot be set leaves the file as
 * it was before.
 */
VOID
Ext4InheritSecurityLabel(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Parent,
    IN PEXT2_MCB            Mcb
)
{
    struct ext4_xattr_ref   Ref;
    UCHAR                   Label[EXT4_SELINUX_MAX];
    size_t                  Length = 0;
    int                     rc;

    if (Parent == NULL || Mcb == NULL ||
        ext4_fs_get_xattr_ref(IrpContext, Vcb, Parent, &Ref) != 0) {
        return;
    }
    rc = ext4_fs_get_xattr(&Ref, EXT4_XATTR_INDEX_SECURITY, EXT4_SELINUX_NAME,
                           sizeof(EXT4_SELINUX_NAME) - 1, Label, sizeof(Label), &Length);
    ext4_fs_put_xattr_ref(&Ref);
    if (rc != 0 || Length == 0 || Length > sizeof(Label)) {
        return;
    }

    if (ext4_fs_get_xattr_ref(IrpContext, Vcb, Mcb, &Ref) != 0) {
        return;
    }
    rc = ext4_fs_set_xattr(&Ref, EXT4_XATTR_INDEX_SECURITY, EXT4_SELINUX_NAME,
                           sizeof(EXT4_SELINUX_NAME) - 1, Label, Length, FALSE);
    if (rc != 0) {
        Ref.dirty = FALSE;
    }
    ext4_fs_put_xattr_ref(&Ref);
}

NTSTATUS
Ext2CreateVolume(PEXT2_IRP_CONTEXT IrpContext, PEXT2_VCB Vcb)
{
    PIO_STACK_LOCATION  IrpSp;
    PIRP                Irp;
    PEXT2_CCB           Ccb;

    NTSTATUS            Status;

    ACCESS_MASK         DesiredAccess;
    ULONG               ShareAccess;

    ULONG               Options;
    BOOLEAN             DirectoryFile;
    BOOLEAN             OpenTargetDirectory;

    ULONG               CreateDisposition;

    Irp = IrpContext->Irp;
    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    Options  = IrpSp->Parameters.Create.Options;

    DirectoryFile = IsFlagOn(Options, FILE_DIRECTORY_FILE);
    OpenTargetDirectory = IsFlagOn(IrpSp->Flags, SL_OPEN_TARGET_DIRECTORY);

    CreateDisposition = (Options >> 24) & 0x000000ff;

    DesiredAccess = IrpSp->Parameters.Create.SecurityContext->DesiredAccess;
    ShareAccess   = IrpSp->Parameters.Create.ShareAccess;

    if (DirectoryFile) {
        return STATUS_NOT_A_DIRECTORY;
    }

    if (OpenTargetDirectory) {
        DbgBreak();
        return STATUS_INVALID_PARAMETER;
    }

    if ( (CreateDisposition != FILE_OPEN) &&
            (CreateDisposition != FILE_OPEN_IF) ) {
        return STATUS_ACCESS_DENIED;
    }

    if ( !FlagOn(ShareAccess, FILE_SHARE_READ) &&
            Vcb->OpenVolumeCount  != 0 ) {
        return STATUS_SHARING_VIOLATION;
    }

    Ccb = Ext2AllocateCcb(0, NULL, NULL);
    if (Ccb == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }

    Status = STATUS_SUCCESS;

    if (Vcb->OpenVolumeCount > 0) {
        Status = IoCheckShareAccess( DesiredAccess, ShareAccess,
                                     IrpSp->FileObject,
                                     &(Vcb->ShareAccess), TRUE);

        if (!NT_SUCCESS(Status)) {
            goto errorout;
        }
    } else {
        IoSetShareAccess( DesiredAccess, ShareAccess,
                          IrpSp->FileObject,
                          &(Vcb->ShareAccess)   );
    }


    if (Vcb->OpenVolumeCount == 0 &&
        !IsFlagOn(ShareAccess, FILE_SHARE_READ)  &&
        !IsFlagOn(ShareAccess, FILE_SHARE_WRITE) ){

        if (!IsVcbReadOnly(Vcb)) {
            Ext2FlushFiles(IrpContext, Vcb, FALSE);
            Ext2FlushVolume(IrpContext, Vcb, FALSE);
        }

        SetLongFlag(Vcb->Flags, VCB_VOLUME_LOCKED);
        Vcb->LockFile = IrpSp->FileObject;
    } else {

        if (FlagOn(IrpSp->FileObject->Flags, FO_NO_INTERMEDIATE_BUFFERING) &&
            FlagOn(DesiredAccess, FILE_READ_DATA | FILE_WRITE_DATA) ) {
            if (!IsVcbReadOnly(Vcb)) {
                Ext2FlushFiles(IrpContext, Vcb, FALSE);
                Ext2FlushVolume(IrpContext, Vcb, FALSE);
            }
        }
    }

    IrpSp->FileObject->Flags |= FO_NO_INTERMEDIATE_BUFFERING;
    IrpSp->FileObject->FsContext  = Vcb;
    IrpSp->FileObject->FsContext2 = Ccb;
    IrpSp->FileObject->Vpb = Vcb->Vpb;

    Ext2ReferXcb(&Vcb->ReferenceCount);
    Ext2ReferXcb(&Vcb->OpenHandleCount);
    Ext2ReferXcb(&Vcb->OpenVolumeCount);

    Irp->IoStatus.Information = FILE_OPENED;

errorout:

    return Status;
}

/*
 * Does this create need the volume to itself?
 *
 * The namespace changes under the exclusive volume resource: a name is
 * looked up, found missing and added as one step, and delete, supersede
 * and the volume open itself rely on no other create running meanwhile.
 * A plain open of something that exists (FILE_OPEN, or FILE_OVERWRITE,
 * which only truncates the file it finds) changes no directory: the name
 * cache is guarded by McbLock, the Fcb table by FcbLock and the file by its
 * own MainResource, so any number of them can run side by side under the
 * shared resource. Removable media stay exclusive: the verify that runs
 * first updates the volume's media state.
 */
static BOOLEAN
Ext2CreateNeedsExclusiveVcb(
    IN PEXT2_VCB            Vcb,
    IN PIO_STACK_LOCATION   IrpSp,
    IN PEXT2_FCBVCB         Xcb
)
{
    ULONG Disposition = (IrpSp->Parameters.Create.Options >> 24) & 0xFF;

    if ((IrpSp->FileObject->FileName.Length == 0 &&
         IrpSp->FileObject->RelatedFileObject == NULL) ||
        (Xcb && Xcb->Identifier.Type == EXT2VCB)) {
        return TRUE;                    /* the volume itself */
    }
    if (IsFlagOn(IrpSp->Flags, SL_OPEN_TARGET_DIRECTORY)) {
        return TRUE;                    /* a rename or link is on its way */
    }
    if (IsFlagOn(Vcb->Flags, (VCB_REMOVABLE_MEDIA | VCB_FLOPPY_DISK))) {
        return TRUE;
    }
    return (Disposition != FILE_OPEN && Disposition != FILE_OVERWRITE) ? TRUE : FALSE;
}

NTSTATUS
Ext2Create (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT      DeviceObject;
    PIRP                Irp;
    PIO_STACK_LOCATION  IrpSp;
    PEXT2_VCB           Vcb = 0;
    NTSTATUS            Status = STATUS_OBJECT_NAME_NOT_FOUND;
    PEXT2_FCBVCB        Xcb = NULL;
    BOOLEAN             PostIrp = FALSE;
    BOOLEAN             VcbResourceAcquired = FALSE;

    DeviceObject = IrpContext->DeviceObject;
    Irp = IrpContext->Irp;
    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    Xcb = (PEXT2_FCBVCB) (IrpSp->FileObject->FsContext);

    if (IsExt2FsDevice(DeviceObject)) {

        DEBUG(DL_INF, ( "Ext2Create: Create on main device object.\n"));

        Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = FILE_OPENED;

        Ext2CompleteIrpContext(IrpContext, Status);

        return Status;
    }

    __try {

        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        ASSERT(Vcb->Identifier.Type == EXT2VCB);
        IrpSp->FileObject->Vpb = Vcb->Vpb;

        if (!IsMounted(Vcb)) {
            DbgBreak();
            if (IsFlagOn(Vcb->Flags, VCB_DEVICE_REMOVED)) {
                Status = STATUS_NO_SUCH_DEVICE;
            } else {
                Status = STATUS_VOLUME_DISMOUNTED;
            }
            __leave;
        }

        if (Ext2CreateNeedsExclusiveVcb(Vcb, IrpSp, Xcb)) {
            ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);
        } else {
            ExAcquireResourceSharedLite(&Vcb->MainResource, TRUE);
        }
        VcbResourceAcquired = TRUE;

        Ext2VerifyVcb(IrpContext, Vcb);

        if (!IsMounted(Vcb) || IsFlagOn(Vcb->Flags, VCB_DISMOUNT_PENDING)) {
            Status = STATUS_VOLUME_DISMOUNTED;
            __leave;
        }

        if (FlagOn(Vcb->Flags, VCB_VOLUME_LOCKED)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        if ( ((IrpSp->FileObject->FileName.Length == 0) &&
                (IrpSp->FileObject->RelatedFileObject == NULL)) ||
                (Xcb && Xcb->Identifier.Type == EXT2VCB)  ) {
            Status = Ext2CreateVolume(IrpContext, Vcb);
        } else {

            Status = Ext2CreateFile(IrpContext, Vcb, &PostIrp);
        }

    } __finally {

        if (VcbResourceAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }

        if (!IrpContext->ExceptionInProgress && !PostIrp)  {
            if ( Status == STATUS_PENDING ||
                    Status == STATUS_CANT_WAIT) {
                Status = Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }

    return Status;
}

NTSTATUS
Ext2CreateInode(
    PEXT2_IRP_CONTEXT   IrpContext,
    PEXT2_VCB           Vcb,
    PEXT2_FCB           Parent,
    ULONG               Type,
    ULONG               FileAttr,
    PUNICODE_STRING     FileName)
{
    UNREFERENCED_PARAMETER(FileAttr);
    NTSTATUS    Status;
    ULONG       iGrp;
    ULONG       iNo;
    struct inode Inode = { 0 };
    struct dentry *Dentry = NULL;
	struct ext3_super_block *es = EXT3_SB(&Vcb->sb)->s_es;

    LARGE_INTEGER   SysTime;

    iGrp = (Parent->Inode->i_ino - 1) / INODES_PER_GROUP;

    DEBUG(DL_INF, ("Ext2CreateInode: %S in %S(Inode=%xh)\n",
                   FileName->Buffer,
                   Parent->Mcb->ShortName.Buffer,
                   Parent->Inode->i_ino));

    Status = Ext2NewInode(IrpContext, Vcb, iGrp, Type, &iNo);
    if (!NT_SUCCESS(Status)) {
        goto errorout;
    }

    KeQuerySystemTime(&SysTime);
    Ext2ClearInode(IrpContext, Vcb, iNo);
    Inode.i_sb = &Vcb->sb;
    Inode.i_ino = iNo;
    Ext2SetInodeTime(&SysTime, &Inode.i_crtime, &Inode.i_crtime_extra);
    Ext2SetInodeTime(&SysTime, &Inode.i_ctime, &Inode.i_ctime_extra);
    Ext2SetInodeTime(&SysTime, &Inode.i_mtime, &Inode.i_mtime_extra);
    Ext2SetInodeTime(&SysTime, &Inode.i_atime, &Inode.i_atime_extra);
    if (IsFlagOn(Vcb->Flags, VCB_USER_IDS)) {
        Inode.i_uid = Vcb->uid;
        Inode.i_gid = Vcb->gid;
    } else {
        Inode.i_uid = Parent->Mcb->Inode->i_uid;
        Inode.i_gid = Parent->Mcb->Inode->i_gid;
    }
    /* A fresh generation per inode, as Linux draws one: it is what tells a
       reused inode number from the file it used to be (NFS file handles,
       open-by-handle). Copying the parent's made every file of a directory
       share one. */
    {
        static ULONG Seed;
        LARGE_INTEGER Tick = KeQueryPerformanceCounter(NULL);

        if (Seed == 0) {
            Seed = Tick.LowPart | 1;
        }
        Inode.i_generation = RtlRandomEx(&Seed) ^ Tick.LowPart;
    }

    /* Windows has no umask: the parent's permissions stand in for it (a
       0755 directory gives 0644 files and 0755 directories, a 0700 one
       keeps both private). Special bits are not inherited, except that a
       directory created in a set-group-ID directory is set-group-ID too,
       as on Linux, so the group of the tree carries on down. */
    Inode.i_mode = S_IPERMISSION_MASK &
                   Parent->Inode->i_mode;
    if (Type == EXT2_FT_DIR)  {
        Inode.i_mode |= S_IFDIR;
        if (Parent->Inode->i_mode & S_ISGID) {
            Inode.i_mode |= S_ISGID;
        }
    } else if (Type == EXT2_FT_REG_FILE) {
        Inode.i_mode &= S_IFATTR;
        Inode.i_mode |= S_IFREG;
    } else {
        DbgBreak();
    }
	if (Vcb->InodeSize > EXT2_GOOD_OLD_INODE_SIZE && le16_to_cpu(es->s_want_extra_isize))
		Inode.i_extra_isize = le16_to_cpu(es->s_want_extra_isize);

    /* the flags a new inode takes from its directory, masked for its type,
       as on Linux: a directory made in a casefolded one is casefolded too,
       and sync, noatime and nodump carry on down the tree */
    Inode.i_flags = ext4_mask_flags(Inode.i_mode,
                                    (__u32)(Parent->Inode->i_flags & EXT4_FL_INHERITED));

    /* Force using extent */
    if (IsFlagOn(SUPER_BLOCK->s_feature_incompat, EXT4_FEATURE_INCOMPAT_EXTENTS)) {
        Inode.i_flags |= EXT2_EXTENTS_FL;
        ext4_ext_tree_init(IrpContext, NULL, &Inode);
        /* ext4_ext_tree_init will save inode body */
    } else {
        /* save inode body to cache */
        Ext2SaveInode(IrpContext, Vcb, &Inode);
    }

    /* add new entry to its parent */
    Status = Ext2AddEntry(
                 IrpContext,
                 Vcb,
                 Parent,
                 &Inode,
                 FileName,
                 &Dentry
             );

    if (!NT_SUCCESS(Status)) {
        DbgBreak();
        Ext2FreeInode(IrpContext, Vcb, iNo, Type);
        goto errorout;
    }

    DEBUG(DL_INF, ("Ext2CreateInode: New Inode = %xh (Type=%xh)\n",
                   Inode.i_ino, Type));

errorout:

    if (Dentry)
        Ext2FreeEntry(Dentry);

    return Status;
}

NTSTATUS
Ext2SupersedeOrOverWriteFile(
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PFILE_OBJECT      FileObject,
    IN PEXT2_VCB         Vcb,
    IN PEXT2_FCB         Fcb,
    IN PLARGE_INTEGER    AllocationSize,
    IN ULONG             Disposition
)
{
    LARGE_INTEGER   CurrentTime;
    LARGE_INTEGER   Size;

    KeQuerySystemTime(&CurrentTime);

    Size.QuadPart = 0;
    if (!MmCanFileBeTruncated(&(Fcb->SectionObject), &(Size))) {
        return STATUS_USER_MAPPED_FILE;
    }

    /* purge all file cache and shrink cache windows size */
    CcPurgeCacheSection(&Fcb->SectionObject, NULL, 0, FALSE);
    Fcb->Header.AllocationSize.QuadPart =
        Fcb->Header.FileSize.QuadPart =
            Fcb->Header.ValidDataLength.QuadPart = 0;
    CcSetFileSizes(FileObject,
                   (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);

    Size.QuadPart = CEILING_ALIGNED(ULONGLONG,
                                    (ULONGLONG)AllocationSize->QuadPart,
                                    (ULONGLONG)BLOCK_SIZE);

    if ((loff_t)Size.QuadPart > Fcb->Inode->i_size) {
        Ext2ExpandFile(IrpContext, Vcb, Fcb->Mcb, &Size);
    } else {
        Ext2TruncateFile(IrpContext, Vcb, Fcb->Mcb, &Size);
    }

    Fcb->Header.AllocationSize = Size;
    if (Fcb->Header.AllocationSize.QuadPart > 0) {
        SetLongFlag(Fcb->Flags, FCB_ALLOC_IN_CREATE);
        CcSetFileSizes(FileObject,
                       (PCC_FILE_SIZES)&Fcb->Header.AllocationSize );
    }

    /* remove all extent mappings */
    DEBUG(DL_EXT, ("Ext2SuperSede ...: %wZ\n", &Fcb->Mcb->FullName));
    Fcb->Inode->i_size = 0;

    if (Disposition == FILE_SUPERSEDE) {
        Ext2SetInodeTime(&CurrentTime, &Fcb->Inode->i_crtime, &Fcb->Inode->i_crtime_extra);
        Ext2SetInodeTime(&CurrentTime, &Fcb->Inode->i_ctime, &Fcb->Inode->i_ctime_extra);
    }
    Ext2SetInodeTime(&CurrentTime, &Fcb->Inode->i_mtime, &Fcb->Inode->i_mtime_extra);
    Ext2SetInodeTime(&CurrentTime, &Fcb->Inode->i_atime, &Fcb->Inode->i_atime_extra);
    Ext2SaveInode(IrpContext, Vcb, Fcb->Inode);

    // See if we need to overwrite EA of the file
    return Ext2OverwriteEa(IrpContext, Vcb, Fcb, &IrpContext->Irp->IoStatus);
}
