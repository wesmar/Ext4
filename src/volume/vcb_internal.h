/**
 * vcb_internal.h - what the volume files share (vcb.c, vcb_init.c).
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_VOLUME_VCB_INTERNAL_H_
#define _EXT4_VOLUME_VCB_INTERNAL_H_

/* our last reference to the metadata stream (vcb.c) */
VOID
Ext2ReleaseStream(IN PEXT2_VCB Vcb, IN PFILE_OBJECT Stream);

#endif /* _EXT4_VOLUME_VCB_INTERNAL_H_ */