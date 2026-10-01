/**
 * ext4fs_config.h - build options, constants, device and registry names, pool tags, bug check codes.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4FS_CONFIG_H_
#define _EXT4_EXT4FS_CONFIG_H_

/* DEBUG ****************************************************************/
#if DBG
# define EXT2_DEBUG   1
#else
# define EXT2_DEBUG   0
#endif

#if EXT2_DEBUG
#if _X86_
#define DbgBreak()      __asm int 3
#else
#define DbgBreak()      KdBreakPoint()
#endif
#else
#define DbgBreak()
#endif

#include "version.h"


/* WDK DEFINITIONS ******************************************************/


/* COMPILER SWITCH / OPTIONS ********************************************/

//
// Ext2Fsd build options
//

// To support driver dynamics unload

#define EXT2_UNLOAD                     TRUE

// To support inode size expansion (fallocate)

#define EXT2_PRE_ALLOCATION_SUPPORT     TRUE

//
// Constants
//

#define EXT2_MAX_NESTED_LINKS           (8)
#define EXT2_LINKLEN_IN_INODE           (60)
#define EXT2_BLOCK_TYPES                (0x04)

#define MAXIMUM_RECORD_LENGTH           (0x10000)

#define SECTOR_BITS                     (Vcb->SectorBits)
#define SECTOR_SIZE                     (Vcb->DiskGeometry.BytesPerSector)
#define DEFAULT_SECTOR_SIZE             (0x200)

#define SUPER_BLOCK_OFFSET              (0x400)
#define SUPER_BLOCK_SIZE                (0x400)

#define READ_AHEAD_GRANULARITY          (0x10000)

#define SUPER_BLOCK                     (Vcb->SuperBlock)

#define INODE_SIZE                      (Vcb->InodeSize)
#define BLOCK_SIZE                      (Vcb->BlockSize)
#define BLOCK_BITS                      (SUPER_BLOCK->s_log_block_size + 10)
#define GROUP_DESC_SIZE                 (Vcb->sbi.s_desc_size)

#define INODES_COUNT                    (Vcb->SuperBlock->s_inodes_count)

#define INODES_PER_GROUP                (SUPER_BLOCK->s_inodes_per_group)
#define BLOCKS_PER_GROUP                (SUPER_BLOCK->s_blocks_per_group)
#define TOTAL_BLOCKS                    (ext3_blocks_count(SUPER_BLOCK))

#define EXT2_FIRST_DATA_BLOCK           (SUPER_BLOCK->s_first_data_block)

typedef struct ext3_super_block EXT2_SUPER_BLOCK, *PEXT2_SUPER_BLOCK;
typedef struct ext3_inode EXT2_INODE, *PEXT2_INODE;
typedef struct ext4_group_desc EXT2_GROUP_DESC, *PEXT2_GROUP_DESC;
typedef struct ext3_dir_entry EXT2_DIR_ENTRY, *PEXT2_DIR_ENTRY;
typedef struct ext3_dir_entry_2 EXT2_DIR_ENTRY2, *PEXT2_DIR_ENTRY2;

#define CEILING_ALIGNED(T, A, B) (((A) + (B) - 1) & (~((T)(B) - 1)))
#define COCKLOFT_ALIGNED(T, A, B) (((A) + (B)) & (~((T)(B) - 1)))



/*
 * Compile-time assertion: (Lustre version)
 *
 * Check an invariant described by a constant expression at compile time by
 * forcing a compiler error if it does not hold.  \a cond must be a constant
 * expression as defined by the ISO C Standard:
 *
 *       6.8.4.2  The switch statement
 *       ....
 *       [#3] The expression of each case label shall be  an  integer
 *       constant   expression  and  no  two  of  the  case  constant
 *       expressions in the same switch statement shall have the same
 *       value  after  conversion...
 *
 */

#define CL_ASSERT(cond) do {switch('x') {case (cond): case 0: break;}} while (0)

/* File System Releated *************************************************/

#define DRIVER_NAME      "ext4"
#define DEVICE_NAME     L"\\ext4"
#define CDROM_NAME      L"\\ext4cd"

// Registry

#define PARAMETERS_KEY      L"\\Parameters"
#define VOLUMES_KEY         L"\\Volumes"

#define READING_ONLY        L"Readonly"
#define WRITING_SUPPORT     L"WritingSupport"
#define CHECKING_BITMAP     L"CheckingBitmap"
#define EXT3_FORCEWRITING   L"Ext3ForceWriting"
#define CODEPAGE_NAME       L"CodePage"
#define HIDING_PREFIX       L"HidingPrefix"
#define HIDING_SUFFIX       L"HidingSuffix"
#define AUTO_MOUNT          L"AutoMount"
#define MOUNT_POINT         L"MountPoint"
#define UID                 L"uid"
#define GID                 L"gid"
#define EUID                L"euid"
#define EGID                L"egid"

#define DOS_DEVICE_NAME L"\\DosDevices\\ext4"

// To support ext2fsd unload routine
//
// Private IOCTL to make the driver ready to unload
//
#define IOCTL_PREPARE_TO_UNLOAD \
CTL_CODE(FILE_DEVICE_UNKNOWN, 2048, METHOD_NEITHER, FILE_WRITE_ACCESS)


#include "common.h"

typedef IO_STACK_LOCATION EXTENDED_IO_STACK_LOCATION, *PEXTENDED_IO_STACK_LOCATION;

#define IsFlagOn(a,b) ((BOOLEAN)(FlagOn(a,b) == b))
#ifndef FlagOn
#define FlagOn(_F,_SF)        ((_F) & (_SF))
#endif

#ifndef BooleanFlagOn
#define BooleanFlagOn(F,SF)   ((BOOLEAN)(((F) & (SF)) != 0))
#endif

#ifndef SetFlag
#define SetFlag(_F,_SF)       ((_F) |= (_SF))
#endif

#ifndef ClearFlag
#define ClearFlag(_F,_SF)     ((_F) &= ~(_SF))
#endif

#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif


/* Flag words are ULONG bit masks shared between threads and changed
   atomically. The Interlocked primitives are declared on LONG; the bit
   pattern is the same, so the one cast lives here. */
#define SetLongFlag(_F, _SF)    ((VOID)InterlockedOr((volatile LONG *)&(_F), (LONG)(_SF)))
#define ClearLongFlag(_F, _SF)  ((VOID)InterlockedAnd((volatile LONG *)&(_F), ~(LONG)(_SF)))

#define Ext2RaiseStatus(IRPCONTEXT,STATUS) {  \
    (IRPCONTEXT)->ExceptionCode = (STATUS); \
    ExRaiseStatus((STATUS));                \
}

#define Ext2NormalizeAndRaiseStatus(IRPCONTEXT,STATUS) {                        \
    (IRPCONTEXT)->ExceptionCode = STATUS;                                       \
    if ((STATUS) == STATUS_VERIFY_REQUIRED) { ExRaiseStatus((STATUS)); }        \
    ExRaiseStatus(FsRtlNormalizeNtstatus((STATUS),STATUS_UNEXPECTED_IO_ERROR)); \
}

//
// Define IsWritingToEof for write (append) operations
//

#define FILE_WRITE_TO_END_OF_FILE       0xffffffff

#define IsWritingToEof(Pos) (((Pos).LowPart == FILE_WRITE_TO_END_OF_FILE) && \
                             ((Pos).HighPart == -1 ))

#define IsDirectory(Fcb)    IsMcbDirectory((Fcb)->Mcb)
#define IsSpecialFile(Fcb)  IsMcbSpecialFile((Fcb)->Mcb)
#define IsSymLink(Fcb)      IsMcbSymLink((Fcb)->Mcb)
#define IsInodeSymLink(I)   S_ISLNK((I)->i_mode)
#define IsRoot(Fcb)         IsMcbRoot((Fcb)->Mcb)

//
// Pool Tags
//

#define TAG_VPB  ' bpV'
#define VPB_SIZE sizeof(VPB)

#define EXT2_DATA_MAGIC         'BD2E'
#define EXT2_INAME_MAGIC        'NI2E'
#define EXT2_FNAME_MAGIC        'NF2E'
#define EXT2_VNAME_MAGIC        'NV2E'
#define EXT2_DENTRY_MAGIC       'ED2E'
#define EXT2_DIRSP_MAGIC        'SD2E'
#define EXT2_SB_MAGIC           'BS2E'
#define EXT2_GD_MAGIC           'DG2E'
#define EXT2_INODE_MAGIC        'EI2E'
#define EXT2_ICB_MAGIC          'BI2E'
#define EXT2_FLIST_MAGIC        'LF2E'
#define EXT2_PARAM_MAGIC        'PP2E'
#define EXT4_TAG_GLOBAL         'LG2E'  /* EXT2_GLOBAL and the registry path */
#define EXT2_RWC_MAGIC          'WR2E'

//
// Bug Check Codes Definitions
//

#define EXT2_FILE_SYSTEM   (FILE_SYSTEM)

#define EXT2_BUGCHK_BLOCK               (0x00010000)
#define EXT2_BUGCHK_CLEANUP             (0x00020000)
#define EXT2_BUGCHK_CLOSE               (0x00030000)
#define EXT2_BUGCHK_CMCB                (0x00040000)
#define EXT2_BUGCHK_CREATE              (0x00050000)
#define EXT2_BUGCHK_DEBUG               (0x00060000)
#define EXT2_BUGCHK_DEVCTL              (0x00070000)
#define EXT2_BUGCHK_DIRCTL              (0x00080000)
#define EXT2_BUGCHK_DISPATCH            (0x00090000)
#define EXT2_BUGCHK_EXCEPT              (0x000A0000)
#define EXT2_BUGCHK_EXT2                (0x000B0000)
#define EXT2_BUGCHK_FASTIO              (0x000C0000)
#define EXT2_BUGCHK_FILEINFO            (0x000D0000)
#define EXT2_BUGCHK_FLUSH               (0x000E0000)
#define EXT2_BUGCHK_FSCTL               (0x000F0000)
#define EXT2_BUGCHK_INIT                (0x00100000)
#define EXT2_BUGCHK_LOCK                (0x0011000)
#define EXT2_BUGCHK_MEMORY              (0x0012000)
#define EXT2_BUGCHK_MISC                (0x0013000)
#define EXT2_BUGCHK_READ                (0x00140000)
#define EXT2_BUGCHK_SHUTDOWN            (0x00150000)
#define EXT2_BUGCHK_VOLINFO             (0x00160000)
#define EXT2_BUGCHK_WRITE               (0x00170000)

#define EXT2_BUGCHK_LAST                (0x00170000)

#define Ext2BugCheck(A,B,C,D) { KeBugCheckEx(EXT2_FILE_SYSTEM, A | __LINE__, B, C, D ); }


/* Ext2 file system definions *******************************************/

//
// The second extended file system magic number
//

#define EXT2_SUPER_MAGIC        0xEF53

#define EXT2_MIN_BLOCK          1024
#define EXT2_MIN_FRAG           1024
#define EXT2_MAX_USER_BLKSIZE   65536
//
// Inode flags (Linux uses octad number, but why ? strange!!!)
//

#define S_IFMT   0x0F000            /* 017 0000 */
#define S_IFSOCK 0x0C000            /* 014 0000 */
#define S_IFLNK  0x0A000            /* 012 0000 */
#define S_IFREG  0x08000            /* 010 0000 */
#define S_IFBLK  0x06000            /* 006 0000 */
#define S_IFDIR  0x04000            /* 004 0000 */
#define S_IFCHR  0x02000            /* 002 0000 */
#define S_IFIFO  0x01000            /* 001 0000 */

#define S_ISUID  0x00800            /* 000 4000 */
#define S_ISGID  0x00400            /* 000 2000 */
#define S_ISVTX  0x00200            /* 000 1000 */

#define S_ISREG(m)      (((m) & S_IFMT) == S_IFREG)
#define S_ISSOCK(m)     (((m) & S_IFMT) == S_IFSOCK)
#define S_ISLNK(m)      (((m) & S_IFMT) == S_IFLNK)
#define S_ISFIL(m)      (((m) & S_IFMT) == S_IFFIL)
#define S_ISBLK(m)      (((m) & S_IFMT) == S_IFBLK)
#define S_ISDIR(m)      (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)      (((m) & S_IFMT) == S_IFCHR)
#define S_ISFIFO(m)     (((m) & S_IFMT) == S_IFIFO)

#define S_IPERMISSION_MASK 0x1FF /*  */

#define S_IRWXU  0x1C0              /* 0 0700 */
#define S_IRWNU  0x180              /* 0 0600 */
#define S_IRUSR  0x100              /* 0 0400 */
#define S_IWUSR  0x080              /* 0 0200 */
#define S_IXUSR  0x040              /* 0 0100 */

#define S_IRWXG  0x038              /* 0 0070 */
#define S_IRWNG  0x030              /* 0 0060 */
#define S_IRGRP  0x020              /* 0 0040 */
#define S_IWGRP  0x010              /* 0 0020 */
#define S_IXGRP  0x008              /* 0 0010 */

#define S_IRWXO  0x007              /* 0 0007 */
#define S_IRWNO  0x006              /* 0 0006 */
#define S_IROTH  0x004              /* 0 0004 */
#define S_IWOTH  0x002              /* 0 0002 */
#define S_IXOTH  0x001              /* 0 0001 */

#define S_IRWXUGO   (S_IRWXU|S_IRWXG|S_IRWXO)
#define S_IALLUGO   (S_ISUID|S_ISGID|S_ISVTX|S_IRWXUGO)
#define S_IRUGO     (S_IRUSR|S_IRGRP|S_IROTH)
#define S_IWUGO     (S_IWUSR|S_IWGRP|S_IWOTH)
#define S_IXUGO     (S_IXUSR|S_IXGRP|S_IXOTH)
#define S_IFATTR    (S_IRWNU|S_IRWNG|S_IRWNO)

#define S_ISREADABLE(m)    (((m) & S_IPERMISSION_MASK) == (S_IRUSR | S_IRGRP | S_IROTH))
#define S_ISWRITABLE(m)    (((m) & S_IPERMISSION_MASK) == (S_IWUSR | S_IWGRP | S_IWOTH))

#define Ext2SetReadable(m) do {(m) = (m) | (S_IRUSR | S_IRGRP | S_IROTH);} while(0)
#define Ext2SetWritable(m) do {(m) = (m) | (S_IWUSR | S_IWGRP | S_IWOTH);} while(0)

#define Ext2SetOwnerWritable(m) do {(m) |= S_IWUSR;} while(0)
#define Ext2SetOwnerReadOnly(m) do {(m) &= ~S_IWUSR;} while(0)

#define Ext2IsOwnerWritable(m)  (((m) & S_IWUSR) == S_IWUSR)
#define Ext2IsOwnerReadable(m)  (((m) & S_IRUSR) == S_IRUSR)
#define Ext2IsOwnerReadOnly(m)  (!(Ext2IsOwnerWritable(m)) && Ext2IsOwnerReadable(m))

#define Ext2IsGroupWritable(m)  (((m) & S_IWGRP) == S_IWGRP)
#define Ext2IsGroupReadable(m)  (((m) & S_IRGRP) == S_IRGRP)
#define Ext2IsGroupReadOnly(m)  (!(Ext2IsGroupWritable(m)) && Ext2IsGroupReadable(m))

#define Ext2IsOtherWritable(m)  (((m) & S_IWOTH) == S_IWOTH)
#define Ext2IsOtherReadable(m)  (((m) & S_IROTH) == S_IROTH)
#define Ext2IsOtherReadOnly(m)  (!(Ext2IsOtherWritable(m)) && Ext2IsOtherReadable(m))

#define Ext2SetReadOnly(m) do {(m) &= ~(S_IWUSR | S_IWGRP | S_IWOTH);} while(0)


#define Ext2FileCanRead         (0x1)
#define Ext2FileCanWrite        (0x2)
#define Ext2FileCanExecute      (0x4)


/*
 * We need 8-bytes aligned for all the sturctures
 * It's a must for all ERESOURCE allocations
 */

#endif /* _EXT4_EXT4FS_CONFIG_H_ */
