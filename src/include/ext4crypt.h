/**
 * ext4crypt.h - LUKS volumes: the interface between ext4ctl and ext4.sys.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The split follows dm-crypt and cryptsetup. ext4ctl (user mode) reads the
 * LUKS header, derives the keyslot key from the passphrase (PBKDF2 or
 * Argon2, which may want a gigabyte of memory - nothing for a kernel),
 * decrypts and merges the key material and checks it against the digest.
 * Only the verified volume key crosses into the kernel, together with the
 * geometry of the encrypted segment. The driver then puts a disk device of
 * its own over the partition that en- and decrypts every sector (AES-XTS),
 * and the file system mounts on that device like on any other.
 *
 * Shared by the driver and ext4ctl: plain Windows types only.
 */

#ifndef _EXT4_EXT4CRYPT_H_
#define _EXT4_EXT4CRYPT_H_

/* on the control device \\.\ext4, administrators only (Ext2CheckAppIoctl) */
#define IOCTL_APP_CRYPT_UNLOCK \
CTL_CODE(FILE_DEVICE_UNKNOWN, 2010, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_APP_CRYPT_LOCK \
CTL_CODE(FILE_DEVICE_UNKNOWN, 2011, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_APP_CRYPT_QUERY \
CTL_CODE(FILE_DEVICE_UNKNOWN, 2012, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define EXT4_CRYPT_MAGIC            'TPRC'
#define EXT4_CRYPT_VERSION          1

#define EXT4_CRYPT_DEVICE_CHARS     128     /* \Device\HarddiskVolumeN and the like */
#define EXT4_CRYPT_UUID_CHARS       40      /* LUKS UUID, text, NUL-terminated */
#define EXT4_CRYPT_MAX_KEY          64      /* AES-256-XTS: two 256-bit keys */
#define EXT4_CRYPT_KEY_AES128_XTS   32      /* two 128-bit keys */
#define EXT4_CRYPT_KEY_AES256_XTS   64
#define EXT4_CRYPT_MAX_VOLUMES      16

/* sector encryption; the only one LUKS has used by default since 2011 */
#define EXT4_CIPHER_AES_XTS_PLAIN64 1       /* IV: 64-bit sector number */
#define EXT4_CIPHER_AES_XTS_PLAIN   2       /* IV: the same, truncated to 32 bits */

/* Flags of EXT4_CRYPT_UNLOCK */
#define EXT4_CRYPT_READ_ONLY        0x00000001
#define EXT4_CRYPT_IV_LARGE_SECTORS 0x00000002  /* IV counts SectorSize units (LUKS2),
                                                   not 512-byte ones (LUKS1) */
/* Flags of EXT4_CRYPT_LOCK */
#define EXT4_CRYPT_FORCE            0x00000001  /* dismount even with open files */

/*
 * Every request starts with the header the driver's IOCTL gate checks
 * (Magic, Flags, Command); the output buffer is the same structure.
 */
typedef struct _EXT4_CRYPT_UNLOCK {
    ULONG       Magic;
    ULONG       Flags;
    ULONG       Command;                    /* 0 */
    ULONG       Version;                    /* EXT4_CRYPT_VERSION */

    ULONG       Cipher;                     /* EXT4_CIPHER_* */
    ULONG       KeyBytes;                   /* 32 or 64 */
    ULONG       SectorSize;                 /* 512, 1024, 2048 or 4096 */
    WCHAR       Letter;                     /* wanted drive letter, 0: first free */
    WCHAR       Reserved;

    ULONGLONG   PayloadOffset;              /* bytes from the start of the partition */
    ULONGLONG   PayloadSize;                /* bytes, 0: up to the end of the partition */
    ULONGLONG   IvOffset;                   /* added to every sector number (iv_tweak) */

    WCHAR       Device[EXT4_CRYPT_DEVICE_CHARS];    /* NT name of the partition */
    CHAR        Uuid[EXT4_CRYPT_UUID_CHARS];        /* identifies the volume to the mount manager */
    UCHAR       Key[EXT4_CRYPT_MAX_KEY];            /* the volume (master) key */

    /* out */
    ULONG       Index;                      /* \Device\Ext4Crypt<Index> */
    ULONG       Padding;
} EXT4_CRYPT_UNLOCK, *PEXT4_CRYPT_UNLOCK;

typedef struct _EXT4_CRYPT_LOCK {
    ULONG       Magic;
    ULONG       Flags;                      /* EXT4_CRYPT_FORCE */
    ULONG       Command;                    /* 0 */
    ULONG       Index;                      /* the unlocked volume to close */
} EXT4_CRYPT_LOCK, *PEXT4_CRYPT_LOCK;

typedef struct _EXT4_CRYPT_ENTRY {
    ULONG       Index;
    ULONG       Flags;                      /* EXT4_CRYPT_READ_ONLY ... */
    ULONG       SectorSize;
    ULONG       KeyBytes;
    ULONGLONG   PayloadOffset;
    ULONGLONG   PayloadSize;
    WCHAR       Device[EXT4_CRYPT_DEVICE_CHARS];
    CHAR        Uuid[EXT4_CRYPT_UUID_CHARS];
} EXT4_CRYPT_ENTRY, *PEXT4_CRYPT_ENTRY;

typedef struct _EXT4_CRYPT_QUERY {
    ULONG       Magic;
    ULONG       Flags;
    ULONG       Command;                    /* 0 */
    ULONG       Count;                      /* out */
    EXT4_CRYPT_ENTRY Entries[EXT4_CRYPT_MAX_VOLUMES];
} EXT4_CRYPT_QUERY, *PEXT4_CRYPT_QUERY;

#define EXT4_CRYPT_DEVICE_PREFIX    L"\\Device\\Ext4Crypt"

/*
 * LVM logical volumes inside an unlocked LUKS volume (Qubes OS keeps its
 * root and every qube's disks as thin volumes there). ext4ctl reads the
 * LVM and dm-thin metadata and sends the volume as a table of runs over
 * the LUKS device; the driver only remaps, read-only: a write to a thin
 * volume would have to update the pool's metadata.
 */
#define IOCTL_APP_LV_OPEN \
CTL_CODE(FILE_DEVICE_UNKNOWN, 2013, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_APP_LV_CLOSE \
CTL_CODE(FILE_DEVICE_UNKNOWN, 2014, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_APP_LV_QUERY \
CTL_CODE(FILE_DEVICE_UNKNOWN, 2015, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define EXT4_LV_HOLE                ((ULONGLONG)-1)     /* a run that reads as zeros */
#define EXT4_LV_MAX_RUNS            (1024 * 1024)
#define EXT4_LV_NAME_CHARS          128                 /* "vg/lv" */
#define EXT4_LV_MAX_VOLUMES         32
#define EXT4_LV_DEVICE_PREFIX       L"\\Device\\Ext4Lv"

typedef struct _EXT4_LV_RUN {
    ULONGLONG   Start;                      /* in the volume, bytes */
    ULONGLONG   Length;                     /* bytes, a multiple of 512 */
    ULONGLONG   Target;                     /* on the LUKS device, bytes; EXT4_LV_HOLE */
} EXT4_LV_RUN, *PEXT4_LV_RUN;

typedef struct _EXT4_LV_OPEN {
    ULONG       Magic;                      /* EXT4_CRYPT_MAGIC */
    ULONG       Flags;
    ULONG       Command;                    /* 0 */
    ULONG       Version;                    /* EXT4_CRYPT_VERSION */
    ULONG       Crypt;                      /* \Device\Ext4Crypt<Crypt> below */
    WCHAR       Letter;                     /* wanted drive letter, 0: first free */
    WCHAR       Reserved;
    ULONGLONG   Size;                       /* of the volume, bytes */
    CHAR        Uuid[EXT4_CRYPT_UUID_CHARS];/* LVM LV uuid: the mount manager's unique id */
    CHAR        Name[EXT4_LV_NAME_CHARS];   /* vg/lv, for status */
    ULONG       Runs;                       /* entries in Run[], covering [0, Size) in order */
    ULONG       Index;                      /* out: \Device\Ext4Lv<Index> */
    EXT4_LV_RUN Run[ANYSIZE_ARRAY];
} EXT4_LV_OPEN, *PEXT4_LV_OPEN;

typedef struct _EXT4_LV_CLOSE {
    ULONG       Magic;
    ULONG       Flags;                      /* EXT4_CRYPT_FORCE */
    ULONG       Command;
    ULONG       Index;
} EXT4_LV_CLOSE, *PEXT4_LV_CLOSE;

typedef struct _EXT4_LV_ENTRY {
    ULONG       Index;
    ULONG       Crypt;
    ULONG       Runs;
    ULONG       Reserved;
    ULONGLONG   Size;
    CHAR        Uuid[EXT4_CRYPT_UUID_CHARS];
    CHAR        Name[EXT4_LV_NAME_CHARS];
} EXT4_LV_ENTRY, *PEXT4_LV_ENTRY;

typedef struct _EXT4_LV_QUERY {
    ULONG       Magic;
    ULONG       Flags;
    ULONG       Command;
    ULONG       Count;                      /* out */
    EXT4_LV_ENTRY Entries[EXT4_LV_MAX_VOLUMES];
} EXT4_LV_QUERY, *PEXT4_LV_QUERY;

/*
 * A file as a disk (volume\FileDisk.c): an ext4 volume in an image file, on
 * any file system but this driver's own. Path is the file's NT name.
 */
#define IOCTL_APP_FILE_OPEN \
CTL_CODE(FILE_DEVICE_UNKNOWN, 2016, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_APP_FILE_CLOSE \
CTL_CODE(FILE_DEVICE_UNKNOWN, 2017, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_APP_FILE_QUERY \
CTL_CODE(FILE_DEVICE_UNKNOWN, 2018, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define EXT4_FILE_PATH_CHARS        512
#define EXT4_FILE_MAX_DISKS         16
#define EXT4_FILE_DEVICE_PREFIX     L"\\Device\\Ext4File"

typedef struct _EXT4_FILE_OPEN {
    ULONG       Magic;                      /* EXT4_CRYPT_MAGIC */
    ULONG       Flags;                      /* EXT4_CRYPT_READ_ONLY */
    ULONG       Command;                    /* 0 */
    ULONG       Version;                    /* EXT4_CRYPT_VERSION */
    WCHAR       Letter;                     /* wanted drive letter, 0: first free */
    WCHAR       Reserved;
    ULONG       Index;                      /* out: \Device\Ext4File<Index> */
    ULONG       Sparse;                     /* out: the file is sparse, trims make holes */
    ULONG       Padding;
    ULONGLONG   Size;                       /* out: of the device, bytes */
    WCHAR       Path[EXT4_FILE_PATH_CHARS];
} EXT4_FILE_OPEN, *PEXT4_FILE_OPEN;

typedef struct _EXT4_FILE_CLOSE {
    ULONG       Magic;
    ULONG       Flags;                      /* EXT4_CRYPT_FORCE */
    ULONG       Command;
    ULONG       Index;
} EXT4_FILE_CLOSE, *PEXT4_FILE_CLOSE;

typedef struct _EXT4_FILE_ENTRY {
    ULONG       Index;
    ULONG       Flags;
    ULONG       Sparse;
    ULONG       Reserved;
    ULONGLONG   Size;
    WCHAR       Path[EXT4_FILE_PATH_CHARS];
} EXT4_FILE_ENTRY, *PEXT4_FILE_ENTRY;

typedef struct _EXT4_FILE_QUERY {
    ULONG       Magic;
    ULONG       Flags;
    ULONG       Command;
    ULONG       Count;                      /* out */
    EXT4_FILE_ENTRY Entries[EXT4_FILE_MAX_DISKS];
} EXT4_FILE_QUERY, *PEXT4_FILE_QUERY;

/*
 * On a volume handle (\\.\X:, write access): the free blocks given back to
 * the device, what fstrim does. MinLength: shorter free runs stay.
 */
#define FSCTL_EXT4_TRIM \
CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 3000, METHOD_BUFFERED, FILE_WRITE_DATA)

typedef struct _EXT4_TRIM {
    ULONGLONG   MinLength;                  /* in: bytes */
    ULONGLONG   Bytes;                      /* out: trimmed */
    ULONGLONG   Runs;                       /* out: in so many runs */
} EXT4_TRIM, *PEXT4_TRIM;

#endif /* _EXT4_EXT4CRYPT_H_ */
