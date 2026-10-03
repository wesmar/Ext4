/**
 * VolumeInitialize.c - building a VCB from a superblock: the mount, step by step.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ext2InitializeVcb takes a volume from a superblock that looks like ext
 * to a mounted Vcb, in the order each step needs the ones before it:
 *
 *   1. the superblock is checked: geometry, features, size limits
 *   2. the Vcb's own objects: resources, the name cache, the inode table
 *   3. the device: label, characteristics, serial, partition, media count
 *   4. the volume stream and its cache, the block device of the bh layer
 *   5. the superblock's derived values (sbi), the read-only decisions
 *   6. the block groups: their locks, every descriptor block
 *   7. the journal and multi-mount protection: replay, then logging
 *   8. the root directory
 *
 * What a step set up is recorded in an EXT2_MOUNT_STATE, and a mount that
 * fails is undone from it (Ext2UndoMount), whichever step it failed in.
 */

#include "ext4fs.h"
#include "../core/core_internal.h"
#include "vcb_internal.h"

#define is_power_of_2(x)        ((x) != 0 && (((x) & ((x) - 1)) == 0))

typedef struct _EXT2_MOUNT_STATE {
    BOOLEAN     Resources;          /* ERESOURCEs of the Vcb */
    BOOLEAN     NotifySync;
    BOOLEAN     InodeLookaside;
    BOOLEAN     VolumeCache;        /* CcInitializeCacheMap on the stream */
    BOOLEAN     Extents;            /* Vcb->Extents and the bh layer's lock */
    BOOLEAN     GroupLoaded;
} EXT2_MOUNT_STATE, *PEXT2_MOUNT_STATE;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2CheckBitmapConsistency)
#pragma alloc_text(PAGE, Ext2CheckSetBlock)
#pragma alloc_text(PAGE, Ext2InitializeVcb)
#endif

static inline BOOLEAN Ext2IsNullUuid (__u8 * uuid)
{
    int i;
    for (i = 0; i < 16; i++) {
        if (uuid[i]) {
            break;
        }
    }

    return (i >= 16);
}

/* ---------------------------------------------------------------- bitmap check */

BOOLEAN
Ext2CheckSetBlock(PEXT2_IRP_CONTEXT IrpContext, PEXT2_VCB Vcb, LONGLONG Block)
{
    UNREFERENCED_PARAMETER(IrpContext);
    PEXT2_GROUP_DESC    gd;
    struct buffer_head *gb = NULL;
    struct buffer_head *bh = NULL;
    ULONG               group, dwBlk, Length;
    RTL_BITMAP          bitmap;
    BOOLEAN             bModified = FALSE;

    if (Block < EXT2_FIRST_DATA_BLOCK || (ULONGLONG)Block >= TOTAL_BLOCKS) {
        return FALSE;
    }
    group = (ULONG)((Block - EXT2_FIRST_DATA_BLOCK) / BLOCKS_PER_GROUP);
    dwBlk = (ULONG)((Block - EXT2_FIRST_DATA_BLOCK) % BLOCKS_PER_GROUP);

    gd = ext4_get_group_desc(&Vcb->sb, group, &gb);
    if (!gd) {
        return FALSE;
    }
    bh = sb_getblk(&Vcb->sb, ext4_block_bitmap(&Vcb->sb, gd));
    if (!bh) {
        fini_bh(&gb);
        return FALSE;
    }

    if (group == Vcb->sbi.s_groups_count - 1) {
        Length = (ULONG)(TOTAL_BLOCKS % BLOCKS_PER_GROUP);

        /* s_blocks_count is integer multiple of s_blocks_per_group */
        if (Length == 0)
            Length = BLOCKS_PER_GROUP;
    } else {
        Length = BLOCKS_PER_GROUP;
    }

    if (dwBlk >= Length) {
        fini_bh(&gb);
        fini_bh(&bh);
        return FALSE;
    }

    RtlInitializeBitMap(&bitmap, (PULONG)bh->b_data, Length);

    if (RtlCheckBit(&bitmap, dwBlk) == 0) {
        RtlSetBits(&bitmap, dwBlk, 1);
        bModified = TRUE;
        mark_buffer_dirty(bh);
    }

    fini_bh(&gb);
    fini_bh(&bh);

    return (!bModified);
}

BOOLEAN
Ext2CheckBitmapConsistency(PEXT2_IRP_CONTEXT IrpContext, PEXT2_VCB Vcb)
{
    ULONG i, j, InodeBlocks;

    for (i = 0; i < Vcb->sbi.s_groups_count; i++) {

        PEXT2_GROUP_DESC    gd;
        struct buffer_head  *bh = NULL;

        gd = ext4_get_group_desc(&Vcb->sb, i, &bh);
        if (!gd)
            continue;
        Ext2CheckSetBlock(IrpContext, Vcb, ext4_block_bitmap(&Vcb->sb, gd));
        Ext2CheckSetBlock(IrpContext, Vcb, ext4_inode_bitmap(&Vcb->sb, gd));

        if (i == Vcb->sbi.s_groups_count - 1) {
            InodeBlocks = ((INODES_COUNT % INODES_PER_GROUP) *
                           Vcb->InodeSize + Vcb->BlockSize - 1) /
                          (Vcb->BlockSize);
        } else {
            InodeBlocks = (INODES_PER_GROUP * Vcb->InodeSize +
                           Vcb->BlockSize - 1) / (Vcb->BlockSize);
        }

        for (j = 0; j < InodeBlocks; j++ )
            Ext2CheckSetBlock(IrpContext, Vcb, ext4_inode_table(&Vcb->sb, gd) + j);

        fini_bh(&bh);
    }

    return TRUE;
}

/* ---------------------------------------------------------------- 1. superblock */

/*
 * The geometry a superblock must have before anything is computed from it,
 * as Linux checks it (ext4_fill_super, ext4_check_geometry): a value out of
 * range makes block, inode and group arithmetic divide by zero, shift past
 * the width of the type or index past its tables. An ext superblock that
 * fails is a corrupt volume, not an unknown one: reported as such, so no
 * other file system (RAW) takes it - and nobody is offered to format it.
 */
static NTSTATUS
Ext4CheckGeometry(IN PEXT2_SUPER_BLOCK sb)
{
    ULONG       log = le32_to_cpu(sb->s_log_block_size);
    ULONG       bs, isize, ipg, bpg, first;
    ULONGLONG   blocks, groups;
    const char *why = NULL;

    if (log > EXT4_MAX_BLOCK_LOG_SIZE - EXT4_MIN_BLOCK_LOG_SIZE) {
        why = "block size";
        goto bad;
    }
    bs = EXT4_MIN_BLOCK_SIZE << log;

    if (le32_to_cpu(sb->s_rev_level) >= EXT4_DYNAMIC_REV) {
        isize = le16_to_cpu(sb->s_inode_size);
        if (isize < EXT4_GOOD_OLD_INODE_SIZE || isize > bs || !is_power_of_2(isize)) {
            why = "inode size";
            goto bad;
        }
        if (le32_to_cpu(sb->s_first_ino) < EXT4_GOOD_OLD_FIRST_INO) {
            why = "first inode";
            goto bad;
        }
        if (isize > EXT4_GOOD_OLD_INODE_SIZE &&
            (le16_to_cpu(sb->s_min_extra_isize) > isize - EXT4_GOOD_OLD_INODE_SIZE ||
             le16_to_cpu(sb->s_want_extra_isize) > isize - EXT4_GOOD_OLD_INODE_SIZE)) {
            why = "extra inode size";
            goto bad;
        }
    } else {
        isize = EXT4_GOOD_OLD_INODE_SIZE;
    }

    bpg = le32_to_cpu(sb->s_blocks_per_group);
    ipg = le32_to_cpu(sb->s_inodes_per_group);
    if (bpg == 0 || bpg > bs * 8) {
        why = "blocks per group";
        goto bad;
    }
    if (ipg < bs / isize || ipg > bs * 8) {
        why = "inodes per group";
        goto bad;
    }

    blocks = ext3_blocks_count(sb);
    first = le32_to_cpu(sb->s_first_data_block);
    if (blocks == 0 || first >= blocks) {
        why = "block count";
        goto bad;
    }
    /* Physical extents have 48-bit addresses; Windows I/O uses signed byte offsets. */
    if (blocks > (1ULL << 48) || blocks > (ULONGLONG)MAXLONGLONG / bs ||
        (blocks > MAXULONG && !(le32_to_cpu(sb->s_feature_incompat) & EXT4_FEATURE_INCOMPAT_64BIT))) {
        why = "physical address range";
        goto bad;
    }
    groups = (blocks - first - 1) / bpg + 1;
    if (groups > MAXULONG || groups * ipg != le32_to_cpu(sb->s_inodes_count)) {
        why = "inode count";
        goto bad;
    }
    if (le16_to_cpu(sb->s_reserved_gdt_blocks) > bs / 4) {
        why = "reserved GDT blocks";
        goto bad;
    }
    return STATUS_SUCCESS;

bad:
    DbgPrint("ext4: superblock: bad %s, the volume is not mounted\n", why);
    return STATUS_DISK_CORRUPT_ERROR;
}

/*
 * Whether this driver may mount the volume, and how: geometry, features
 * (unknown incompatible ones refuse it, unknown read-only-compatible ones,
 * and casefolding in an encoding not built in, mount it read-only), the
 * block count it can address, the block size.
 */
static NTSTATUS
Ext2CheckSuperBlock(IN PEXT2_VCB Vcb, IN PEXT2_SUPER_BLOCK sb)
{
    NTSTATUS Status;

    /* nothing is computed from a superblock with impossible geometry */
    Status = Ext4CheckGeometry(sb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    /* Reject mounting volume if we encounter unsupported incompat features */
    if (FlagOn(sb->s_feature_incompat,
               ~(EXT4_FEATURE_INCOMPAT_SUPP | EXT4_FEATURE_INCOMPAT_READ_ONLY))) {
        DbgPrint("ext4: unsupported incompatible features (%xh), the volume is not mounted\n",
                 le32_to_cpu(sb->s_feature_incompat));
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    /* ... and read-only for the ones it reads but does not write */
    if (FlagOn(sb->s_feature_incompat, EXT4_FEATURE_INCOMPAT_READ_ONLY)) {
        SetLongFlag(Vcb->Flags, VCB_RO_COMPAT_READ_ONLY);
    }

    /* Mount the volume RO if we encounter unsupported ro_compat features */
    if (FlagOn(sb->s_feature_ro_compat, ~EXT4_FEATURE_RO_COMPAT_SUPP)) {
        SetLongFlag(Vcb->Flags, VCB_RO_COMPAT_READ_ONLY);
    }

    /* casefolded directories are changed only with the tables of their
       encoding; with another one they are still read (lookups fall back
       to a linear scan), but nothing is written */
    if (FlagOn(sb->s_feature_incompat, EXT4_FEATURE_INCOMPAT_CASEFOLD) &&
        !Ext4Utf8Supported(le16_to_cpu(sb->s_encoding))) {
        SetLongFlag(Vcb->Flags, VCB_RO_COMPAT_READ_ONLY);
    }

    /* Recognize the filesystem as Ext3fs if it supports journalling */
    if (IsFlagOn(sb->s_feature_compat, EXT4_FEATURE_COMPAT_HAS_JOURNAL)) {
        Vcb->IsExt3fs = TRUE;
    }

    /* we cannot handle volume with block size bigger than 64k */
    Vcb->BlockSize  = (EXT2_MIN_BLOCK_SIZE << sb->s_log_block_size);
    if (Vcb->BlockSize > EXT2_MAX_USER_BLKSIZE) {
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    if (Vcb->BlockSize >= PAGE_SIZE) {
        Vcb->IoUnitBits = PAGE_SHIFT;
        Vcb->IoUnitSize = PAGE_SIZE;
    } else {
        Vcb->IoUnitSize = Vcb->BlockSize;
        Vcb->IoUnitBits = Ext2Log2(Vcb->BlockSize);
    }
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- 2. objects */

static NTSTATUS
Ext2InitializeVcbObjects(IN PEXT2_VCB Vcb, IN PEXT2_SUPER_BLOCK sb,
                         IN PDEVICE_OBJECT TargetDevice, IN PDEVICE_OBJECT VolumeDevice,
                         IN PVPB Vpb, IN OUT PEXT2_MOUNT_STATE State)
{
    NTSTATUS Status;

    /* initialize vcb header members ... */
    Vcb->Header.IsFastIoPossible = FastIoIsNotPossible;
    Vcb->Header.Resource = &(Vcb->MainResource);
    Vcb->Header.PagingIoResource = &(Vcb->PagingIoResource);
    Vcb->OpenVolumeCount = 0;
    Vcb->OpenHandleCount = 0;
    Vcb->ReferenceCount = 0;

    /* initialize eresources */
    ExInitializeResourceLite(&Vcb->MainResource);
    ExInitializeResourceLite(&Vcb->PagingIoResource);
    ExInitializeResourceLite(&Vcb->SuperLock);
    ExInitializeResourceLite(&Vcb->FcbLock);
    ExInitializeResourceLite(&Vcb->sbi.s_gd_lock);
    ExInitializeFastMutex(&Vcb->Mutex);
    FsRtlSetupAdvancedHeader(&Vcb->Header,  &Vcb->Mutex);
    State->Resources = TRUE;

    /* initialize Fcb list head */
    InitializeListHead(&Vcb->FcbList);

    /* the name cache: hash, stripes and reaping order, sized for this
       machine (Ext2InitializeNameCache) */
    Status = Ext2InitializeNameCache(Vcb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    /* initialize the inode node table */
    Ext2InitializeIcbTable(Vcb);

    /* initialize directory notify list */
    InitializeListHead(&Vcb->NotifyList);
    FsRtlNotifyInitializeSync(&Vcb->NotifySync);
    State->NotifySync = TRUE;

    Vcb->SuperBlock = sb;
    Vcb->DeviceObject = VolumeDevice;
    Vcb->TargetDeviceObject = TargetDevice;
    Vcb->Vpb = Vpb;
    Vcb->RealDevice = Vpb->RealDevice;
    Vpb->DeviceObject = VolumeDevice;

    /* revision 0 has no inode size field (Linux ignores what is there) */
    if (le32_to_cpu(sb->s_rev_level) >= EXT4_DYNAMIC_REV) {
        Vcb->InodeSize = (ULONG)le16_to_cpu(sb->s_inode_size);
    } else {
        Vcb->InodeSize = EXT2_GOOD_OLD_INODE_SIZE;
    }

    ExInitializeNPagedLookasideList(&(Vcb->InodeLookasideList),
                                    NULL, NULL, 0, Vcb->InodeSize,
                                    'SNIE', 0);
    State->InodeLookaside = TRUE;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- 3. device */

static NTSTATUS
Ext2ProbeDevice(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PEXT2_SUPER_BLOCK sb,
                IN PDEVICE_OBJECT TargetDevice, IN PVPB Vpb)
{
    NTSTATUS    Status;
    ULONG       IoctlSize, ChangeCount = 0;
    LONGLONG    DiskSize, PartSize;
    PARTITION_INFORMATION_EX PartitionEx;
    GET_LENGTH_INFORMATION DeviceLength;

    /* the label in the Vpb; a name that does not convert leaves it empty */
    Ext2InitializeLabel(Vcb, sb);

    /* check device characteristics flags */
    if (IsFlagOn(Vpb->RealDevice->Characteristics, FILE_REMOVABLE_MEDIA)) {
        SetLongFlag(Vcb->Flags, VCB_REMOVABLE_MEDIA);
    }
    if (IsFlagOn(Vpb->RealDevice->Characteristics, FILE_FLOPPY_DISKETTE)) {
        SetLongFlag(Vcb->Flags, VCB_FLOPPY_DISK);
    }
    if (IsFlagOn(Vpb->RealDevice->Characteristics, FILE_READ_ONLY_DEVICE) ||
        IsFlagOn(TargetDevice->Characteristics, FILE_READ_ONLY_DEVICE) ||
        Ext2IsMediaWriteProtected(IrpContext, TargetDevice)) {
        SetLongFlag(Vcb->Flags, VCB_WRITE_PROTECTED);
    }

    /* initialize UUID and serial number */
    if (Ext2IsNullUuid(sb->s_uuid)) {
        ExUuidCreate((UUID *)sb->s_uuid);
    }
    Vpb->SerialNumber = ((ULONG*)sb->s_uuid)[0] +
                        ((ULONG*)sb->s_uuid)[1] +
                        ((ULONG*)sb->s_uuid)[2] +
                        ((ULONG*)sb->s_uuid)[3];

    /* query partition size and disk geometry parameters */
    DiskSize =  Vcb->DiskGeometry.Cylinders.QuadPart *
                Vcb->DiskGeometry.TracksPerCylinder *
                Vcb->DiskGeometry.SectorsPerTrack *
                Vcb->DiskGeometry.BytesPerSector;

    IoctlSize = sizeof(PARTITION_INFORMATION);
    Status = Ext2DiskIoControl(TargetDevice, IOCTL_DISK_GET_PARTITION_INFO,
                               NULL, 0, &Vcb->PartitionInformation, &IoctlSize);
    if (NT_SUCCESS(Status)) {
        PartSize = Vcb->PartitionInformation.PartitionLength.QuadPart;
    } else {
        Vcb->PartitionInformation.StartingOffset.QuadPart = 0;
        Vcb->PartitionInformation.PartitionLength.QuadPart = DiskSize;
        PartSize = DiskSize;
    }
    /* GPT and large devices need their reported length, not rounded CHS geometry. */
    IoctlSize = sizeof(PartitionEx);
    Status = Ext2DiskIoControl(TargetDevice, IOCTL_DISK_GET_PARTITION_INFO_EX,
                               NULL, 0, &PartitionEx, &IoctlSize);
    if (NT_SUCCESS(Status)) {
        PartSize = PartitionEx.PartitionLength.QuadPart;
        Vcb->PartitionInformation.StartingOffset = PartitionEx.StartingOffset;
    } else {
        IoctlSize = sizeof(DeviceLength);
        Status = Ext2DiskIoControl(TargetDevice, IOCTL_DISK_GET_LENGTH_INFO,
                                   NULL, 0, &DeviceLength, &IoctlSize);
        if (NT_SUCCESS(Status))
            PartSize = DeviceLength.Length.QuadPart;
    }
    if (PartSize <= 0)
        return STATUS_DISK_CORRUPT_ERROR;
    Vcb->PartitionInformation.PartitionLength.QuadPart = PartSize;
    Vcb->Header.AllocationSize.QuadPart =
        Vcb->Header.FileSize.QuadPart =
            Vcb->Header.ValidDataLength.QuadPart = PartSize;

    /* a file system larger than its partition (a truncated image, a
       corrupt block count): Linux refuses it too - every block past
       the end would be read from, or written to, what is not ours */
    if (ext3_blocks_count(sb) > (ULONGLONG)PartSize / Vcb->BlockSize) {
        DbgPrint("ext4: %I64u blocks of %u bytes do not fit the partition (%I64d bytes)\n",
                 (ULONGLONG)ext3_blocks_count(sb), Vcb->BlockSize, PartSize);
        return STATUS_DISK_CORRUPT_ERROR;
    }

    /* verify count */
    IoctlSize = sizeof(ULONG);
    Status = Ext2DiskIoControl(TargetDevice, IOCTL_DISK_CHECK_VERIFY,
                               NULL, 0, &ChangeCount, &IoctlSize);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Vcb->ChangeCount = ChangeCount;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- 4. stream */

static NTSTATUS
Ext2OpenVolumeStream(IN PEXT2_VCB Vcb, IN OUT PEXT2_MOUNT_STATE State)
{
    CC_FILE_SIZES   FileSizes;

    /* create the stream object for ext2 volume */
    KeInitializeEvent(&Vcb->StreamClosed, NotificationEvent, FALSE);
    Vcb->Volume = IoCreateStreamFileObject(NULL, Vcb->Vpb->RealDevice);
    if (!Vcb->Volume) {
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    /* initialize streaming object file */
    Vcb->Volume->SectionObjectPointer = &(Vcb->SectionObject);
    Vcb->Volume->ReadAccess = TRUE;
    Vcb->Volume->WriteAccess = TRUE;
    Vcb->Volume->DeleteAccess = TRUE;
    Vcb->Volume->FsContext = (PVOID) Vcb;
    Vcb->Volume->FsContext2 = NULL;
    Vcb->Volume->Vpb = Vcb->Vpb;

    FileSizes.AllocationSize.QuadPart =
        FileSizes.FileSize.QuadPart =
            FileSizes.ValidDataLength.QuadPart =
                Vcb->Header.AllocationSize.QuadPart;

    /* CcInitializeCacheMap can raise (e.g. STATUS_INSUFFICIENT_RESOURCES).
       If it does, the cache map is only partially set up and the undo
       must not call CcUninitializeCacheMap on it, or the Cache Manager
       bugchecks (0x34 CACHE_MANAGER): only a map set up is recorded. */
    __try {
        CcInitializeCacheMap( Vcb->Volume,
                              &FileSizes,
                              TRUE,
                              &(Ext2Global->CacheManagerNoOpCallbacks),
                              Vcb );
        State->VolumeCache = TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Initialization embeds the lock and empty tree; no allocation is needed. */
    Ext4RunMapInitialize(&(Vcb->Extents));

    /* the block device of the bh layer, its lock with the extents */
    Vcb->bd.bd_dev = Vcb->RealDevice;
    Vcb->bd.bd_geo = Vcb->DiskGeometry;
    Vcb->bd.bd_part = Vcb->PartitionInformation;
    Vcb->bd.bd_volume = Vcb->Volume;
    Vcb->bd.bd_priv = (void *) Vcb;
    memset(&Vcb->bd.bd_bh_root, 0, sizeof(struct rb_root));
    InitializeListHead(&Vcb->bd.bd_bh_free);
    KeInitializeSpinLock(&Vcb->bd.bd_bh_free_lock);
    ExInitializeResourceLite(&Vcb->bd.bd_bh_lock);
    KeInitializeEvent(&Vcb->bd.bd_bh_notify, NotificationEvent, TRUE);
    State->Extents = TRUE;

    Vcb->bd.bd_bh_cache = kmem_cache_create("bd_bh_buffer", Vcb->BlockSize, 0, 0, NULL);
    if (!Vcb->bd.bd_bh_cache) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- 5. sbi */

static NTSTATUS
Ext2InitializeSbInfo(IN PEXT2_VCB Vcb, IN PEXT2_SUPER_BLOCK sb)
{
    ULONG   features;
    int     i, has_huge_files;

    Vcb->SectorBits = Ext2Log2(SECTOR_SIZE);
    Vcb->sb.s_magic = sb->s_magic;
    Vcb->sb.s_bdev = &Vcb->bd;
    Vcb->sb.s_blocksize = BLOCK_SIZE;
    Vcb->sb.s_blocksize_bits = (unsigned char)BLOCK_BITS;
    Vcb->sb.s_priv = (void *) Vcb;
    Vcb->sb.s_fs_info = &Vcb->sbi;

    Vcb->sbi.s_es = sb;
    Vcb->sbi.s_blocks_per_group = sb->s_blocks_per_group;
    /* revision 0: the fixed values Linux uses, not the fields */
    Vcb->sbi.s_first_ino = le32_to_cpu(sb->s_rev_level) >= EXT4_DYNAMIC_REV ?
                           sb->s_first_ino : EXT4_GOOD_OLD_FIRST_INO;
    Vcb->sbi.s_desc_size = sb->s_desc_size;
    Vcb->sbi.s_clusters_per_group = sb->s_clusters_per_group;
    Vcb->sbi.s_inode_size = Vcb->InodeSize;

    /* Precompute checksum seed for all metadata */
    if (ext4_has_feature_csum_seed(&Vcb->sb))
        Vcb->sbi.s_csum_seed = sb->s_checksum_seed;
    else if (ext4_has_metadata_csum(&Vcb->sb) || ext4_has_feature_ea_inode(&Vcb->sb))
        Vcb->sbi.s_csum_seed = ext4_chksum(&Vcb->sbi, ~0U, sb->s_uuid, sizeof(sb->s_uuid));

    if (EXT3_HAS_INCOMPAT_FEATURE(&Vcb->sb, EXT4_FEATURE_INCOMPAT_64BIT)) {
        if (Vcb->sbi.s_desc_size < EXT4_MIN_DESC_SIZE_64BIT ||
                Vcb->sbi.s_desc_size > EXT4_MAX_DESC_SIZE ||
                !is_power_of_2(Vcb->sbi.s_desc_size)) {
            DEBUG(DL_ERR, ("EXT4-fs: unsupported descriptor size %lu\n", Vcb->sbi.s_desc_size));
            return STATUS_DISK_CORRUPT_ERROR;
        }
    } else {
        Vcb->sbi.s_desc_size = EXT4_MIN_DESC_SIZE;
    }

    Vcb->sbi.s_inodes_per_group = sb->s_inodes_per_group;
    if (EXT3_INODES_PER_GROUP(&Vcb->sb) == 0) {
        return STATUS_DISK_CORRUPT_ERROR;
    }
    Vcb->sbi.s_inodes_per_block = BLOCK_SIZE / Vcb->InodeSize;
    if (Vcb->sbi.s_inodes_per_block == 0) {
        return STATUS_DISK_CORRUPT_ERROR;
    }
    Vcb->sbi.s_itb_per_group = Vcb->sbi.s_inodes_per_group /
                               Vcb->sbi.s_inodes_per_block;

    Vcb->sbi.s_desc_per_block = BLOCK_SIZE / GROUP_DESC_SIZE;
    Vcb->sbi.s_desc_per_block_bits = ilog2(Vcb->sbi.s_desc_per_block);

    for (i = 0; i < 4; i++) {
        Vcb->sbi.s_hash_seed[i] = sb->s_hash_seed[i];
    }
    Vcb->sbi.s_def_hash_version = sb->s_def_hash_version;

    if (le32_to_cpu(sb->s_rev_level) == EXT3_GOOD_OLD_REV &&
            (EXT3_HAS_COMPAT_FEATURE(&Vcb->sb, ~0U) ||
             EXT3_HAS_RO_COMPAT_FEATURE(&Vcb->sb, ~0U) ||
             EXT3_HAS_INCOMPAT_FEATURE(&Vcb->sb, ~0U))) {
        printk(KERN_WARNING
               "EXT3-fs warning: feature flags set on rev 0 fs, "
               "running e2fsck is recommended\n");
    }

    /* unknown read-only-compatible features: written only if the
       registry says so (CanIWrite) */
    features = EXT3_HAS_RO_COMPAT_FEATURE(&Vcb->sb, ~EXT4_FEATURE_RO_COMPAT_SUPP);
    if (features) {
        printk(KERN_ERR "EXT3-fs: %s: unsupported optional features in this volume: (%x).\n",
               Vcb->sb.s_id, le32_to_cpu(features));
        if (!CanIWrite(Vcb)) {
            SetLongFlag(Vcb->Flags, VCB_READ_ONLY);
        }
    }

    has_huge_files = EXT3_HAS_RO_COMPAT_FEATURE(&Vcb->sb, EXT4_FEATURE_RO_COMPAT_HUGE_FILE);

    Vcb->sb.s_maxbytes = ext3_max_size(BLOCK_BITS, has_huge_files);
    Vcb->max_bitmap_bytes = ext3_max_bitmap_size(BLOCK_BITS,
                            has_huge_files);
    Vcb->max_bytes = ext3_max_size(BLOCK_BITS, has_huge_files);

    /* the largest file an indirect map can address: 12 direct, then 1, 2 and 3 levels of indirection */
    {
        ULONG  dwData[EXT2_BLOCK_TYPES] = {EXT2_NDIR_BLOCKS, 1, 1, 1};
        ULONG  Level;

        ASSERT(BLOCK_BITS == Ext2Log2(BLOCK_SIZE));

        Vcb->sbi.s_groups_count = (ULONG)((ext3_blocks_count(sb) - sb->s_first_data_block +
                                           sb->s_blocks_per_group - 1) / sb->s_blocks_per_group);

        Vcb->max_data_blocks = 0;
        for (Level = 0; Level < EXT2_BLOCK_TYPES; Level++) {
            if (BLOCK_BITS >= 12 && Level == (EXT2_BLOCK_TYPES - 1)) {
                dwData[Level] = 0x40000000;
            } else {
                dwData[Level] = dwData[Level] << ((BLOCK_BITS - 2) * Level);
            }
            Vcb->max_blocks_per_layer[Level] = dwData[Level];
            Vcb->max_data_blocks += Vcb->max_blocks_per_layer[Level];
        }
    }

    /* Geometry guarantees a nonempty volume; this ceiling cannot wrap. */
    Vcb->sbi.s_gdb_count = (Vcb->sbi.s_groups_count - 1) /
                           Vcb->sbi.s_desc_per_block + 1;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- 6. groups */

static NTSTATUS
Ext2LoadGroups(IN PEXT2_VCB Vcb, IN OUT PEXT2_MOUNT_STATE State)
{
    NTSTATUS Status;

    /* the group locks, sized from the groups and the processors */
    Status = Ext2InitializeGroupLocks(Vcb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    /* load every group descriptor block; they stay pinned for the life of the mount */
    if (!Ext2LoadGroup(Vcb)) {
        return STATUS_UNSUCCESSFUL;
    }
    State->GroupLoaded = TRUE;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- 7. journal */

static VOID
Ext2StartJournal(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb)
{
    /* query parameters from registry: the read-only decision must be
       known before the journal is opened */
    Ext2PerformRegistryVolumeParams(Vcb);

    /* multi-mount protection, ahead of anything written: a volume another
       node may hold is not written - and not replayed - unless the
       protocol gives it to us */
    if (EXT3_HAS_INCOMPAT_FEATURE(&Vcb->sb, EXT4_FEATURE_INCOMPAT_MMP) &&
        !IsVcbReadOnly(Vcb) && !NT_SUCCESS(Ext4MmpStart(Vcb))) {
        DbgPrint("ext4: MMP: the volume is not ours to write, mounting read-only\n");
        SetLongFlag(Vcb->Flags, VCB_READ_ONLY);
    }

    /* replay the journal and start logging, since it's ext3/ext4 */
    if (Vcb->IsExt3fs &&
        (!EXT3_HAS_INCOMPAT_FEATURE(&Vcb->sb, EXT4_FEATURE_INCOMPAT_MMP) || Vcb->Mmp)) {
        INT rc = Ext2RecoverJournal(IrpContext, Vcb);
        if (rc != 0 && !IsVcbReadOnly(Vcb)) {
            /* no usable journal (external, corrupt, failed replay or
               out of resources): never write unjournaled, like Linux */
            DbgPrint("ext4: journal unavailable (rc=%d), mounting read-only\n", rc);
            SetLongFlag(Vcb->Flags, VCB_READ_ONLY);
        }
        if (IsFlagOn(Vcb->Flags, VCB_JOURNAL_RECOVER)) {
            SetLongFlag(Vcb->Flags, VCB_READ_ONLY);
        }
    }

    /* the free totals live in the Vcb from here on (Ext2AdjustVcbStat),
       summed from the descriptors - which a replay may have changed */
    Vcb->FreeBlocks = (LONG64)ext4_count_free_blocks(&Vcb->sb);
    Vcb->FreeInodes = (LONG64)ext4_count_free_inodes(&Vcb->sb);
}

/* ---------------------------------------------------------------- 8. root */

static NTSTATUS
Ext2LoadRoot(IN PEXT2_VCB Vcb)
{
    UNICODE_STRING  RootNode;
    USHORT          Buffer[2];
    PEXT2_ICB       Icb;
    struct inode   *Inode;

    Buffer[0] = L'\\';
    Buffer[1] = 0;
    RootNode.Buffer = Buffer;
    RootNode.MaximumLength = RootNode.Length = sizeof(WCHAR);
    Vcb->McbTree = Ext2AllocateMcb(Vcb, &RootNode, NULL, FILE_ATTRIBUTE_DIRECTORY);
    if (!Vcb->McbTree) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (!Ext2AttachIcb(Vcb, Vcb->McbTree, EXT2_ROOT_INO)) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Vcb->sb.s_root = Ext2BuildEntry(Vcb, NULL, &RootNode);
    if (!Vcb->sb.s_root) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Vcb->sb.s_root->d_sb = &Vcb->sb;
    Vcb->sb.s_root->d_inode = Vcb->McbTree->Inode;
    Vcb->McbTree->de = Vcb->sb.s_root;

    Icb = Vcb->McbTree->Icb;
    Inode = Vcb->McbTree->Inode;
    if (!Ext2LoadInode(Vcb, Inode)) {
        return STATUS_CANT_WAIT;
    }
    SetLongFlag(Icb->Flags, ICB_INODE_LOADED);

    Icb->LastAccessTime = Ext2GetInodeTime(Inode->i_atime, Inode->i_atime_extra);
    Icb->LastWriteTime = Ext2GetInodeTime(Inode->i_mtime, Inode->i_mtime_extra);
    Icb->ChangeTime = Ext2GetInodeTime(Inode->i_ctime, Inode->i_ctime_extra);
    if (Inode->i_crtime)
        Icb->CreationTime = Ext2GetInodeTime(Inode->i_crtime, Inode->i_crtime_extra);
    else
        Icb->CreationTime = Ext2GetInodeTime(Inode->i_ctime, Inode->i_ctime_extra);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- the mount */

/* undo what a failed mount set up, in the reverse order */
static VOID
Ext2UndoMount(IN PEXT2_VCB Vcb, IN PEXT2_MOUNT_STATE State)
{
    if (Vcb->McbTree) {
        Ext2FreeMcb(Vcb, Vcb->McbTree);
        Vcb->McbTree = NULL;
    }

    if (State->InodeLookaside) {
        ExDeleteNPagedLookasideList(&(Vcb->InodeLookasideList));
    }

    if (Vcb->Journal) {
        Ext2JournalStop(Vcb, !IsVcbReadOnly(Vcb));
    }
    Ext4MmpStop(Vcb, !IsFlagOn(Vcb->Flags, VCB_DEVICE_REMOVED));

    if (State->Extents) {
        if (Vcb->bd.bd_bh_cache) {
            if (State->GroupLoaded)
                Ext2PutGroup(Vcb);
            /* every metadata block read so far is pinned in the volume
               stream, released or not: the cache map below cannot go while
               one is (bugcheck 0x34, CcDeleteBcbs) */
            Ext2DrainBH(Vcb);
            kmem_cache_destroy(Vcb->bd.bd_bh_cache);
            Vcb->bd.bd_bh_cache = NULL;
        }
        /* initialized with the extents: an ERESOURCE left on the system
           list in freed memory brings down the next mount (bugcheck 0x139
           in ExInitializeResourceLite) */
        ExDeleteResourceLite(&Vcb->bd.bd_bh_lock);
        Ext4RunMapDestroy(&(Vcb->Extents));
    }

    if (Vcb->Volume) {
        if (State->VolumeCache && Vcb->Volume->PrivateCacheMap) {
            Ext2SyncUninitializeCacheMap(Vcb->Volume);
        }
        /* as in Ext2TearDownStream: the data section the reads so far
           created references the stream file object, and that one the
           device below. Left to the memory manager, every failed mount
           pinned the device - and the driver, which could not unload
           until a reboot. */
        MmForceSectionClosed(&Vcb->SectionObject, TRUE);
        Ext2ReleaseStream(Vcb, Vcb->Volume);
        Vcb->Volume = NULL;
    }

    Ext2JournalDestroy(Vcb);

    if (State->NotifySync) {
        FsRtlNotifyUninitializeSync(&Vcb->NotifySync);
    }

    Ext2DestroyNameCache(Vcb);
    Ext2DestroyGroupLocks(Vcb);

    if (State->Resources) {
        ExDeleteResourceLite(&Vcb->FcbLock);
        ExDeleteResourceLite(&Vcb->SuperLock);
        ExDeleteResourceLite(&Vcb->sbi.s_gd_lock);
        ExDeleteResourceLite(&Vcb->MainResource);
        ExDeleteResourceLite(&Vcb->PagingIoResource);
    }
}

NTSTATUS
Ext2InitializeVcb( IN PEXT2_IRP_CONTEXT IrpContext,
                   IN PEXT2_VCB         Vcb,
                   IN PEXT2_SUPER_BLOCK sb,
                   IN PDEVICE_OBJECT TargetDevice,
                   IN PDEVICE_OBJECT VolumeDevice,
                   IN PVPB Vpb )
{
    EXT2_MOUNT_STATE    State = { 0 };
    NTSTATUS            Status = STATUS_DEVICE_NOT_READY;

    __try {

        if (Vpb == NULL) {
            __leave;
        }

        Status = Ext2CheckSuperBlock(Vcb, sb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        Status = Ext2InitializeVcbObjects(Vcb, sb, TargetDevice, VolumeDevice, Vpb, &State);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        Status = Ext2ProbeDevice(IrpContext, Vcb, sb, TargetDevice, Vpb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        Status = Ext2OpenVolumeStream(Vcb, &State);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        Status = Ext2InitializeSbInfo(Vcb, sb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        Status = Ext2LoadGroups(Vcb, &State);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        Ext2StartJournal(IrpContext, Vcb);
        Status = Ext2LoadRoot(Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* check bitmap if user specifies it */
        if (IsFlagOn(Ext2Global->Flags, EXT2_CHECKING_BITMAP)) {
            Ext2CheckBitmapConsistency(IrpContext, Vcb);
        }

        /* get anything done, then refer target device */
        ObReferenceObject(Vcb->TargetDeviceObject);
        SetLongFlag(Vcb->Flags, VCB_INITIALIZED);

    } __finally {

        if (!NT_SUCCESS(Status)) {
            DbgPrint("ext4: mount of an ext volume failed (%xh)\n", Status);
            Ext2UndoMount(Vcb, &State);
        }
    }

    return Status;
}
