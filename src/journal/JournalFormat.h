/**
 * JournalFormat.h - the jbd2 log on disk: block layouts, flags and features.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * Every field is big-endian and read or written by its byte offset, so the
 * same definitions serve the driver, the host models and gcc in WSL. The
 * layout is the published jbd2 format (e2fsprogs, Linux fs/jbd2):
 *
 *   every log block but a data block starts with a 12-byte header
 *       magic 0xC03B3998 | block type | transaction ID
 *   descriptor block: header, then tags, each naming the volume block the
 *       next log block is a copy of; v2/v3 checksums end it with a 4-byte tail
 *   commit block: header, checksum type and size, 32 bytes of checksum,
 *       the commit time (seconds, nanoseconds)
 *   revoke block: header, the bytes in use, then 4- or 8-byte block numbers
 *   superblock: geometry, the log's start and first transaction, features
 *
 * Data blocks carry no header; one whose first word would read as the magic
 * is logged with that word zeroed and the ESCAPE flag in its tag.
 */

#ifndef _EXT4_JOURNAL_FORMAT_H_
#define _EXT4_JOURNAL_FORMAT_H_

#define JF_MAGIC                0xC03B3998u

/* block types */
#define JF_DESCRIPTOR           1u
#define JF_COMMIT               2u
#define JF_SUPERBLOCK_V1        3u
#define JF_SUPERBLOCK_V2        4u
#define JF_REVOKE               5u

/* header */
#define JF_H_MAGIC              0
#define JF_H_TYPE               4
#define JF_H_SEQUENCE           8
#define JF_HEADER_SIZE          12

/* superblock */
#define JF_S_BLOCKSIZE          12
#define JF_S_MAXLEN             16
#define JF_S_FIRST              20
#define JF_S_SEQUENCE           24
#define JF_S_START              28
#define JF_S_ERRNO              32
#define JF_S_COMPAT             36
#define JF_S_INCOMPAT           40
#define JF_S_RO_COMPAT          44
#define JF_S_UUID               48      /* 16 bytes */
#define JF_S_NR_USERS           64
#define JF_S_CHECKSUM_TYPE      80      /* one byte */
#define JF_S_NUM_FC_BLOCKS      84
#define JF_S_HEAD               88
#define JF_S_CHECKSUM           252
#define JF_SUPERBLOCK_SIZE      1024    /* what the superblock checksum covers */

/* commit block */
#define JF_C_CHECKSUM_TYPE      12      /* one byte */
#define JF_C_CHECKSUM_SIZE      13      /* one byte */
#define JF_C_CHECKSUM           16      /* first of eight words */
#define JF_C_COMMIT_SEC         48      /* 8 bytes */
#define JF_C_COMMIT_NSEC        56

/* revoke block */
#define JF_R_COUNT              12      /* bytes in use, header included */
#define JF_REVOKE_HEADER_SIZE   16

/* the checksum tail of descriptor and revoke blocks (v2/v3) */
#define JF_TAIL_SIZE            4

/* tags: the block number, flags in the 16 bits at offset 6 in both forms,
   the high half at 8 (64-bit logs); the checksum is 16 bits at 4 (v2) or
   32 bits at 12 (v3) */
#define JF_T_BLOCK              0
#define JF_T_CHECKSUM16         4
#define JF_T_FLAGS              6
#define JF_T_BLOCK_HIGH         8
#define JF_T_CHECKSUM32         12
#define JF_UUID_SIZE            16      /* follows a tag without SAME_UUID */

#define JF_FLAG_ESCAPE          0x0001u /* the first word was the magic */
#define JF_FLAG_SAME_UUID       0x0002u /* no uuid follows this tag */
#define JF_FLAG_DELETED         0x0004u
#define JF_FLAG_LAST_TAG        0x0008u

/* features (a version 1 superblock has none) */
#define JF_COMPAT_CHECKSUM      0x00000001u     /* v1: crc32 in the commit */
#define JF_INCOMPAT_REVOKE      0x00000001u
#define JF_INCOMPAT_64BIT       0x00000002u
#define JF_INCOMPAT_ASYNC_COMMIT 0x00000004u
#define JF_INCOMPAT_CSUM_V2     0x00000008u
#define JF_INCOMPAT_CSUM_V3     0x00000010u
#define JF_INCOMPAT_FAST_COMMIT 0x00000020u
#define JF_KNOWN_COMPAT         JF_COMPAT_CHECKSUM
#define JF_KNOWN_INCOMPAT       (JF_INCOMPAT_REVOKE | JF_INCOMPAT_64BIT | JF_INCOMPAT_ASYNC_COMMIT | \
                                 JF_INCOMPAT_CSUM_V2 | JF_INCOMPAT_CSUM_V3)
#define JF_KNOWN_RO_COMPAT      0u

/* commit block checksum types */
#define JF_CRC32_CHECKSUM       1u
#define JF_CRC32_CHECKSUM_SIZE  4u
#define JF_CRC32C_CHECKSUM      4u      /* the superblock's s_checksum_type */

/* the smallest log the format allows */
#define JF_MIN_LOG_BLOCKS       1024u

static __inline unsigned int JfGet32(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

static __inline unsigned int JfGet16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}

static __inline unsigned long long JfGet64(const unsigned char *p)
{
    return ((unsigned long long)JfGet32(p) << 32) | JfGet32(p + 4);
}

static __inline void JfPut32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static __inline void JfPut16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static __inline void JfPut64(unsigned char *p, unsigned long long v)
{
    JfPut32(p, (unsigned int)(v >> 32));
    JfPut32(p + 4, (unsigned int)v);
}

/* checksums v2 and v3 both use crc32c over the uuid-seeded state */
static __inline int JfHasCsumV2V3(unsigned int Incompat)
{
    return (Incompat & (JF_INCOMPAT_CSUM_V2 | JF_INCOMPAT_CSUM_V3)) != 0;
}

/*
 * Bytes per tag, by the feature set: 16 with checksum v3; otherwise 12,
 * 2 more with checksum v2, 4 fewer without 64-bit block numbers. A tag
 * without SAME_UUID is followed by the 16-byte uuid.
 */
static __inline unsigned int JfTagBytes(unsigned int Incompat)
{
    unsigned int bytes;

    if (Incompat & JF_INCOMPAT_CSUM_V3)
        return 16;
    bytes = 12;
    if (Incompat & JF_INCOMPAT_CSUM_V2)
        bytes += 2;
    if (!(Incompat & JF_INCOMPAT_64BIT))
        bytes -= 4;
    return bytes;
}

/* transaction IDs compare on the circle of 2^32: a after b when a - b,
   read as a signed number, is positive */
static __inline int JfTidAfter(unsigned int a, unsigned int b)
{
    return (int)(a - b) > 0;
}

static __inline int JfTidAtLeast(unsigned int a, unsigned int b)
{
    return (int)(a - b) >= 0;
}

#endif /* _EXT4_JOURNAL_FORMAT_H_ */
