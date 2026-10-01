/**
 * debug.c - checked-build debug output: level filtered prints, process names, MCB traces.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <stdarg.h>

#if EXT2_DEBUG

#define SYSTEM_PROCESS_NAME "System"

ULONG   DebugFilter = DL_DEFAULT;

ULONG  ProcessNameOffset = 0;

/*
 * Ext2Printf
 *   This function is variable-argument, level-sensitive debug print routine.
 *   If the specified debug level for the print statement is lower or equal
 *   to the current debug level, the message will be printed.
 *
 * Arguments:
 *   DebugMessage - Variable argument ascii c string
 *
 * Return Value:
 *   N/A
 *
 * NOTES:
 *   N/A
 */

#define DBG_BUF_LEN 0x100
VOID
Ext2Printf(
    PCHAR DebugMessage,
    ...
)
{
    va_list             ap;
    LARGE_INTEGER       CurrentTime;
    TIME_FIELDS         TimeFields;
    CHAR                Buffer[DBG_BUF_LEN];

    RtlZeroMemory(Buffer, DBG_BUF_LEN);
    va_start(ap, DebugMessage);

    KeQuerySystemTime( &CurrentTime);
    RtlTimeToTimeFields(&CurrentTime, &TimeFields);
    _vsnprintf(&Buffer[0], DBG_BUF_LEN, DebugMessage, ap);

    DbgPrint(DRIVER_NAME":~%d: %2.2d:%2.2d:%2.2d:%3.3d %8.8x:   %s",
             KeGetCurrentProcessorNumber(),
             TimeFields.Hour, TimeFields.Minute,
             TimeFields.Second, TimeFields.Milliseconds,
             PsGetCurrentThread(), Buffer);

    va_end(ap);
}

VOID
Ext2NiPrintf(
    PCHAR DebugMessage,
    ...
)
{
    va_list             ap;
    LARGE_INTEGER       CurrentTime;
    TIME_FIELDS         TimeFields;
    CHAR                Buffer[0x100];

    va_start(ap, DebugMessage);

    KeQuerySystemTime( &CurrentTime);
    RtlTimeToTimeFields(&CurrentTime, &TimeFields);
    _vsnprintf(&Buffer[0], 0x100, DebugMessage, ap);

    DbgPrint(DRIVER_NAME":~%d: %2.2d:%2.2d:%2.2d:%3.3d %8.8x: %s",
             KeGetCurrentProcessorNumber(),
             TimeFields.Hour, TimeFields.Minute,
             TimeFields.Second, TimeFields.Milliseconds,
             PsGetCurrentThread(), Buffer);

    va_end(ap);

} // Ext2NiPrintf()

ULONG
Ext2GetProcessNameOffset ( VOID )
{
    PEPROCESS   Process;
    ULONG       i;

    Process = PsGetCurrentProcess();

    for (i = 0; i < PAGE_SIZE; i++) {
        if (!strncmp(
                    SYSTEM_PROCESS_NAME,
                    (PCHAR) Process + i,
                    strlen(SYSTEM_PROCESS_NAME)
                )) {

            return i;
        }
    }

    DEBUG(DL_ERR, ( ": *** FsdGetProcessNameOffset failed ***\n"));

    return 0;
}

/* avoid stack overflow for dead symlinks */

VOID
Ext2TraceMcb(PCHAR fn, USHORT lc, USHORT add, PEXT2_MCB Mcb) {
    size_t i;
    CHAR _space[33];

    _snprintf(&_space[0], 32, "%s:%d:", fn, lc);
    _space[32] = 0;
    i = strlen(_space);
    while (i < 32) {
        _space[i++] = ' ';
        _space[i]=0;
    }
    if (add) {
        Ext2ReferXcb(&Mcb->Refercount);
        DEBUG(DL_RES,   ("%s +%2u %wZ (%p)\n", _space, (Mcb->Refercount - 1), &Mcb->FullName, Mcb));
    } else {
        Ext2DerefXcb(&Mcb->Refercount);
        DEBUG(DL_RES, ("%s -%2u %wZ (%p)\n", _space, Mcb->Refercount, &Mcb->FullName, Mcb));
    }
}

#endif /* EXT2_DEBUG */
