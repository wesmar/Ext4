/**
 * vcb.c - VCB construction and destruction, volume list, bitmap sanity checks.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "../core/core_internal.h"

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

#define is_power_of_2(x)        ((x) != 0 && (((x) & ((x) - 1)) == 0))

/* the longest the teardown waits for the metadata stream's last close (a
   cache manager work item drops it), in 100 ns units: 5 s */
#define EXT2_STREAM_CLOSE_WAIT  (5LL * 1000 * 1000 * 10)

/*
 * The last reference of ours to the metadata stream (its cache map gone,
 * its section closed). It is not always the last one: the cache manager
 * takes one of its own when the first cache map of a device is set up
 * (CcGetDeviceGuidAsync, a worker item) and drops it when that item has
 * run - after a short mount (a volume refused right away), only after the
 * Vcb was gone. The stream's close then came to a freed Vcb: its
 * resources, and the per-stream contexts Filter Manager looks up through
 * FsContext. So the stream no longer points at the Vcb (FsContext2 only
 * marks it as ours for Ext2Close), and, when its close is to come to this
 * volume, the Vcb stays until it has.
 */
static VOID
Ext2ReleaseStream(IN PEXT2_VCB Vcb, IN PFILE_OBJECT Stream)
{
    BOOLEAN       Ours = Stream->Vpb != NULL && Stream->Vpb->DeviceObject == Vcb->DeviceObject;
    LARGE_INTEGER Wait;

    Stream->FsContext = NULL;
    Stream->FsContext2 = Vcb;
    ObDereferenceObject(Stream);
    if (Ours) {
        Wait.QuadPart = -(LONGLONG)EXT2_STREAM_CLOSE_WAIT;
        if (KeWaitForSingleObject(&Vcb->StreamClosed, Executive, KernelMode,
                                  FALSE, &Wait) == STATUS_TIMEOUT) {
            DbgPrint("ext4: the metadata stream of %p is not closed yet\n", Vcb);
        }
    }
}

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2CheckBitmapConsistency)
#pragma alloc_text(PAGE, Ext2CheckSetBlock)
#pragma alloc_text(PAGE, Ext2InitializeVcb)
#pragma alloc_text(PAGE, Ext2TearDownStream)
#pragma alloc_text(PAGE, Ext2DestroyVcb)
#pragma alloc_text(PAGE, Ext2SyncUninitializeCacheMap)
#endif

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

    group = (ULONG)(Block - EXT2_FIRST_DATA_BLOCK) / BLOCKS_PER_GROUP;
    dwBlk = (ULONG)(Block - EXT2_FIRST_DATA_BLOCK) % BLOCKS_PER_GROUP;

    gd = ext4_get_group_desc(&Vcb->sb, group, &gb);
    if (!gd) {
        return FALSE;
    }
    bh = sb_getblk(&Vcb->sb, ext4_block_bitmap(&Vcb->sb, gd));

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
        DbgBreak();
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

/* Ext2Global->Resource should be already acquired */
VOID
Ext2InsertVcb(PEXT2_VCB Vcb)
{
    InsertTailList(&(Ext2Global->VcbList), &Vcb->Next);
}

/* Ext2Global->Resource should be already acquired. A dismounted volume
   moves to the dismounting list until Ext2DestroyVcb: its cached Fcbs
   still reference it and the Fcb reaper has to see it to drain them. */
VOID
Ext2RemoveVcb(PEXT2_VCB Vcb)
{
    RemoveEntryList(&Vcb->Next);
    InsertTailList(&(Ext2Global->DismountingVcbList), &Vcb->Next);
}

/* the counterpart, from Ext2DestroyVcb: off whichever list it is on */
VOID
Ext2UnlinkVcb(PEXT2_VCB Vcb)
{
    ExAcquireResourceExclusiveLite(&Ext2Global->Resource, TRUE);
    RemoveEntryList(&Vcb->Next);
    InitializeListHead(&Vcb->Next);
    ExReleaseResourceLite(&Ext2Global->Resource);
}

NTSTATUS
Ext2InitializeLabel(
    IN PEXT2_VCB            Vcb,
    IN PEXT2_SUPER_BLOCK    Sb
)
{
    NTSTATUS            status;

    USHORT              Length;
    UNICODE_STRING      Label;
    OEM_STRING          OemName;

    Label.MaximumLength = 16 * sizeof(WCHAR);
    Label.Length    = 0;
    Label.Buffer    = Vcb->Vpb->VolumeLabel;
    Vcb->Vpb->VolumeLabelLength = 0;
    RtlZeroMemory(Label.Buffer, Label.MaximumLength);

    Length = 16;
    while (  (Length > 0) &&
             ((Sb->s_volume_name[Length -1]  == 0x00) ||
              (Sb->s_volume_name[Length - 1] == 0x20)  )
          ) {
        Length--;
    }

    if (Length == 0) {
        return STATUS_SUCCESS;
    }

    OemName.Buffer =  Sb->s_volume_name;
    OemName.MaximumLength = 16;
    OemName.Length = Length;

    status = Ext2OEMToUnicode(Vcb, &Label, &OemName);
    if (NT_SUCCESS(status)) {
        Vcb->Vpb->VolumeLabelLength = Label.Length;
    }

    return status;
}

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
    ULONG       bs, isize, ipg, bpg, first, groups;
    ULONGLONG   blocks;
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
    groups = (ULONG)((blocks - first - 1) / bpg + 1);
    if ((ULONGLONG)groups * ipg != le32_to_cpu(sb->s_inodes_count)) {
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

NTSTATUS
Ext2InitializeVcb( IN PEXT2_IRP_CONTEXT IrpContext,
                   IN PEXT2_VCB         Vcb,
                   IN PEXT2_SUPER_BLOCK sb,
                   IN PDEVICE_OBJECT TargetDevice,
                   IN PDEVICE_OBJECT VolumeDevice,
                   IN PVPB Vpb )
{
    NTSTATUS                    Status = STATUS_UNRECOGNIZED_VOLUME;
    ULONG                       IoctlSize;
    LONGLONG                    DiskSize;
    LONGLONG                    PartSize;
    UNICODE_STRING              RootNode;
    USHORT                      Buffer[2];
    ULONG                       ChangeCount = 0, features;
    CC_FILE_SIZES               FileSizes;
    int                         i, has_huge_files;

    BOOLEAN                     VcbResourceInitialized = FALSE;
    BOOLEAN                     NotifySyncInitialized = FALSE;
    BOOLEAN                     ExtentsInitialized = FALSE;
    BOOLEAN                     InodeLookasideInitialized = FALSE;
    BOOLEAN                     GroupLoaded = FALSE;
    BOOLEAN                     VolumeCacheInitialized = FALSE;
    BOOLEAN                     IcbAttached = FALSE;

    __try {

        if (Vpb == NULL) {
            Status = STATUS_DEVICE_NOT_READY;
            __leave;
        }

        /* nothing is computed from a superblock with impossible geometry */
        Status = Ext4CheckGeometry(sb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* Reject mounting volume if we encounter unsupported incompat features */
        if (FlagOn(sb->s_feature_incompat,
                   ~(EXT4_FEATURE_INCOMPAT_SUPP | EXT4_FEATURE_INCOMPAT_READ_ONLY))) {
            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
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

        /* The block mapping interfaces of this driver carry physical block
           numbers in 32 bits. A volume with more blocks (a 64bit file
           system above 16 TiB with 4 KiB blocks) would be read and written
           at wrong places, so it is not mounted at all. Every narrowing of
           a block number to 32 bits relies on this check. */
        if (ext3_blocks_count(sb) > MAXULONG) {
            DbgPrint("ext4: %I64u blocks: volumes above 2^32 blocks are not supported\n",
                     (ULONGLONG)ext3_blocks_count(sb));
            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
        }

        /* check block size */
        Vcb->BlockSize  = (EXT2_MIN_BLOCK_SIZE << sb->s_log_block_size);
        /* we cannot handle volume with block size bigger than 64k */
        if (Vcb->BlockSize > EXT2_MAX_USER_BLKSIZE) {
            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
        }

        if (Vcb->BlockSize >= PAGE_SIZE) {
            Vcb->IoUnitBits = PAGE_SHIFT;
            Vcb->IoUnitSize = PAGE_SIZE;
        } else {
            Vcb->IoUnitSize = Vcb->BlockSize;
            Vcb->IoUnitBits = Ext2Log2(Vcb->BlockSize);
        }

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
        ExInitializeResourceLite(&Vcb->MetaInode);
        ExInitializeResourceLite(&Vcb->MetaBlock);
        ExInitializeResourceLite(&Vcb->McbLock);
        ExInitializeResourceLite(&Vcb->FcbLock);
        ExInitializeResourceLite(&Vcb->sbi.s_gd_lock);
        ExInitializeFastMutex(&Vcb->Mutex);
        FsRtlSetupAdvancedHeader(&Vcb->Header,  &Vcb->Mutex);
        VcbResourceInitialized = TRUE;

        /* initialize Fcb list head */
        InitializeListHead(&Vcb->FcbList);

        /* initialize Mcb list head and the name hash */
        InitializeListHead(&(Vcb->McbList));
        Vcb->McbHash = Ext2AllocatePool(NonPagedPool,
                           sizeof(LIST_ENTRY) * EXT2_MCB_HASH_BUCKETS, TAG_VPB);
        if (Vcb->McbHash == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }
        {
            ULONG Bucket;
            for (Bucket = 0; Bucket < EXT2_MCB_HASH_BUCKETS; Bucket++) {
                InitializeListHead(&Vcb->McbHash[Bucket]);
            }
        }

        /* initialize the inode node table */
        Ext2InitializeIcbTable(Vcb);

        /* initialize directory notify list */
        InitializeListHead(&Vcb->NotifyList);
        FsRtlNotifyInitializeSync(&Vcb->NotifySync);
        NotifySyncInitialized = TRUE;

        /* superblock checking */
        Vcb->SuperBlock = sb;

        /* initialize Vpb and Label */
        Vcb->DeviceObject = VolumeDevice;
        Vcb->TargetDeviceObject = TargetDevice;
        Vcb->Vpb = Vpb;
        Vcb->RealDevice = Vpb->RealDevice;
        Vpb->DeviceObject = VolumeDevice;

        /* set inode size */
        /* revision 0 has no inode size field (Linux ignores what is there) */
        if (le32_to_cpu(sb->s_rev_level) >= EXT4_DYNAMIC_REV) {
            Vcb->InodeSize = (ULONG)le16_to_cpu(sb->s_inode_size);
        } else {
            Vcb->InodeSize = EXT2_GOOD_OLD_INODE_SIZE;
        }

        /* initialize inode lookaside list */
        ExInitializeNPagedLookasideList(&(Vcb->InodeLookasideList),
                                        NULL, NULL, 0, Vcb->InodeSize,
                                        'SNIE', 0);

        InodeLookasideInitialized = TRUE;

        /* initialize label in Vpb */
        Status = Ext2InitializeLabel(Vcb, sb);
        if (!NT_SUCCESS(Status)) {
            DbgBreak();
        }

        /* check device characteristics flags */
        if (IsFlagOn(Vpb->RealDevice->Characteristics, FILE_REMOVABLE_MEDIA)) {
            SetLongFlag(Vcb->Flags, VCB_REMOVABLE_MEDIA);
        }

        if (IsFlagOn(Vpb->RealDevice->Characteristics, FILE_FLOPPY_DISKETTE)) {
            SetLongFlag(Vcb->Flags, VCB_FLOPPY_DISK);
        }

        if (IsFlagOn(Vpb->RealDevice->Characteristics, FILE_READ_ONLY_DEVICE)) {
            SetLongFlag(Vcb->Flags, VCB_WRITE_PROTECTED);
        }

        if (IsFlagOn(TargetDevice->Characteristics, FILE_READ_ONLY_DEVICE)) {
            SetLongFlag(Vcb->Flags, VCB_WRITE_PROTECTED);
        }

        /* verify device is writable ? */
        if (Ext2IsMediaWriteProtected(IrpContext, TargetDevice)) {
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
        Status = Ext2DiskIoControl(
                     TargetDevice,
                     IOCTL_DISK_GET_PARTITION_INFO,
                     NULL,
                     0,
                     &Vcb->PartitionInformation,
                     &IoctlSize );
        if (NT_SUCCESS(Status)) {
            PartSize = Vcb->PartitionInformation.PartitionLength.QuadPart;
        } else {
            Vcb->PartitionInformation.StartingOffset.QuadPart = 0;
            Vcb->PartitionInformation.PartitionLength.QuadPart = DiskSize;
            PartSize = DiskSize;
            Status = STATUS_SUCCESS;
        }
        Vcb->Header.AllocationSize.QuadPart =
            Vcb->Header.FileSize.QuadPart = PartSize;

        /* a file system larger than its partition (a truncated image, a
           corrupt block count): Linux refuses it too - every block past
           the end would be read from, or written to, what is not ours */
        if ((ULONGLONG)ext3_blocks_count(sb) * Vcb->BlockSize > (ULONGLONG)PartSize) {
            DbgPrint("ext4: %I64u blocks of %u bytes do not fit the partition (%I64d bytes)\n",
                     (ULONGLONG)ext3_blocks_count(sb), Vcb->BlockSize, PartSize);
            Status = STATUS_DISK_CORRUPT_ERROR;
            __leave;
        }

        Vcb->Header.ValidDataLength.QuadPart =
            Vcb->Header.FileSize.QuadPart;

        /* verify count */
        IoctlSize = sizeof(ULONG);
        Status = Ext2DiskIoControl(
                     TargetDevice,
                     IOCTL_DISK_CHECK_VERIFY,
                     NULL,
                     0,
                     &ChangeCount,
                     &IoctlSize );

        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        Vcb->ChangeCount = ChangeCount;

        /* create the stream object for ext2 volume */
        KeInitializeEvent(&Vcb->StreamClosed, NotificationEvent, FALSE);
        Vcb->Volume = IoCreateStreamFileObject(NULL, Vcb->Vpb->RealDevice);
        if (!Vcb->Volume) {
            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
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
           If it does, the cache map is only partially set up and the cleanup
           path below must not call CcUninitializeCacheMap on it, or the Cache
           Manager bugchecks (0x34 CACHE_MANAGER). Guard it with an explicit
           "initialized" flag, matching the idiom used elsewhere in this
           function. Fixes the mount-failure BSOD reported in issues #71/#54. */
        __try {
            CcInitializeCacheMap( Vcb->Volume,
                                  &FileSizes,
                                  TRUE,
                                  &(Ext2Global->CacheManagerNoOpCallbacks),
                                  Vcb );
            VolumeCacheInitialized = TRUE;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
        if (!VolumeCacheInitialized) {
            __leave;
        }

        /* initialize disk block LargetMcb and entry Mcb,
           it will raise an expcetion if failed */
        __try {
            FsRtlInitializeLargeMcb(&(Vcb->Extents), PagedPool);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            DbgBreak();
        }
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        ExtentsInitialized = TRUE;

        /* set block device */
        Vcb->bd.bd_dev = Vcb->RealDevice;
        Vcb->bd.bd_geo = Vcb->DiskGeometry;
        Vcb->bd.bd_part = Vcb->PartitionInformation;
        Vcb->bd.bd_volume = Vcb->Volume;
        Vcb->bd.bd_priv = (void *) Vcb;
        memset(&Vcb->bd.bd_bh_root, 0, sizeof(struct rb_root));
        InitializeListHead(&Vcb->bd.bd_bh_free);
        ExInitializeResourceLite(&Vcb->bd.bd_bh_lock);
        KeInitializeEvent(&Vcb->bd.bd_bh_notify,
                           NotificationEvent, TRUE);
        Vcb->bd.bd_bh_cache = kmem_cache_create("bd_bh_buffer",
                                                Vcb->BlockSize, 0, 0, NULL);
        if (!Vcb->bd.bd_bh_cache) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

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
                Status = STATUS_DISK_CORRUPT_ERROR;
                __leave;
            }
        } else {
            Vcb->sbi.s_desc_size = EXT4_MIN_DESC_SIZE;
        }

        Vcb->sbi.s_blocks_per_group = sb->s_blocks_per_group;
        Vcb->sbi.s_inodes_per_group = sb->s_inodes_per_group;
        if (EXT3_INODES_PER_GROUP(&Vcb->sb) == 0) {
            Status = STATUS_DISK_CORRUPT_ERROR;
            __leave;
        }
        Vcb->sbi.s_inodes_per_block = BLOCK_SIZE / Vcb->InodeSize;
        if (Vcb->sbi.s_inodes_per_block == 0) {
            Status = STATUS_DISK_CORRUPT_ERROR;
            __leave;
        }
        Vcb->sbi.s_itb_per_group = Vcb->sbi.s_inodes_per_group /
                                   Vcb->sbi.s_inodes_per_block;

        Vcb->sbi.s_desc_per_block = BLOCK_SIZE / GROUP_DESC_SIZE;
        Vcb->sbi.s_desc_per_block_bits = ilog2(Vcb->sbi.s_desc_per_block);

        for (i=0; i < 4; i++) {
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

        /*
         * Check feature flags regardless of the revision level, since we
         * previously didn't change the revision level when setting the flags,
         * so there is a chance incompat flags are set on a rev 0 filesystem.
         */
        features = EXT3_HAS_INCOMPAT_FEATURE(&Vcb->sb,
                   ~(EXT4_FEATURE_INCOMPAT_SUPP | EXT4_FEATURE_INCOMPAT_READ_ONLY));
        if (features) {
            printk(KERN_ERR "EXT3-fs: %s: couldn't mount because of "
                   "unsupported optional features (%x).\n",
                   Vcb->sb.s_id, le32_to_cpu(features));
            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
        }

        features = EXT3_HAS_RO_COMPAT_FEATURE(&Vcb->sb, ~EXT4_FEATURE_RO_COMPAT_SUPP);
        if (features) {
            printk(KERN_ERR "EXT3-fs: %s: unsupported optional features in this volume: (%x).\n",
                   Vcb->sb.s_id, le32_to_cpu(features));
            if (CanIWrite(Vcb)) {
            } else {
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

        Vcb->sbi.s_gdb_count = (Vcb->sbi.s_groups_count + Vcb->sbi.s_desc_per_block - 1) /
                               Vcb->sbi.s_desc_per_block;
        /* load every group descriptor block; they stay pinned for the life of the mount */
        if (!Ext2LoadGroup(Vcb)) {
            Status = STATUS_UNSUCCESSFUL;
            __leave;
        }
        GroupLoaded = TRUE;

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

        /* Now allocating the mcb for root ... */
        Buffer[0] = L'\\';
        Buffer[1] = 0;
        RootNode.Buffer = Buffer;
        RootNode.MaximumLength = RootNode.Length = 2;
        Vcb->McbTree = Ext2AllocateMcb(
                           Vcb, &RootNode, NULL,
                           FILE_ATTRIBUTE_DIRECTORY
                       );
        if (!Vcb->McbTree) {
            DbgBreak();
            Status = STATUS_UNSUCCESSFUL;
            __leave;
        }

        ExAcquireResourceExclusiveLite(&Vcb->McbLock, TRUE);
        IcbAttached = Ext2AttachIcb(Vcb, Vcb->McbTree, EXT2_ROOT_INO);
        ExReleaseResourceLite(&Vcb->McbLock);
        if (!IcbAttached) {
            DbgBreak();
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        Vcb->sb.s_root = Ext2BuildEntry(Vcb, NULL, &RootNode);
        if (!Vcb->sb.s_root) {
            DbgBreak();
            Status = STATUS_UNSUCCESSFUL;
            __leave;
        }
        Vcb->sb.s_root->d_sb = &Vcb->sb;
        Vcb->sb.s_root->d_inode = Vcb->McbTree->Inode;
        Vcb->McbTree->de = Vcb->sb.s_root;

        /* load root inode */
        if (!Ext2LoadInode(Vcb, Vcb->McbTree->Inode)) {
            DbgBreak();
            Status = STATUS_CANT_WAIT;
            __leave;
        }
        SetLongFlag(Vcb->McbTree->Icb->Flags, ICB_INODE_LOADED);

        /* initializeroot node */
        Vcb->McbTree->Icb->LastAccessTime = Ext2GetInodeTime(Vcb->McbTree->Inode->i_atime, Vcb->McbTree->Inode->i_atime_extra);
        Vcb->McbTree->Icb->LastWriteTime = Ext2GetInodeTime(Vcb->McbTree->Inode->i_mtime, Vcb->McbTree->Inode->i_mtime_extra);
        Vcb->McbTree->Icb->ChangeTime = Ext2GetInodeTime(Vcb->McbTree->Inode->i_ctime, Vcb->McbTree->Inode->i_ctime_extra);
        if (Vcb->McbTree->Inode->i_crtime)
            Vcb->McbTree->Icb->CreationTime = Ext2GetInodeTime(Vcb->McbTree->Inode->i_crtime, Vcb->McbTree->Inode->i_crtime_extra);
        else
            Vcb->McbTree->Icb->CreationTime = Ext2GetInodeTime(Vcb->McbTree->Inode->i_ctime, Vcb->McbTree->Inode->i_ctime_extra);

        /* check bitmap if user specifies it */
        if (IsFlagOn(Ext2Global->Flags, EXT2_CHECKING_BITMAP)) {
            Ext2CheckBitmapConsistency(IrpContext, Vcb);
        }

        /* get anything doen, then refer target device */
        ObReferenceObject(Vcb->TargetDeviceObject);

        SetLongFlag(Vcb->Flags, VCB_INITIALIZED);

    } __finally {

        if (!NT_SUCCESS(Status)) {

            DbgPrint("ext4: mount of an ext volume failed (%xh)\n", Status);

            if (Vcb->McbTree) {
                Ext2FreeMcb(Vcb, Vcb->McbTree);
            }

            if (InodeLookasideInitialized) {
                ExDeleteNPagedLookasideList(&(Vcb->InodeLookasideList));
            }

            if (Vcb->Journal) {
                Ext2JournalDestroy(Vcb);
            }
            Ext4MmpStop(Vcb, !IsFlagOn(Vcb->Flags, VCB_DEVICE_REMOVED));

            if (ExtentsInitialized) {
                if (Vcb->bd.bd_bh_cache) {
                    if (GroupLoaded)
                        Ext2PutGroup(Vcb);
                    /* every metadata block read so far is pinned in the
                       volume stream, released or not: the cache map below
                       cannot go while one is (bugcheck 0x34, CcDeleteBcbs) */
                    Ext2DrainBH(Vcb);
                    kmem_cache_destroy(Vcb->bd.bd_bh_cache);
                    Vcb->bd.bd_bh_cache = NULL;
                }
                /* initialized with the extents, right above: an ERESOURCE
                   left on the system list in freed memory brings down the
                   next mount (bugcheck 0x139 in ExInitializeResourceLite) */
                ExDeleteResourceLite(&Vcb->bd.bd_bh_lock);
                FsRtlUninitializeLargeMcb(&(Vcb->Extents));
            }

            if (Vcb->Volume) {
                if (VolumeCacheInitialized && Vcb->Volume->PrivateCacheMap) {
                    Ext2SyncUninitializeCacheMap(Vcb->Volume);
                }
                /* as in Ext2TearDownStream: the data section the reads so far
                   created references the stream file object, and that one
                   the device below. Left to the memory manager, every failed
                   mount pinned the device - and the driver, which could not
                   unload until a reboot. */
                MmForceSectionClosed(&Vcb->SectionObject, TRUE);
                Ext2ReleaseStream(Vcb, Vcb->Volume);
                Vcb->Volume = NULL;
            }

            if (NotifySyncInitialized) {
                FsRtlNotifyUninitializeSync(&Vcb->NotifySync);
            }

            if (Vcb->McbHash) {
                Ext2FreePool(Vcb->McbHash, TAG_VPB);
                Vcb->McbHash = NULL;
            }

            if (VcbResourceInitialized) {
                ExDeleteResourceLite(&Vcb->FcbLock);
                ExDeleteResourceLite(&Vcb->McbLock);
                ExDeleteResourceLite(&Vcb->MetaInode);
                ExDeleteResourceLite(&Vcb->MetaBlock);
                ExDeleteResourceLite(&Vcb->sbi.s_gd_lock);
                ExDeleteResourceLite(&Vcb->MainResource);
                ExDeleteResourceLite(&Vcb->PagingIoResource);
            }
        }
    }

    return Status;
}

VOID
Ext2TearDownStream(IN PEXT2_VCB Vcb)
{
    PFILE_OBJECT    Stream = Vcb->Volume;
    IO_STATUS_BLOCK IoStatus;

    ASSERT(Vcb != NULL);
    ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
           (Vcb->Identifier.Size == sizeof(EXT2_VCB)));

    /* complete all pending directory change notifications on this volume so
       monitoring applications (Total Commander, FileSystemWatcher) can close handles */
    if (Vcb->NotifySync != NULL) {
        FsRtlNotifyCleanup(Vcb->NotifySync, &Vcb->NotifyList, NULL);
    }

    if (Stream) {

        /* commit, checkpoint and mark the fs clean while the cache and the
           journal inode are still usable */
        if (Vcb->Journal) {
            Ext2JournalDestroy(Vcb);
        }
        /* the last write is done: release the volume to other nodes */
        Ext4MmpStop(Vcb, !IsFlagOn(Vcb->Flags, VCB_DEVICE_REMOVED));

        /* every buffer head pins its block in this stream; the cache map
           cannot go while one is left (bugcheck 0x34) */
        Ext2DrainBH(Vcb);

        Vcb->Volume = NULL;

        if (IsFlagOn(Stream->Flags, FO_FILE_MODIFIED)) {
            CcFlushCache(&(Vcb->SectionObject), NULL, 0, &IoStatus);
            ClearFlag(Stream->Flags, FO_FILE_MODIFIED);
        }

        if (Stream->PrivateCacheMap) {
            Ext2SyncUninitializeCacheMap(Stream);
        }

        /* the stream's data section references the stream file object, and
           that file object holds the Vpb reference that keeps the volume
           device alive: delete the section with the cache map instead of
           whenever the memory manager trims it */
        MmForceSectionClosed(&Vcb->SectionObject, TRUE);

        Ext2ReleaseStream(Vcb, Stream);
        Stream = NULL;
    }
}

VOID
Ext2DestroyVcb (IN PEXT2_VCB Vcb)
{
    ASSERT(Vcb != NULL);
    ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
           (Vcb->Identifier.Size == sizeof(EXT2_VCB)));

    DEBUG(DL_FUN, ("Ext2DestroyVcb ...\n"));

    /* off the mounted or the dismounting list, whichever it is on */
    Ext2UnlinkVcb(Vcb);

    /* the update thread may not outlive the Vcb it writes for */
    Ext4MmpStop(Vcb, !IsFlagOn(Vcb->Flags, VCB_DEVICE_REMOVED));

    if (Vcb->Volume) {
        Ext2TearDownStream(Vcb);
    }
    ASSERT(NULL == Vcb->Volume);

    FsRtlNotifyUninitializeSync(&Vcb->NotifySync);
    if (Vcb->Extents.GuardedMutex != NULL) {
        Ext2ListExtents(&Vcb->Extents);
        FsRtlUninitializeLargeMcb(&(Vcb->Extents));
        Vcb->Extents.GuardedMutex = NULL;
    }

    Ext2CleanupAllMcbs(Vcb);

    Ext2DropBH(Vcb);
    /* the group descriptor table itself (its blocks went with the BHs):
       left behind, every mount lost it */
    Ext2PutGroup(Vcb);

    if (Vcb->bd.bd_bh_cache) {
        kmem_cache_destroy(Vcb->bd.bd_bh_cache);
        Vcb->bd.bd_bh_cache = NULL;
    }
    ExDeleteResourceLite(&Vcb->bd.bd_bh_lock);

    if (Vcb->SuperBlock) {
        Ext2FreePool(Vcb->SuperBlock, EXT2_SB_MAGIC);
        Vcb->SuperBlock = NULL;
    }

    if (IsFlagOn(Vcb->Flags, VCB_NEW_VPB)) {
        PVPB    Free = Vcb->Vpb2;

        ASSERT(Vcb->Vpb2 != NULL);
        /* A forced dismount gave the device a VPB of ours and kept its own
           (Vpb2) for the file objects still open then. With all of them
           gone and ours unused since, the device gets its own back and
           ours is the one freed: an allocation of this driver must not
           outlive it (Driver Verifier counts it as leaked at unload). */
        /* Not now: another file system may be mounting the device with
           our VPB this very moment (nothing locks it against that), and
           freeing it under the mount brought the system down. The device
           keeps ours, as FastFAT leaves its own; the exchange back is
           made at unload, when this driver's allocations must be gone */
        if (Vcb->SwapVpb && !IsFlagOn(Vcb->Flags, VCB_DEVICE_REMOVED) &&
            Ext2DeferVpb(Vcb->Vpb2, Vcb->SwapVpb)) {
            Free = NULL;
        }
        if (Free) {
            DEBUG(DL_DBG, ("Ext2DestroyVcb: Vpb to be freed: %p\n", Free));
            Ext2FreePool(Free, TAG_VPB);
            DEC_MEM_COUNT(PS_VPB, Free, sizeof(VPB));
        }
        Vcb->Vpb2 = NULL;
        Vcb->SwapVpb = NULL;
    }

    ObDereferenceObject(Vcb->TargetDeviceObject);

    if (Vcb->MountDevLink.Buffer) {
        /* the device is gone or we are: the interface name is all that is
           left to free (its state went with the PDO or with the release) */
        RtlFreeUnicodeString(&Vcb->MountDevLink);
    }

    ExDeleteNPagedLookasideList(&(Vcb->InodeLookasideList));
    if (Vcb->McbHash) {
        Ext2FreePool(Vcb->McbHash, TAG_VPB);
        Vcb->McbHash = NULL;
    }
    ExDeleteResourceLite(&Vcb->FcbLock);
    ExDeleteResourceLite(&Vcb->McbLock);
    ExDeleteResourceLite(&Vcb->MetaInode);
    ExDeleteResourceLite(&Vcb->MetaBlock);
    ExDeleteResourceLite(&Vcb->sbi.s_gd_lock);
    ExDeleteResourceLite(&Vcb->PagingIoResource);
    ExDeleteResourceLite(&Vcb->MainResource);

    DEBUG(DL_DBG, ("Ext2DestroyVcb: DevObject=%p Vcb=%p\n", Vcb->DeviceObject, Vcb));
    IoDeleteDevice(Vcb->DeviceObject);
    DEC_MEM_COUNT(PS_VCB, Vcb->DeviceObject, sizeof(EXT2_VCB));

    /* one volume fewer stands between "sc stop" and DriverUnload */
    Ext2UnloadKick();
}

/* uninitialize cache map */

VOID
Ext2SyncUninitializeCacheMap (
    IN PFILE_OBJECT FileObject
)
{
    CACHE_UNINITIALIZE_EVENT UninitializeCompleteEvent;
    NTSTATUS WaitStatus;
    LARGE_INTEGER Ext2LargeZero = {0,0};

    KeInitializeEvent( &UninitializeCompleteEvent.Event,
                       SynchronizationEvent,
                       FALSE);

    CcUninitializeCacheMap( FileObject,
                            &Ext2LargeZero,
                            &UninitializeCompleteEvent );

    WaitStatus = KeWaitForSingleObject( &UninitializeCompleteEvent.Event,
                                        Executive,
                                        KernelMode,
                                        FALSE,
                                        NULL);

    ASSERT (NT_SUCCESS(WaitStatus));
}
