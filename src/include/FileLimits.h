/* SPDX-License-Identifier: GPL-2.0-only
 * FileLimits.h - checked file-logical bounds before mutation.
 * File-logical limits are independent of the volume's physical address width.
 */
#ifndef EXT4_FILE_LIMITS_H
#define EXT4_FILE_LIMITS_H
static inline ULONGLONG Ext4FileSizeLimit(PEXT2_VCB Vcb, const struct inode *Inode)
{
    /* The exclusive logical end must fit the block mapper's 32-bit index. */
    ULONGLONG Blocks = INODE_HAS_EXTENT(Inode) ? MAXULONG : Vcb->max_data_blocks;
    return Blocks * Vcb->BlockSize;
}

static inline NTSTATUS Ext4CheckFileSize(PEXT2_VCB Vcb, const struct inode *Inode, LONGLONG Size)
{
    if (Size < 0)
        return STATUS_INVALID_PARAMETER;
    return (ULONGLONG)Size > Ext4FileSizeLimit(Vcb, Inode) ? STATUS_FILE_TOO_LARGE : STATUS_SUCCESS;
}

static inline NTSTATUS Ext4CheckWriteRange(PEXT2_VCB Vcb, const struct inode *Inode,
    LONGLONG Offset, ULONG Length)
{
    ULONGLONG Limit = Ext4FileSizeLimit(Vcb, Inode);
    if (Offset < 0)
        return STATUS_INVALID_PARAMETER;
    if ((ULONGLONG)Offset > Limit || Length > Limit - (ULONGLONG)Offset)
        return STATUS_FILE_TOO_LARGE;
    return STATUS_SUCCESS;
}
#endif
