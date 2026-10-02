/**
 * create_new.c - a new file or directory: its inode, its entry, its name, its first contents.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ext2CreateFile decides that a name is to be made; this is the making:
 * Ext2CreateNewName under the directory's lock (inode, entry, name cache),
 * then, once the new file is open, Ext2InitializeCreatedFile (the EAs the
 * create carried, the SELinux label, "." and "..").
 */

#include "ext4fs.h"
#include <linux/ext4_xattr.h>
#include "create_internal.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2CreateInode)
#pragma alloc_text(PAGE, Ext2CreateNewName)
#pragma alloc_text(PAGE, Ext2InitializeCreatedFile)
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

/* The caller holds the Fcb's MainResource. */

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

        /* Return peacefully if there is no EaBuffer provided. */
        if (!EaBuffer) {
            Status = STATUS_SUCCESS;
            __leave;
        }

		/* If the caller specifies an EaBuffer, but has no knowledge about Ea,
		   we reject the request. */
		if (EaBuffer != NULL &&
			FlagOn(IrpSp->Parameters.Create.Options, FILE_NO_EA_KNOWLEDGE)) {
			Status = STATUS_ACCESS_DENIED;
			__leave;
		}

        /* Check Ea Buffer validity. */
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

        /* The EAs of Windows are the user namespace of ext4, and that is
           what the caller's set replaces. security.selinux, the POSIX ACLs
           and trusted.* belong to Linux and stay: overwriting a file with
           an EA buffer used to purge every namespace. */
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

        /* Iterate the whole EA buffer to do inspection */
        for (FullEa = (PFILE_FULL_EA_INFORMATION)EaBuffer;
            FullEa < (PFILE_FULL_EA_INFORMATION)&EaBuffer[EaBufferLength];
            FullEa = (PFILE_FULL_EA_INFORMATION)(FullEa->NextEntryOffset == 0 ?
                &EaBuffer[EaBufferLength] :
                (PCHAR)FullEa + FullEa->NextEntryOffset)) {

            OEM_STRING EaName;

            EaName.MaximumLength = EaName.Length = FullEa->EaNameLength;
            EaName.Buffer = &FullEa->EaName[0];

            /* Check if EA's name is valid */
            if (!Ext2IsEaNameValid(EaName)) {
                Status = STATUS_INVALID_EA_NAME;
                __leave;
            }
        }

        /* Now add EA entries to the inode */
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
Ext2CreateInode(
    PEXT2_IRP_CONTEXT   IrpContext,
    PEXT2_VCB           Vcb,
    PEXT2_FCB           Parent,
    ULONG               Type,
    ULONG               FileAttr,
    PUNICODE_STRING     FileName,
    PULONG              NewIno,
    struct dentry     **NewEntry)
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
        Ext2FreeInode(IrpContext, Vcb, iNo, Type);
        goto errorout;
    }

    DEBUG(DL_INF, ("Ext2CreateInode: New Inode = %xh (Type=%xh)\n",
                   Inode.i_ino, Type));

    /* the entry, for the caller to put the name into the tree at once
       (it still points at this function's inode: the caller repoints it) */
    if (NewIno) {
        *NewIno = iNo;
    }
    if (NewEntry) {
        Dentry->d_inode = NULL;
        *NewEntry = Dentry;
        Dentry = NULL;
    }

errorout:

    if (Dentry)
        Ext2FreeEntry(Dentry);

    return Status;
}

/*
 * Make the name RealName in directory ParentFcb: a new inode, its entry,
 * and the name in the name cache. The name is checked again, on disk, and
 * made under the directory's DirResource: two creates of one name cannot
 * both find it free, while creates in other directories and plain opens go
 * on under the shared volume resource. A directory deleted meanwhile, set
 * to go (NTFS refuses new names in one the same way), or made a moment ago
 * and without "." yet (see Ext2AddEntry) takes no names.
 *
 * *Created tells a new name from one another create made since the
 * caller's lookup; either way *Mcb is the name, referenced. A failure after
 * the inode was made leaves *Created set: the entry is there.
 */
NTSTATUS
Ext2CreateNewName(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_FCB            ParentFcb,
    IN PUNICODE_STRING      RealName,
    IN BOOLEAN              DirectoryFile,
    IN ULONG                FileAttributes,
    OUT PEXT2_MCB          *Mcb,
    OUT PBOOLEAN            Created
)
{
    PEXT2_MCB       Dir = ParentFcb->Mcb;
    PEXT2_ICB       DirIcb = Dir->Icb;
    struct dentry  *Entry = NULL;
    ULONG           Ino = 0;
    BOOLEAN         Exists = FALSE;
    BOOLEAN         Published = FALSE;
    NTSTATUS        Status;

    *Mcb = NULL;
    *Created = FALSE;

    Ext2JournalJoin(Vcb);
    ExAcquireResourceExclusiveLite(&DirIcb->DirResource, TRUE);

    /* A directory without "." yet is being made: its entry is on disk a
       moment before its first block, and a lookup may have found it and
       taken its DirResource ahead of its maker. The maker holds the
       DirResource of the directory above until "." and ".." are in, so
       that is waited for - with this one let go, parent before child as
       everywhere - and the directory looked at again. */
    if (ParentFcb->Inode->i_size == 0 && !IsFileDeleted(Dir)) {
        PEXT2_MCB Above = Ext2ReferParent(Vcb, Dir);

        ExReleaseResourceLite(&DirIcb->DirResource);
        if (Above != NULL) {
            ExAcquireResourceSharedLite(&Above->Icb->DirResource, TRUE);
            ExReleaseResourceLite(&Above->Icb->DirResource);
            Ext2DerefMcb(Above);
        }
        ExAcquireResourceExclusiveLite(&DirIcb->DirResource, TRUE);
    }

    __try {

        if (IsFileDeleted(Dir) || ParentFcb->Inode->i_size == 0) {
            Status = STATUS_OBJECT_PATH_NOT_FOUND;
            __leave;
        }
        if (IsFlagOn(ParentFcb->Flags, FCB_DELETE_PENDING) ||
            IsFlagOn(Dir->Flags, MCB_DELETE_PENDING)) {
            Status = STATUS_DELETE_PENDING;
            __leave;
        }

        Status = Ext2ScanDir(IrpContext, Vcb, Dir, RealName, &Ino, &Entry);
        if (NT_SUCCESS(Status)) {
            Exists = TRUE;
            __leave;
        }
        if (Status != STATUS_NO_SUCH_FILE && Status != STATUS_OBJECT_NAME_NOT_FOUND) {
            __leave;
        }

        Status = Ext2CreateInode(IrpContext, Vcb, ParentFcb,
                                 DirectoryFile ? EXT2_FT_DIR : EXT2_FT_REG_FILE,
                                 FileAttributes, RealName, &Ino, &Entry);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        *Created = TRUE;

        /* The name goes into the tree straight away, under its stripe: no
           lookup to read back what was just written. A lookup of another
           thread may have found the new entry on disk first; then its node
           stands. */
        {
            ULONG       Hash = Ext2HashMcbName(RealName);
            PERESOURCE  Stripe = Ext2AcquireNameStripe(Vcb, Dir, Hash, TRUE);

            *Mcb = Ext2FindMcbLocked(Vcb, Dir, RealName, Hash);
            if (*Mcb) {
                Status = STATUS_SUCCESS;
                Published = TRUE;
            } else {
                Status = Ext2InsertName(IrpContext, Vcb, Dir, RealName, Ino, Entry, TRUE, Mcb);
                Entry = NULL;
            }
            Ext2ReleaseNameStripe(Stripe);
        }

        /* A directory gets "." and ".." before anybody can make a name in
           it: Ext2InsertName returned it with its DirResource held, taken
           before the name could be found. (A lookup that cached the name
           first - it read the new entry on disk a moment before - leaves
           only the wait for that lock.) Waiting there, a create finds the
           directory complete instead of refusing a name in it. */
        if (NT_SUCCESS(Status) && DirectoryFile) {
            PEXT2_ICB   NewDir = (*Mcb)->Icb;
            PEXT2_FCB   NewDcb;

            if (Published) {
                KeEnterCriticalRegion();
                ExAcquireResourceExclusiveLite(&NewDir->DirResource, TRUE);
            }
            NewDcb = Ext2ReferDcb(Vcb, *Mcb);
            if (NewDcb == NULL) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                ExAcquireResourceExclusiveLite(&NewDir->EntryResource, TRUE);
                __try {
                    Status = Ext2AddDotEntries(IrpContext, Dir->Inode, (*Mcb)->Inode);
                } __finally {
                    ExReleaseResourceLite(&NewDir->EntryResource);
                }
                if (!NT_SUCCESS(Status)) {
                    /* a directory without "." is no directory: it goes */
                    Ext2DeleteFile(IrpContext, Vcb, NewDcb, *Mcb);
                }
                Ext2ReleaseFcb(NewDcb);
            }
            ExReleaseResourceLite(&NewDir->DirResource);
            KeLeaveCriticalRegion();
            if (!NT_SUCCESS(Status)) {
                PEXT2_MCB Gone = *Mcb;

                *Mcb = NULL;
                Ext2DerefMcb(Gone);
            }
        }

    } __finally {

        ExReleaseResourceLite(&DirIcb->DirResource);
        if (Entry) {
            Ext2FreeEntry(Entry);
        }
    }

    /* another create made it since the caller's lookup: it is looked up
       like any existing name, with the directory released */
    if (Exists) {
        Status = Ext2LookupFile(IrpContext, Vcb, RealName, Dir, Mcb, 0);
    }
    return Status;
}

/*
 * A file or directory Ext2CreateNewName just made, now open (the caller
 * holds its Fcb's MainResource): the EAs the create carried, the SELinux
 * label of its directory, and room for the allocation size asked for (a
 * directory has its "." and ".." already). On failure the caller takes the
 * name away.
 */
NTSTATUS
Ext2InitializeCreatedFile(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_FCB            Fcb,
    IN PEXT2_MCB            Mcb,
    IN PEXT2_MCB            ParentMcb,
    IN BOOLEAN              DirectoryFile
)
{
    PIRP        Irp = IrpContext->Irp;
    NTSTATUS    Status;

    Status = Ext2OverwriteEa(IrpContext, Vcb, Fcb, &Irp->IoStatus);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    /* the SELinux label of the directory, as Linux would give */
    Ext4InheritSecurityLabel(IrpContext, Vcb, ParentMcb, Mcb);

    UNREFERENCED_PARAMETER(ParentMcb);
    if (DirectoryFile) {
        return STATUS_SUCCESS;
    }

    if ((LONGLONG)Ext2FreeBlocks(Vcb) <=
        Ext2TotalBlocks(Vcb, &Irp->Overlay.AllocationSize, NULL)) {
        return STATUS_DISK_FULL;
    }
    return STATUS_SUCCESS;
}
