/**
 * delete.c - removal of a name and of the inode behind the last name.
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

NTSTATUS
Ext2DeleteFile(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_FCB         Fcb,
    PEXT2_MCB         Mcb
)
{
    PEXT2_FCB       Dcb = NULL;

    NTSTATUS        Status = STATUS_UNSUCCESSFUL;

    BOOLEAN         VcbResourceAcquired = FALSE;
    BOOLEAN         FcbPagingIoAcquired = FALSE;
    BOOLEAN         FcbResourceAcquired = FALSE;
    BOOLEAN         DcbResourceAcquired = FALSE;

    LARGE_INTEGER   Size;
    LARGE_INTEGER   SysTime;

    BOOLEAN         bFcbLockAcquired = FALSE;

    DEBUG(DL_INF, ( "Ext2DeleteFile: File %wZ (%xh) will be deleted!\n",
                    &Mcb->FullName, Mcb->Inode->i_ino));

    if (IsFlagOn(Mcb->Flags, MCB_FILE_DELETED)) {
        return STATUS_SUCCESS;
    }

    if (!IsMcbSymLink(Mcb) && IsMcbDirectory(Mcb)) {
        if (!Ext2IsDirectoryEmpty(IrpContext, Vcb, Mcb)) {
            return STATUS_DIRECTORY_NOT_EMPTY;
        }
    }

    __try {

        Ext2ReferMcb(Mcb);

        ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);
        VcbResourceAcquired = TRUE;

        ExAcquireResourceExclusiveLite(&Vcb->FcbLock, TRUE);
        bFcbLockAcquired = TRUE;

        /* Mcb->Parent could be NULL when working with layered file systems */
        if (Mcb->Parent) {
            Dcb = Mcb->Parent->Icb->Fcb;
            if (!Dcb)
                Dcb = Ext2AllocateFcb(Vcb, Mcb->Parent);
        }
        if (Dcb)
            Ext2ReferXcb(&Dcb->ReferenceCount);

        if (bFcbLockAcquired) {
            ExReleaseResourceLite(&Vcb->FcbLock);
            bFcbLockAcquired = FALSE;
        }

        if (Dcb) {
            DcbResourceAcquired =
                ExAcquireResourceExclusiveLite(&Dcb->MainResource, TRUE);

            /* remove it's entry form it's parent */
            Status = Ext2RemoveEntry(IrpContext, Vcb, Dcb, Mcb);
        }

        if (NT_SUCCESS(Status)) {

            SetLongFlag(Mcb->Flags, MCB_FILE_DELETED);
            Ext2RemoveMcb(Vcb, Mcb);

            if (Fcb) {
                FcbResourceAcquired =
                    ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);

                FcbPagingIoAcquired =
                    ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
            }

            if (DcbResourceAcquired) {
                ExReleaseResourceLite(&Dcb->MainResource);
                DcbResourceAcquired = FALSE;
            }

            if (VcbResourceAcquired) {
                ExReleaseResourceLite(&Vcb->MainResource);
                VcbResourceAcquired = FALSE;
            }

            if (IsMcbSymLink(Mcb)) {
                if (Mcb->Inode->i_nlink > 0) {
                    Ext2NameUnlinked(Vcb, Mcb);
                    Status = STATUS_CANNOT_DELETE;
                    __leave;
                }
            } else if (!IsMcbDirectory(Mcb)) {
                if (Mcb->Inode->i_nlink > 0) {
                    /* other hard links keep the inode: only this name
                       is gone, an open Fcb moves over to a live one */
                    Ext2NameUnlinked(Vcb, Mcb);
                    __leave;
                }
            } else {
                /* a directory has no hard links: with its own entry gone
                   (it was checked to be empty above) the inode goes too.
                   ext3_dec_count deliberately leaves a directory at 2, the
                   way Linux does - rmdir clears the count itself. */
                if (Mcb->Inode->i_nlink > 2) {
                    __leave;
                }
            }

            if (S_ISLNK(Mcb->Inode->i_mode)) {

                /* a short symlink keeps its target inside i_block: no blocks to free */
                if (Mcb->Inode->i_size > EXT2_LINKLEN_IN_INODE) {
                    Size.QuadPart = (LONGLONG)0;
                    Status = Ext2TruncateFile(IrpContext, Vcb, Mcb, &Size);
                }

            } else {

                /* truncate file size */
                Size.QuadPart = (LONGLONG)0;
                Status = Ext2TruncateFile(IrpContext, Vcb, Mcb, &Size);

                /* check file offset mappings */
                DEBUG(DL_EXT, ("Ext2DeleteFile ...: %wZ\n", &Mcb->FullName));

                if (Fcb) {
                    Fcb->Header.AllocationSize.QuadPart = Size.QuadPart;
                    if (Fcb->Header.FileSize.QuadPart > Size.QuadPart) {
                        Fcb->Header.FileSize.QuadPart = Size.QuadPart;
                        Fcb->Mcb->Inode->i_size = Size.QuadPart;
                    }
                    if (Fcb->Header.ValidDataLength.QuadPart > Fcb->Header.FileSize.QuadPart) {
                        Fcb->Header.ValidDataLength.QuadPart = Fcb->Header.FileSize.QuadPart;
                    }
                } else if (Mcb) {
                    /* Update the inode's data length . It should be ZERO if succeeds. */
                    if (Mcb->Inode->i_size > (loff_t)Size.QuadPart) {
                        Mcb->Inode->i_size = Size.QuadPart;
                    }
                }

                /* The cached data of the file going away - not that of a
                   symlink target when only the link goes - has nowhere to be
                   written: drop the dirty pages now instead of letting the
                   writers flush them into a freed inode. A purge that cannot
                   run (a view still mapped) is harmless: such writes are
                   accepted and dropped (Ext2Write). */
                if (Fcb && Fcb->Inode == Mcb->Inode && !IsMcbDirectory(Mcb) &&
                    Fcb->SectionObject.DataSectionObject != NULL) {
                    CcPurgeCacheSection(&Fcb->SectionObject, NULL, 0, FALSE);
                }
            }

            /* An external xattr block belongs to the inode as much as its
               data does. The xattr layer owns the reference counting: with
               every item purged, writing the set back drops this inode's
               reference and frees the block once nobody else shares it
               (ext4 shares identical xattr blocks between inodes). Without
               this the block stayed allocated for ever - e2fsck reported it
               as a block bitmap difference for every deleted file that
               carried a large EA. With ea_inode, entries in the inode body
               may name value inodes too: those lose this inode's reference
               the same way, and go with their last one. */
            if (Mcb->Inode->i_file_acl != 0 || ext4_has_feature_ea_inode(&Vcb->sb)) {
                struct ext4_xattr_ref Xattr;

                if (ext4_fs_get_xattr_ref(IrpContext, Vcb, Mcb, &Xattr) == 0) {
                    ext4_xattr_remove_all(&Xattr);
                    ext4_fs_put_xattr_ref(&Xattr);
                }
            }

            /* set delete time and free the inode */
            KeQuerySystemTime(&SysTime);
            Mcb->Inode->i_nlink = 0;
            /* i_dtime is 32 bits wide, as in Linux (it only orders orphans) */
            Mcb->Inode->i_dtime = (__u32)Ext2UnixTime(&SysTime);
            Ext2SaveInode(IrpContext, Vcb, Mcb->Inode);

            /* Unhash BEFORE the number goes back to the bitmap: once it is
               free, a concurrent create can allocate it, and its lookup
               must not find this node (it would attach to a dead, already
               loaded inode and then fail with STATUS_FILE_DELETED, or
               worse, open the new file on the old inode's contents).
               Handles still holding this node see IsInodeDeleted. */
            Ext2UnhashIcb(Vcb, Mcb->Icb);
            Ext2FreeInode(IrpContext, Vcb, Mcb->Inode->i_ino, Ext2InodeType(Mcb));
        }

    } __finally {

        if (FcbPagingIoAcquired) {
            ExReleaseResourceLite(&Fcb->PagingIoResource);
        }

        if (FcbResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (DcbResourceAcquired) {
            ExReleaseResourceLite(&Dcb->MainResource);
        }

        if (bFcbLockAcquired) {
            ExReleaseResourceLite(&Vcb->FcbLock);
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
                    &Mcb->FullName, ext3_free_blocks_count(SUPER_BLOCK)));

    return Status;
}
