/**
 * FileDelete.c - removal of a name and of the inode behind the last name.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "linux\ext4.h"
#include "linux\ext4_xattr.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2DeleteFile)
#endif

ULONG
Ext2InodeType(PEXT2_MCB Mcb)
{
    /* the inode is the truth: a symlink whose target went away keeps
       the directory attribute of the target on its Mcb, and freeing it
       as a directory would take one off the group's directory count */
    if (Mcb->Inode) {
        if (S_ISLNK(Mcb->Inode->i_mode)) {
            return EXT2_FT_SYMLINK;
        }
        if (S_ISDIR(Mcb->Inode->i_mode)) {
            return EXT2_FT_DIR;
        }
        return EXT2_FT_REG_FILE;
    }

    if (IsMcbSymLink(Mcb)) {
        return EXT2_FT_SYMLINK;
    }

    if (IsMcbDirectory(Mcb)) {
        return EXT2_FT_DIR;
    }

    return EXT2_FT_REG_FILE;
}

/*
 * The inode behind the last name goes: its data, its extended attributes,
 * then the inode itself. The name is gone already, so what cannot be given
 * back does not fail the delete - the inode then stays, unlinked and
 * allocated with all it still holds: a leak e2fsck reclaims, never a block
 * that two owners would claim. Fails only when the volume stopped writing
 * or the inode bitmap disagrees (corruption).
 */
static NTSTATUS
Ext2ReleaseInode(
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PEXT2_VCB         Vcb,
    IN PEXT2_FCB         Fcb,
    IN PEXT2_MCB         Mcb
)
{
    struct inode   *Inode = Mcb->Inode;
    LARGE_INTEGER   Size;
    LARGE_INTEGER   SysTime;
    NTSTATUS        Status = STATUS_SUCCESS;
    NTSTATUS        Kept;

    /* a short symlink keeps its target inside i_block: no blocks to free
       (a 60 byte one has a block, which used to be left behind) */
    Size.QuadPart = 0;
    if (!ext4_inode_is_fast_symlink(Inode)) {
        Status = Ext2TruncateFile(IrpContext, Vcb, Mcb, &Size);
    }

    if (!S_ISLNK(Inode->i_mode)) {
        if (Fcb) {
            Fcb->Header.AllocationSize.QuadPart = Size.QuadPart;
            if (Fcb->Header.FileSize.QuadPart > Size.QuadPart) {
                Fcb->Header.FileSize.QuadPart = Size.QuadPart;
                Fcb->Mcb->Inode->i_size = Size.QuadPart;
            }
            if (Fcb->Header.ValidDataLength.QuadPart > Fcb->Header.FileSize.QuadPart) {
                Fcb->Header.ValidDataLength.QuadPart = Fcb->Header.FileSize.QuadPart;
            }
        } else if (Inode->i_size > (loff_t)Size.QuadPart) {
            Inode->i_size = Size.QuadPart;
        }

        /* The cached data of the file going away - not that of a symlink
           target when only the link goes - has nowhere to be written: drop
           the dirty pages now instead of letting the writers flush them
           into a freed inode. A purge that cannot run (a view still mapped)
           is harmless: such writes are accepted and dropped (Ext2Write). */
        if (Fcb && Fcb->Inode == Inode && !IsMcbDirectory(Mcb) &&
            Fcb->SectionObject.DataSectionObject != NULL) {
            CcPurgeCacheSection(&Fcb->SectionObject, NULL, 0, FALSE);
        }
    }

    /* An external xattr block belongs to the inode as much as its data
       does. The xattr layer owns the reference counting: with every item
       purged, writing the set back drops this inode's reference and frees
       the block once nobody else shares it (ext4 shares identical xattr
       blocks between inodes). With ea_inode, entries in the inode body may
       name value inodes too: those lose this inode's reference the same
       way, and go with their last one. */
    if (NT_SUCCESS(Status) &&
        (Inode->i_file_acl != 0 || ext4_has_feature_ea_inode(&Vcb->sb))) {
        struct ext4_xattr_ref Xattr;
        int rc = ext4_fs_get_xattr_ref(IrpContext, Vcb, Mcb, &Xattr);

        if (rc == 0) {
            ext4_xattr_remove_all(&Xattr);
            rc = ext4_fs_put_xattr_ref(&Xattr);
        }
        Status = Ext2WinntError(rc);
    }
    Kept = Status;

    /* i_dtime is 32 bits wide, as in Linux (it only orders orphans) */
    KeQuerySystemTime(&SysTime);
    Inode->i_nlink = 0;
    Inode->i_dtime = (__u32)Ext2UnixTime(&SysTime);
    Status = Ext2SaveInode(IrpContext, Vcb, Inode) ? STATUS_SUCCESS : STATUS_UNEXPECTED_IO_ERROR;

    /* Unhash BEFORE the number goes back to the bitmap: once it is free, a
       concurrent create can allocate it, and its lookup must not find this
       node (it would attach to a dead, already loaded inode and then fail
       with STATUS_FILE_DELETED, or worse, open the new file on the old
       inode's contents). Handles still holding this node see
       IsInodeDeleted. */
    Ext2UnhashIcb(Vcb, Mcb->Icb);

    if (!NT_SUCCESS(Kept)) {
        DbgPrint("ext4: inode %u lost its last name but keeps its blocks (status %08x); "
                 "e2fsck reclaims them\n", Inode->i_ino, Kept);
        return Status;
    }
    if (NT_SUCCESS(Status)) {
        Status = Ext2FreeInode(IrpContext, Vcb, Inode->i_ino, Ext2InodeType(Mcb));
    }
    return Status;
}

NTSTATUS
Ext2DeleteFile(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_FCB         Fcb,
    PEXT2_MCB         Mcb
)
{
    PEXT2_FCB       Dcb = NULL;
    PEXT2_ICB       ParentDir = NULL;
    PEXT2_ICB       OwnDir = NULL;

    NTSTATUS        Status = STATUS_UNSUCCESSFUL;

    BOOLEAN         VcbResourceAcquired = FALSE;
    BOOLEAN         FcbPagingIoAcquired = FALSE;
    BOOLEAN         FcbResourceAcquired = FALSE;
    BOOLEAN         LastName = FALSE;

    DEBUG(DL_INF, ( "Ext2DeleteFile: File %wZ (%xh) will be deleted!\n",
                    &Mcb->FullName, Mcb->Inode->i_ino));

    if (IsFlagOn(Mcb->Flags, MCB_FILE_DELETED)) {
        return STATUS_SUCCESS;
    }

    __try {

        Ext2ReferMcb(Mcb);

        /* Shared: deletes in different directories run side by side, the
           parent's DirResource orders those in one. Rename, which moves
           names between directories, still takes the volume exclusively.
           The journal is joined before any of it (Ext2JournalJoin). */
        Ext2JournalJoin(Vcb);
        ExAcquireResourceSharedLite(&Vcb->MainResource, TRUE);
        VcbResourceAcquired = TRUE;

        /* Mcb->Parent could be NULL when working with layered file systems */
        if (Mcb->Parent) {
            Dcb = Ext2ReferDcb(Vcb, Mcb->Parent);
        }

        /* Two DirResources are held at once only as a parent and its child,
           always the parent first - a create of a directory holds its
           parent's while it gives the new one "." and "..". */
        if (Dcb) {
            ParentDir = Dcb->Mcb->Icb;
            ExAcquireResourceExclusiveLite(&ParentDir->DirResource, TRUE);
        }

        /* A directory goes only empty, and stays so until its entry is
           gone: a create inside it waits on its own DirResource, then
           finds it deleted. */
        if (!IsMcbSymLink(Mcb) && IsMcbDirectory(Mcb)) {
            OwnDir = Mcb->Icb;
            ExAcquireResourceExclusiveLite(&OwnDir->DirResource, TRUE);
            if (!Ext2IsDirectoryEmpty(IrpContext, Vcb, Mcb)) {
                Status = STATUS_DIRECTORY_NOT_EMPTY;
                __leave;
            }
        }

        if (Dcb) {
            /* another delete of this name got here first */
            if (IsFlagOn(Mcb->Flags, MCB_FILE_DELETED)) {
                Status = STATUS_SUCCESS;
                __leave;
            }

            /* remove its entry form its parent */
            /* Hard links of one inode can go at the same time, each under
               its own directory's lock: the delete that took the last name
               frees the inode, the others leave it be */
            Status = Ext2RemoveEntry(IrpContext, Vcb, Dcb, Mcb, &LastName);
            if (NT_SUCCESS(Status)) {
                SetLongFlag(Mcb->Flags, MCB_FILE_DELETED);
            }
        }

        if (OwnDir) {
            ExReleaseResourceLite(&OwnDir->DirResource);
            OwnDir = NULL;
        }
        if (ParentDir) {
            ExReleaseResourceLite(&ParentDir->DirResource);
            ParentDir = NULL;
        }

        if (NT_SUCCESS(Status)) {

            Ext2RemoveMcb(Vcb, Mcb);

            if (Fcb) {
                FcbResourceAcquired =
                    ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);

                FcbPagingIoAcquired =
                    ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
            }

            if (VcbResourceAcquired) {
                ExReleaseResourceLite(&Vcb->MainResource);
                VcbResourceAcquired = FALSE;
            }

            if (IsMcbSymLink(Mcb)) {
                if (!LastName) {
                    Ext2NameUnlinked(Vcb, Mcb);
                    Status = STATUS_CANNOT_DELETE;
                    __leave;
                }
            } else if (!IsMcbDirectory(Mcb)) {
                if (!LastName) {
                    /* other hard links keep the inode: only this name
                       is gone, an open Fcb moves over to a live one */
                    Ext2NameUnlinked(Vcb, Mcb);
                    __leave;
                }
            } else {
                /* a directory has no hard links: with its own entry gone
                   (it was checked to be empty above) the inode goes too */
                if (!LastName) {
                    __leave;
                }
            }

            Status = Ext2ReleaseInode(IrpContext, Vcb, Fcb, Mcb);
        }

    } __finally {

        if (FcbPagingIoAcquired) {
            ExReleaseResourceLite(&Fcb->PagingIoResource);
        }

        if (FcbResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (ParentDir) {
            ExReleaseResourceLite(&ParentDir->DirResource);
        }

        if (OwnDir) {
            ExReleaseResourceLite(&OwnDir->DirResource);
        }

        if (VcbResourceAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }

        if (Dcb) {
            Ext2ReleaseFcb(Dcb);
        }

        Ext2DerefMcb(Mcb);
    }

    DEBUG(DL_INF, ( "Ext2DeleteFile: %wZ Succeed... EXT2SB->S_FREE_BLOCKS = %I64xh .\n",
                    &Mcb->FullName, Ext2FreeBlocks(Vcb)));

    return Status;
}
