/**
 * MountRecovery.c - journal replay at mount time.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/jbd2.h>

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2LoadInternalJournal)
#pragma alloc_text(PAGE, Ext2CheckJournal)
#pragma alloc_text(PAGE, Ext2RecoverJournal)
#endif

PEXT2_MCB
Ext2LoadInternalJournal(
    PEXT2_VCB         Vcb,
    ULONG             jNo
)
{
    PEXT2_MCB   Jcb = NULL;
    BOOLEAN     Attached;

    Jcb = Ext2AllocateMcb(Vcb, NULL, NULL, 0);
    if (!Jcb) {
        goto errorout;
    }

    /* the journal inode gets an Icb like any other name */
    Attached = Ext2AttachIcb(Vcb, Jcb, jNo);
    if (!Attached) {
        Ext2FreeMcb(Vcb, Jcb);
        Jcb = NULL;
        goto errorout;
    }

    if (!IsFlagOn(Jcb->Icb->Flags, ICB_INODE_LOADED)) {
        if (!Ext2LoadInode(Vcb, Jcb->Inode)) {
            Ext2FreeMcb(Vcb, Jcb);
            Jcb = NULL;
            goto errorout;
        }
        SetLongFlag(Jcb->Icb->Flags, ICB_INODE_LOADED);
    }

errorout:

    return Jcb;
}

INT
Ext2CheckJournal(
    PEXT2_VCB          Vcb,
    PULONG             jNo
)
{
    struct ext3_super_block* esb = NULL;

    /* check ext3 super block */
    esb = (struct ext3_super_block *)Vcb->SuperBlock;
    if (IsFlagOn(esb->s_feature_incompat,
                 EXT3_FEATURE_INCOMPAT_RECOVER)) {
        SetLongFlag(Vcb->Flags, VCB_JOURNAL_RECOVER);
    }

    /* must stop here if volume is read-only */
    if (IsVcbReadOnly(Vcb)) {
        goto errorout;
    }

    /* journal is external ? */
    if (esb->s_journal_inum == 0) {
        goto errorout;
    }

    /* oops: volume is corrupted */
    if (esb->s_journal_dev) {
        goto errorout;
    }

    /* return the journal inode number */
    *jNo = esb->s_journal_inum;

    return TRUE;

errorout:

    return FALSE;
}

INT
Ext2RecoverJournal(
    PEXT2_IRP_CONTEXT  IrpContext,
    PEXT2_VCB          Vcb
)
{
    INT                     rc = 0;
    ULONG                   jNo = 0;
    PEXT2_MCB               jcb = NULL;
    struct block_device *   bd = &Vcb->bd;
    struct super_block *    sb = &Vcb->sb;
    struct inode *          ji = NULL;
    journal_t *             journal = NULL;
    BOOLEAN                 replayed = FALSE;

    ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);

    /* the engine already owns the journal */
    if (Ext2JournalIsActive(Vcb)) {
        goto errorout;
    }

    /* check journal inode number */
    if (!Ext2CheckJournal(Vcb, &jNo)) {
        rc = -1;
        goto errorout;
    }

    /* allocate journal Mcb */
    jcb =  Ext2LoadInternalJournal(Vcb, jNo);
    if (!jcb) {
        rc = -6;
        goto errorout;
    }

    /* allocate journal inode */
    ji = jcb->Inode;

    /* initialize journal file from inode */
    journal = jbd2_journal_init_inode(ji);

    /* initialzation succeeds ? */
    if (!journal) {
        DEBUG(DL_ERR, ( "jbd2_journal_init_inode failed\n"));
        rc = -8;
        goto errorout;
    }

    if (!ext4_has_feature_journal_needs_recovery(sb)) {
        /* the fs was unmounted cleanly: whatever the journal still holds
           is stale, discard it (Linux does the same before loading) */
        rc = jbd2_journal_wipe(journal, !IsVcbReadOnly(Vcb));
        if (rc != 0) {
            DEBUG(DL_ERR, ( "ext4: recover_journal: failed to wipe journal. rc=%d\n", rc));
            rc = -7;
            goto errorout;
        }
    } else {
        replayed = TRUE;
    }

    /* loading the journal replays it when needed and sets up the log
       geometry the engine works with */
    rc = jbd2_journal_load(journal);
    if (rc != 0) {
        DEBUG(DL_ERR, ( "ext4: recover_journal: failed "
                        "to recover journal data. rc=%d\n", rc));
        rc = -9;
        goto errorout;
    }

    if (replayed) {

        /* reload super_block and group_description */
        Ext2RefreshSuper(IrpContext, Vcb);
        Ext2RefreshGroup(IrpContext, Vcb);

        /* clear recover flag in sb */
        ClearLongFlag(
            Vcb->SuperBlock->s_feature_incompat,
            EXT3_FEATURE_INCOMPAT_RECOVER);
        Ext2SaveSuper(IrpContext, Vcb);
        sync_blockdev(bd);
        ClearLongFlag(Vcb->Flags, VCB_JOURNAL_RECOVER);
    }

    /* a writable volume keeps the journal and logs every change */
    if (!IsVcbReadOnly(Vcb)) {
        NTSTATUS Status = Ext2JournalStart(Vcb, journal, jcb);
        if (NT_SUCCESS(Status)) {
            journal = NULL;     /* owned by the engine now */
            jcb = NULL;
        } else {
            DEBUG(DL_ERR, ( "ext4: recover_journal: failed to start journal: %xh\n", Status));
            rc = -10;
        }
    }

errorout:

    /* destroy journal structure */
    if (journal) {
        jbd2_journal_destroy(journal);
    }

    /* destory journal Mcb */
    if (jcb) {
        Ext2FreeMcb(Vcb, jcb);
    }

    ExReleaseResourceLite(&Vcb->MainResource);

    return rc;
}
