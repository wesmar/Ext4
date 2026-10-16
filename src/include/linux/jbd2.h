/**
 * jbd2.h - what is left of the jbd2 names: the transaction ID type, the
 * extent code's handle type and two error codes.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * The journal is the driver's own (src/journal); the log's on-disk format
 * is src/journal/JournalFormat.h.
 */

#ifndef _LINUX_JBD2_H
#define _LINUX_JBD2_H

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/module.h>

typedef unsigned int tid_t;                     /* a transaction ID */
typedef struct jbd2_journal_handle handle_t;    /* opaque; the extent code passes NULL */

/* crc32c without the final inversion, as ext4 and jbd2 use it
   (MetadataChecksums.c) */
__u32 crc32c(__u32 crc, const __u8 *data, unsigned int length);

#define EFSBADCRC       EBADMSG     /* a checksum does not match */
#define EFSCORRUPTED    EUCLEAN     /* the file system is corrupt */

#endif /* _LINUX_JBD2_H */
