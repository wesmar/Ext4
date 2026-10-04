/**
 * ext4fs_objects.h - volume, file, name, inode and request control blocks.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4FS_OBJECTS_H_
#define _EXT4_EXT4FS_OBJECTS_H_

/* EXT2_VCB Volume Control Block

   Data that represents a mounted logical volume
   It is allocated as the device extension of the volume device object */
typedef struct _EXT2_VCB {

    /* Common header */
    EXT2_FCBVCB;

    /* Block groups: the block bitmap of group g changes under the
       BlockLock of GroupLocks[g & GroupLockMask], the inode bitmap under
       its InodeLock (inode before block when both), the shared descriptor
       is checksummed under its DescLock; the superblock - its counters,
       its checksum, its write - under SuperLock, taken inside a group lock,
       never around one (core\LockStripes.c) */
    PEXT2_GROUP_STRIPE          GroupLocks;
    PVOID                       GroupLockPool;
    ULONG                       GroupLockMask;
    ERESOURCE                   SuperLock;

    /* Every block of metadata of the volume as sorted, disjoint ranges:
       read-only after mount, so read without a lock (ext4\MetadataMap.c) */
    struct _EXT2_META_MAP      *MetaMap;

    /* Free block and inode totals, kept here and written to the superblock
       on flush (Ext2AdjustVcbStat, Ext2SyncSuperTotals) */
    volatile LONG64             FreeBlocks;
    volatile LONG64             FreeInodes;

    /* Blocks free on disk that the allocator must not reuse before the
       transaction that freed them commits, and the last transaction for
       which a commit was asked on their account (ext4\FreedBlocks.c) */
    volatile LONG64             FreedPending;
    volatile LONG               FreedCommitAsked;

    /* Symlink targets of the name cache: Mcb->Target and the link type
       bits change under it exclusively and are followed under it shared.
       Taken before any name stripe, never while one is held (core\NameCache.c). */
    ERESOURCE                   LinkLock;

    /* List of FCBs for open files on this volume */
    ERESOURCE                   FcbLock;
    LIST_ENTRY                  FcbList;
    LONG                        FcbCount;
    LONG                        FcbGone;    /* idle Fcbs of deleted files since the last reaper scan */

    /* The name cache (core\NameCache.c). The hash is keyed by parent and name;
       its chains are guarded by stripes, chain i by stripe i & StripeMask,
       sized from the processor count. McbList is the reaping order, under
       McbLruLock; McbReapLock lets one reaper at a time free names. */
    LONG                        NumOfMcb;
    LIST_ENTRY                  McbList;
    KSPIN_LOCK                  McbLruLock;
    FAST_MUTEX                  McbReapLock;
    PLIST_ENTRY                 McbHash;
    ULONG                       McbHashMask;
    PEXT2_LOCK_STRIPE           McbStripes;
    PVOID                       McbStripePool;  /* allocation behind McbStripes */
    ULONG                       McbStripeMask;

    /* Entry of Mcb Tree (Root Node) */
    PEXT2_MCB                   McbTree;

    /* Inode nodes (EXT2_ICB) hashed by inode number: every name of a
       hard-linked file resolves to the same node, so all of them share
       one struct inode, one block map and one Fcb (cache). */
    KSPIN_LOCK                  IcbLock;
    LIST_ENTRY                  IcbTable[EXT2_ICB_BUCKETS];
    ULONG                       NumOfIcb;

    /* Link list to Global */
    LIST_ENTRY                  Next;

    /* Section objects */
    SECTION_OBJECT_POINTERS     SectionObject;

    /* Dirty Mcbs of modifications for volume stream */
    EXT4_RUN_MAP                   Extents;


    /* Share Access for the file object */
    SHARE_ACCESS                ShareAccess;

    /* Incremented on IRP_MJ_CREATE, decremented on IRP_MJ_CLOSE
       for both files on this volume and open instances of the
       volume itself. */
    LONG                        ReferenceCount;     /* total ref count */
    LONG                        OpenHandleCount;    /* all handles */

    LONG                        OpenVolumeCount;    /* volume handle */

    /* Disk change count */
    ULONG                       ChangeCount;

    /* Pointer to the VPB in the target device object */
    PVPB                        Vpb;

    /* The FileObject of Volume used to lock the volume */
    PFILE_OBJECT                LockFile;

    /* List of IRPs pending on directory change notify requests */
    LIST_ENTRY                  NotifyList;

    /* Pointer to syncronization primitive for this list */
    PNOTIFY_SYNC                NotifySync;

    /* This volumes device object */
    PDEVICE_OBJECT              DeviceObject;

    /* The physical device object (the disk) */
    PDEVICE_OBJECT              TargetDeviceObject;

    /* The physical device object (the disk) */
    PDEVICE_OBJECT              RealDevice;

    /* Information about the physical device object */
    DISK_GEOMETRY               DiskGeometry;
    PARTITION_INFORMATION       PartitionInformation;

    BOOLEAN                     IsExt3fs;
    PEXT2_SUPER_BLOCK           SuperBlock;

    /* Block / Cluster size */
    ULONG                       BlockSize;

    /* Sector size in bits */
    ULONG                       SectorBits;

    /* Minimal i/o size: min(PageSize, BlockSize) */
    ULONGLONG                   IoUnitSize;

    /* Bits of aligned size */
    ULONG                       IoUnitBits;

    /* Inode size */
    ULONG                       InodeSize;

    /* Inode lookaside list */
    NPAGED_LOOKASIDE_LIST       InodeLookasideList;

    /* Flags for the volume */
    ULONG                       Flags;

    /* Streaming File Object */
    PFILE_OBJECT                Volume;
    KEVENT                      StreamClosed;   /* the stream's IRP_MJ_CLOSE has come */

    /* User specified codepage name per volume */
    struct {
        CHAR                    AnsiName[CODEPAGE_MAXLEN];
        struct nls_table *      PageTable;
    } Codepage;

    /* patterns to hiding files */
    BOOLEAN                     bHidingPrefix;
    CHAR                        sHidingPrefix[HIDINGPAT_LEN];
    BOOLEAN                     bHidingSuffix;
    CHAR                        sHidingSuffix[HIDINGPAT_LEN];

    /* User to impersanate */
    uid_t                       uid;
    gid_t                       gid;

    /* User to act as */
    uid_t                       euid;
    gid_t                       egid;

    /* mountpoint: symlink to DesDevices */
    UCHAR                       DrvLetter;

    /* the mounted-device interface DriveLetters.c registered on the volume's
       PDO so that the mount manager tracks a hidden volume */
    UNICODE_STRING              MountDevLink;

    struct block_device         bd;
    struct super_block          sb;
    struct ext3_sb_info         sbi;

    /* write-ahead journal engine (jbd2\commit.c), NULL when inactive */
    struct _EXT2_JOURNAL        *Journal;

    /* multi-mount protection while the volume is ours (volume\MultiMountProtection.c) */
    PVOID                       Mmp;

    /* device cache flushes (Ext2FlushDisk): the number the last one issued
       got, and the highest number of one that completed. A flush numbered
       above a mark taken after some writes completed made them durable. */
    volatile LONG64             FlushIssued;
    volatile LONG64             FlushCovered;

    /* Maximum file size in blocks ... */
    ULONG                       max_blocks_per_layer[EXT2_BLOCK_TYPES];
    ULONG                       max_data_blocks;
    loff_t                      max_bitmap_bytes;
    loff_t                      max_bytes;
} EXT2_VCB, *PEXT2_VCB;

/* Flags for EXT2_VCB */
#define VCB_INITIALIZED         0x00000001
#define VCB_VOLUME_LOCKED       0x00000002
#define VCB_MOUNTED             0x00000004
#define VCB_DISMOUNT_PENDING    0x00000008
#define VCB_NEW_VPB             0x00000010
#define VCB_BEING_CLOSED        0x00000020
#define VCB_USER_IDS            0x00000040  /* uid/gid specified by user */
#define VCB_USER_EIDS           0x00000080  /* euid/egid specified by user */
#define VCB_GD_LOADED           0x00000100  /* group desc loaded */
#define VCB_LETTER_ASSIGNED     0x00000200  /* DrvLetter was created by DriveLetters.c */
#define VCB_STREAM_TEARDOWN     0x00000400  /* one thread owns Ext2TearDownStream */
#define VCB_WRITE_TRANSITION    0x00000800  /* replay/start before accepting user writes */

#define VCB_BEING_DROPPED       0x00002000
#define VCB_FORCE_WRITING       0x00004000
#define VCB_DEVICE_REMOVED      0x00008000
#define VCB_JOURNAL_RECOVER     0x00080000
#define VCB_ARRIVAL_NOTIFIED    0x00800000
#define VCB_RO_COMPAT_READ_ONLY 0x01000000
#define VCB_READ_ONLY           0x08000000
#define VCB_WRITE_PROTECTED     0x10000000
#define VCB_FLOPPY_DISK         0x20000000
#define VCB_REMOVAL_PREVENTED   0x40000000
#define VCB_REMOVABLE_MEDIA     0x80000000


#define IsVcbInited(Vcb)   (IsFlagOn((Vcb)->Flags, VCB_INITIALIZED))
#define IsMounted(Vcb)     (IsFlagOn((Vcb)->Flags, VCB_MOUNTED))
#define IsDispending(Vcb)  (IsFlagOn((Vcb)->Flags, VCB_DISMOUNT_PENDING))
#define IsVcbReadOnly(Vcb) (IsFlagOn((Vcb)->Flags, VCB_READ_ONLY) ||    \
                            IsFlagOn((Vcb)->Flags, VCB_RO_COMPAT_READ_ONLY) ||    \
                            IsFlagOn((Vcb)->Flags, VCB_WRITE_PROTECTED))


#define IsExt3ForceWrite()   (IsFlagOn(Ext2Global->Flags, EXT3_FORCE_WRITING))
#define IsVcbForceWrite(Vcb) (IsFlagOn((Vcb)->Flags, VCB_FORCE_WRITING))
#define CanIWrite(Vcb)       (IsExt3ForceWrite() || (!IsVcbReadOnly(Vcb) && IsVcbForceWrite(Vcb)))
#define IsLazyWriter(Fcb)    ((Fcb)->LazyWriterThread == PsGetCurrentThread())
/* EXT2_FCB File Control Block

   Data that represents an open file
   There is a single instance of the FCB for every open file */
typedef struct _EXT2_FCB {

    /* Common header */
    EXT2_FCBVCB;

    /* List of FCBs for this volume */
    LIST_ENTRY                      Next;
    LARGE_INTEGER                   TsDrop; /* drop time */

    SECTION_OBJECT_POINTERS         SectionObject;

    /* Share Access for the file object */
    SHARE_ACCESS                    ShareAccess;

    /* List of byte-range locks for this file */
    FILE_LOCK                       FileLockAnchor;

    /* oplock information management structure */
    OPLOCK                          Oplock;

    /* Lazy writer thread context */
    PETHREAD                        LazyWriterThread;

    /* Incremented on IRP_MJ_CREATE, decremented on IRP_MJ_CLEANUP */
    LONG                            OpenHandleCount;

    /* Incremented on IRP_MJ_CREATE, decremented on IRP_MJ_CLOSE */
    LONG                            ReferenceCount;

    /* Incremented on IRP_MJ_CREATE, decremented on IRP_MJ_CLEANUP
       But only for Files with FO_NO_INTERMEDIATE_BUFFERING flag */
    LONG                            NonCachedOpenCount;
    LONG                            CcbCount;           /* file objects between create and close */
    LIST_ENTRY                      CcbList;            /* their Ccbs (Ccb->FcbLink), under MainResource */

    /* Flags for the FCB */
    ULONG                           Flags;

    /* Pointer to the inode */
    struct inode                   *Inode;

    /* Vcb */
    PEXT2_VCB                       Vcb;

    /* Mcb Node ... */
    PEXT2_MCB                       Mcb;

} EXT2_FCB, *PEXT2_FCB;

/* Flags for EXT2_FCB */

#define FCB_FROM_POOL               0x00000001
#define FCB_PAGE_FILE               0x00000002
#define FCB_FILE_MODIFIED           0x00000020

#define FCB_ALLOC_IN_CREATE         0x00000080
#define FCB_ALLOC_IN_WRITE          0x00000100
#define FCB_ALLOC_IN_SETINFO        0x00000200

#define FCB_DELETE_PENDING          0x80000000

/* Icb Node: the in-memory inode

   One per inode number on a mounted volume, shared by every Mcb (name)
   that refers to it - that is what makes hard links coherent: all names
   see one struct inode, one block map and one Fcb, hence one cache.
   Reached through Mcb->Icb; Mcb->Inode and Fcb->Inode point into it. */

/* the caseless names of a large directory (ext4\DirectoryNameCache.c) */
typedef struct _EXT4_DIR_NAMES EXT4_DIR_NAMES, *PEXT4_DIR_NAMES;

struct _EXT2_ICB {

    /* Identifier for this structure */
    EXT2_IDENTIFIER                 Identifier;

    /* Flags */
    ULONG                           Flags;

    /* Number of attached Mcbs (names, including deleted-but-open ones) */
    LONG                            Refercount;

    /* Link into Vcb->IcbTable[hash] */
    LIST_ENTRY                      Link;

    /* Attached Mcbs (Mcb->IcbLink); live names come first */
    LIST_ENTRY                      Names;

    /* The open-file object, at most one per inode */
    PEXT2_FCB                       Fcb;

    /* Extents zone */
    EXT4_RUN_MAP                       Extents;

    /* Time stamps */
    LARGE_INTEGER                   CreationTime;
    LARGE_INTEGER                   LastWriteTime;
    LARGE_INTEGER                   ChangeTime;
    LARGE_INTEGER                   LastAccessTime;

    /* A directory's names folded to upper case, as hashes: a miss in its
       index needs no scan for other spellings (ext4\DirectoryNameCache.c) */
    EX_PUSH_LOCK                    CaselessLock;
    PEXT4_DIR_NAMES                 Caseless;
    volatile LONG                   CaselessGeneration;

    /* A directory's namespace: held exclusively from "is the name free"
       to "the entry is in", by create and delete, and by every entry
       change (ext4\DirectoryOperations.c). Taken before the directory's Fcb resources
       and before the name stripes; lookups do not take it. Plain files
       leave it be. */
    ERESOURCE                       DirResource;

    /* A directory's entry blocks: exclusive while an entry change rewrites
       them, shared by a lookup reading them (Ext2ScanDir). The innermost
       directory lock: whoever holds it waits for no other lock of ours.
       For any inode it also makes loading it from disk a one-time step
       (Ext2InsertName): two names of one inode may be cached at once. */
    ERESOURCE                       EntryResource;

    struct inode                    Inode;
};

/* Flags for ICB */
#define ICB_ZONE_INITED             0x00000001
#define ICB_INODE_LOADED            0x00000002  /* Inode holds the on-disk contents */
#define ICB_UNHASHED                0x00000004  /* left Vcb->IcbTable (inode freed) */
#define ICB_FILE_DELETED            0x00000008  /* the last name went: inode is gone */
/* the file behind an Fcb no longer exists (as opposed to IsFileDeleted(Mcb):
   one of its names is gone, others may remain) */
#define IsInodeDeleted(Fcb)     IsFlagOn((Fcb)->Mcb->Icb->Flags, ICB_FILE_DELETED)

/* Mcb Node: a name in the directory tree */

struct _EXT2_MCB {

    /* Identifier for this structure */
    EXT2_IDENTIFIER                 Identifier;

    /* Flags */
    ULONG                           Flags;

    /* Name tree: the directory this name lives in (referenced by it) and
       our place in the volume's name hash (Vcb->McbHash, keyed by parent
       and upcased name). Both change under the name's stripe exclusively. */
    PEXT2_MCB                       Parent;
    LIST_ENTRY                      Hash;
    ULONG                           NameHash;

    /* Target Mcb of a symlink (Vcb->LinkLock) */
    PEXT2_MCB                       Target;

    /* Mcb Node Info */

    /* The inode this name refers to, and a shortcut into it */
    PEXT2_ICB                       Icb;
    struct inode                   *Inode;

    /* Link into Icb->Names */
    LIST_ENTRY                      IcbLink;

    /* Short name */
    UNICODE_STRING                  ShortName;

    /* Full name with path */
    UNICODE_STRING                  FullName;

    /* File attribute */
    ULONG                           FileAttr;

    /* reference count */
    LONG                            Refercount;

    /* List Link to Vcb->McbList (Vcb->McbLruLock) */
    LIST_ENTRY                      Link;

    struct dentry                  *de;
};

/* Flags for MCB */
#define MCB_FROM_POOL               0x00000001
#define MCB_VCB_LINK                0x00000002
#define MCB_ENTRY_TREE              0x00000004
#define MCB_FILE_DELETED            0x00000008
#define MCB_DELETE_PENDING          0x00000010  /* this name goes with the last handle */
#define MCB_ACCESSED                0x00000020  /* looked up since the reaper last saw it */

/* lock striping (core\LockStripes.c): stripes per processor, the largest
   power of two a count is rounded to, the pool tag of a set */
#define EXT4_STRIPES_PER_CPU        4
#define EXT4_POW2_MAX               0x80000000UL
#define EXT4_STRIPE_TAG             'TS4E'
/* name cache sizing (Ext2InitializeNameCache): names per hash chain at
   the high water mark, smallest table */
#define EXT4_NAMES_PER_CHAIN        2
#define EXT4_MIN_CHAINS             64
/* 2^64 / golden ratio: a multiply by it spreads pointer bits (Knuth) */
#define EXT4_FIBONACCI_64           0x9E3779B97F4A7C15ULL
/* a reaper pass looks at most at FACTOR * wanted + EXTRA names */
#define EXT4_REAP_LOOK_FACTOR       4
#define EXT4_REAP_LOOK_EXTRA        64
#define EXT4_LEAK_REPORT_MAX        8           /* names listed by Ext2CleanupAllMcbs */
#define Ext2McbHighWater()          (((ULONG)Ext2Global->MaxDepth) * 64)
#define Ext2McbLowWater()           (Ext2McbHighWater() * 3 / 4)

/* cached Fcbs of a volume: past the high water mark the oldest idle ones
   go whatever their age, down to the low water mark (CacheReaper.c) */
#define Ext2FcbHighWater()          (((ULONG)Ext2Global->MaxDepth) * 64)
#define Ext2FcbLowWater()           (Ext2FcbHighWater() * 3 / 4)

#define MCB_TYPE_SPECIAL            0x40000000  /* unresolved symlink + device node */
#define MCB_TYPE_SYMLINK            0x80000000

#define IsMcbUsed(Mcb)          ((Mcb)->Refercount > 0)
#define IsMcbSymLink(Mcb)       IsFlagOn((Mcb)->Flags, MCB_TYPE_SYMLINK)
#define IsZoneInited(Mcb)       IsFlagOn((Mcb)->Icb->Flags, ICB_ZONE_INITED)
#define IsMcbSpecialFile(Mcb)   IsFlagOn((Mcb)->Flags, MCB_TYPE_SPECIAL)
#define IsMcbRoot(Mcb)          ((Mcb)->Inode->i_ino == EXT2_ROOT_INO)
#define IsMcbReadonly(Mcb)      IsFlagOn((Mcb)->FileAttr, FILE_ATTRIBUTE_READONLY)
#define IsMcbDirectory(Mcb)     IsFlagOn((Mcb)->FileAttr, FILE_ATTRIBUTE_DIRECTORY)
#define IsFileDeleted(Mcb)      IsFlagOn((Mcb)->Flags, MCB_FILE_DELETED)

/* a symlink whose target is gone: deleted, or dropped (Target NULL) - read
   under LinkLock, which a demotion of the link takes to drop it */
#define IsLinkInvalid(Vcb, Mcb) (IsMcbSymLink(Mcb) && Ext2IsLinkDangling((Vcb), (Mcb)))

/* removing this name removes the file: directories have no hard links */
#define Ext2IsLastLink(Mcb)     (IsMcbDirectory(Mcb) || (Mcb)->Inode->i_nlink <= 1)

/*
 * routines for reference count management
 */

/* Reference counters are LONG and change only through Interlocked*: a
   negative value is a release without a matching reference. */
FORCEINLINE LONG Ext2ReferXcb(volatile LONG *Count)
{
    return InterlockedIncrement(Count);
}

FORCEINLINE LONG Ext2DerefXcb(volatile LONG *Count)
{
    LONG Result = InterlockedDecrement(Count);

    ASSERT(Result >= 0);    /* one release more than references taken */
    return Result;
}

#if EXT2_DEBUG
VOID
Ext2TraceMcb(PCHAR fn, USHORT lc, USHORT add, PEXT2_MCB Mcb);
#define Ext2ReferMcb(Mcb) Ext2TraceMcb(__FUNCTION__, __LINE__, TRUE, Mcb)
#define Ext2DerefMcb(Mcb) Ext2TraceMcb(__FUNCTION__, __LINE__, FALSE, Mcb)
#else
#define Ext2ReferMcb(Mcb) Ext2ReferXcb(&Mcb->Refercount)
#define Ext2DerefMcb(Mcb) Ext2DerefXcb(&Mcb->Refercount)
#endif

/* EXT2_CCB Context Control Block

   Data that represents one instance of an open file
   There is one instance of the CCB for every instance of an open file */
typedef struct _EXT2_CCB {

    /* Identifier for this structure */
    EXT2_IDENTIFIER     Identifier;

    /* Flags */
    ULONG               Flags;

    /* Mcb of it's symbol link */
    PEXT2_MCB           SymLink;

    /* The name this handle was opened through (a name of Fcb->Mcb->Icb,
       not necessarily Fcb->Mcb itself when the file has hard links);
       NULL for a volume handle */
    PEXT2_MCB           Mcb;

    /* State that may need to be maintained */
    UNICODE_STRING      DirectorySearchPattern;

    /* Open handle control block */
    struct file         filp;

	/* The EA index we are on */
	ULONG           EaIndex;

    /* The file object this Ccb belongs to, and its link in Fcb->CcbList
       (file handles only, under the Fcb's MainResource): what the unload
       report lists when a file object never got its IRP_MJ_CLOSE */
    PFILE_OBJECT        FileObject;
    LIST_ENTRY          FcbLink;

} EXT2_CCB, *PEXT2_CCB;

/* Flags for CCB */

#define CCB_FROM_POOL               0x00000001
#define CCB_VOLUME_DASD_PURGE       0x00000002
#define CCB_LAST_WRITE_UPDATED      0x00000004
#define CCB_OPEN_REPARSE_POINT      0x00000008
#define CCB_DELETE_ON_CLOSE         0x00000010

#define CCB_ALLOW_EXTENDED_DASD_IO  0x80000000

/* The name a handle refers to for delete / rename / link / name queries:
   the symlink it was opened through, else the hard link it was opened
   through, else whichever name the Fcb holds. */
#define Ext2CcbName(Fcb, Ccb)  ((Ccb)->SymLink ? (Ccb)->SymLink : \
                                ((Ccb)->Mcb ? (Ccb)->Mcb : (Fcb)->Mcb))

/* EXT2_IRP_CONTEXT

   Used to pass information about a request between the drivers functions */
typedef struct ext2_icb {

    /* Identifier for this structure */
    EXT2_IDENTIFIER     Identifier;

    /* Pointer to the IRP this request describes */
    PIRP                Irp;

    /* Flags */
    ULONG               Flags;

    /* The major and minor function code for the request */
    UCHAR               MajorFunction;
    UCHAR               MinorFunction;

    /* The device object */
    PDEVICE_OBJECT      DeviceObject;

    /* The real device object */
    PDEVICE_OBJECT      RealDevice;

    /* The file object */
    PFILE_OBJECT        FileObject;

    PEXT2_FCB           Fcb;
    PEXT2_CCB           Ccb;

    /* If the request is top level */
    BOOLEAN             IsTopLevel;

    /* I/O work item of a request queued to a worker thread (Ext2QueueRequest) */
    PIO_WORKITEM        WorkItem;

    /* If an exception is currently in progress */
    BOOLEAN             ExceptionInProgress;

    /* The exception code when an exception is in progress */
    NTSTATUS            ExceptionCode;

} EXT2_IRP_CONTEXT, *PEXT2_IRP_CONTEXT;


#define IRP_CONTEXT_FLAG_FROM_POOL       (0x00000001)
#define IRP_CONTEXT_FLAG_WAIT            (0x00000002)
#define IRP_CONTEXT_FLAG_WRITE_THROUGH   (0x00000004)
#define IRP_CONTEXT_FLAG_FLOPPY          (0x00000008)
#define IRP_CONTEXT_FLAG_DISABLE_POPUPS  (0x00000020)
#define IRP_CONTEXT_FLAG_DEFERRED        (0x00000040)
#define IRP_CONTEXT_FLAG_VERIFY_READ     (0x00000080)
#define IRP_CONTEXT_STACK_IO_CONTEXT     (0x00000100)
#define IRP_CONTEXT_FLAG_REQUEUED        (0x00000200)
#define IRP_CONTEXT_FLAG_USER_IO         (0x00000400)
#define IRP_CONTEXT_FLAG_FILE_BUSY       (0x00001000)


#define Ext2CanIWait() (!IrpContext || IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT))

/* EXT2_ALLOC_HEADER

   In the checked version of the driver this header is put in the beginning of
   every memory allocation */
typedef struct _EXT2_ALLOC_HEADER {
    EXT2_IDENTIFIER Identifier;
} EXT2_ALLOC_HEADER, *PEXT2_ALLOC_HEADER;

typedef struct _FCB_LIST_ENTRY {
    PEXT2_FCB    Fcb;
    LIST_ENTRY   Next;
} FCB_LIST_ENTRY, *PFCB_LIST_ENTRY;


/* Block Description List */
typedef struct _EXT2_EXTENT {
    LONGLONG    Lba;
    ULONG       Offset;
    ULONG       Length;
    PIRP        Irp;
    struct _EXT2_EXTENT * Next;
} EXT2_EXTENT, *PEXT2_EXTENT;

#endif /* _EXT4_EXT4FS_OBJECTS_H_ */
