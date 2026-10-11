/* SPDX-License-Identifier: GPL-2.0-only
 * Extract the real dentry builder; inject allocation/conversion failures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#define IN
#define NT_SUCCESS(s) ((s) >= 0)
#define STATUS_INSUFFICIENT_RESOURCES ((int32_t)0xc000009a)
#define DEBUG(...) ((void)0)
#define PagedPool 1
#define RtlZeroMemory(p,n) memset((p),0,(n))
typedef int32_t NTSTATUS;
typedef unsigned short USHORT;
typedef struct { unsigned short Length, MaximumLength; char *Buffer; } OEM_STRING;
typedef struct { unsigned short Length, MaximumLength; wchar_t *Buffer; } UNICODE_STRING, *PUNICODE_STRING;
struct dentry { void *d_sb; struct dentry *d_parent; struct { char *name; unsigned len; } d_name; };
typedef struct { int sb; } VCB, *PEXT2_VCB;
typedef struct { struct dentry *de; } MCB, *PEXT2_MCB;
static struct dentry Entry;
static unsigned Failure, Frees, Allocations;
static struct dentry *Ext2AllocateEntry(void)
{
    memset(&Entry,0,sizeof(Entry));
    return Failure == 1 ? NULL : &Entry;
}
static void *Ext2AllocatePool(int pool, unsigned bytes, int tag)
{
    (void)pool; (void)tag;
    if (Failure == 2) return NULL;
    Allocations++;
    return malloc(bytes);
}
static unsigned Ext2UnicodeToOEMSize(PEXT2_VCB vcb, PUNICODE_STRING name)
{ (void)vcb; (void)name; return 3; }
static NTSTATUS Ext2UnicodeToOEM(PEXT2_VCB vcb, OEM_STRING *name, PUNICODE_STRING unicode)
{
    (void)vcb; (void)unicode;
    if (Failure == 3) return (int32_t)0xc0000001;
    memcpy(name->Buffer,"abc",3); name->Length=3; return 0;
}
static void Ext2FreeEntry(struct dentry *entry)
{
    if (entry != &Entry || ++Frees != 1) abort();
    if (entry->d_name.name) { free(entry->d_name.name); Allocations--; }
}
#include "builder.h"
void VerifyDentry(void)
{
    VCB vcb = {0}; MCB parent = {&Entry}; UNICODE_STRING name = {0};
    for (Failure=0; Failure<=3; Failure++) {
        struct dentry *result;
        Frees=Allocations=0;
        result=Ext2BuildEntry(&vcb,&parent,&name);
        if (Failure) {
            if (result || Allocations || Frees != (Failure == 1 ? 0u : 1u)) abort();
        } else {
            if (result != &Entry || result->d_name.len != 3 ||
                memcmp(result->d_name.name,"abc",3) || Frees || Allocations != 1) abort();
            Ext2FreeEntry(result);
            if (Allocations || Frees != 1) abort();
        }
    }
    puts("DENTRY-MODEL: success, both allocation failures and conversion failure: ALL PASSED");
}
