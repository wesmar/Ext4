/**
 * lvm_cmd.c - the LVM commands: lv list, lv open, lv close, and the LVM part of status.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "lvm_internal.h"

static void
LvDevice(ULONG Index, WCHAR *Out, size_t OutChars)
{
    swprintf_s(Out, OutChars, EXT4_LV_DEVICE_PREFIX L"%lu", Index);
}

int
CmdLvList(ULONG Crypt)
{
    LVM_VG          G;
    const LVM_NODE *Lvs, *Lv;

    if (!LvmLoad(&G, Crypt, FALSE)) {
        LvmClose(&G);
        return 1;
    }
    Say("VG %s on \\Device\\Ext4Crypt%lu (%s), extent %llu KiB", G.VgName, Crypt, G.PvName,
        G.ExtentBytes / 1024);
    Say("%-32s %-7s %10s  %-9s %s", "volume", "type", "size", "fs", "mapped");
    Lvs = LvmGet(G.Vg, "logical_volumes");
    for (Lv = Lvs ? Lvs->Child : NULL; Lv; Lv = Lv->Next) {
        RUNS    R;
        UINT64  Size = 0;
        ULONG   Mapped = 0;
        LV_TYPE Type;
        char    Full[EXT4_LV_NAME_CHARS];

        if (Lv->Kind != LVM_SECTION || !LvmHasFlag(Lv, "status", "VISIBLE")) {
            continue;
        }
        Type = LvmType(Lv, G.ExtentBytes, &Size);
        snprintf(Full, sizeof(Full), "%s/%s", G.VgName, Lv->Name);
        if (Type == LV_OTHER) {
            Say("%-32s %-7s %8.2f G  %-9s", Full, "pool", Size / EXT4CTL_GIB, "-");
            continue;
        }
        if (!LvmRuns(&G, Lv, &R, &Size, &Mapped)) {
            Say("%-32s %-7s %8.2f G  (cannot map)", Full, Type == LV_THIN ? "thin" : "linear", Size / EXT4CTL_GIB);
        } else if (Type == LV_THIN) {
            Say("%-32s %-7s %8.2f G  %-9s %lu chunks, %lu runs", Full, "thin", Size / EXT4CTL_GIB,
                LvmFsName(&G, &R), Mapped, R.Count);
        } else {
            Say("%-32s %-7s %8.2f G  %-9s %lu run(s)", Full, "linear", Size / EXT4CTL_GIB,
                LvmFsName(&G, &R), R.Count);
        }
        RunsFree(&R);
    }
    LvmClose(&G);
    return 0;
}

/* the open request: the volume's runs and names, for IOCTL_APP_LV_OPEN */
static PEXT4_LV_OPEN
LvOpenRequest(const LVM_VG *G, const LVM_NODE *Lv, ULONG Crypt, WCHAR Letter, const RUNS *R,
              UINT64 Size, SIZE_T *Bytes)
{
    PEXT4_LV_OPEN Req;

    *Bytes = FIELD_OFFSET(EXT4_LV_OPEN, Run) + (SIZE_T)R->Count * sizeof(EXT4_LV_RUN);
    Req = (PEXT4_LV_OPEN)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, *Bytes);
    if (Req == NULL) {
        return NULL;
    }
    Req->Magic = EXT4_CRYPT_MAGIC;
    Req->Version = EXT4_CRYPT_VERSION;
    Req->Crypt = Crypt;
    Req->Letter = Letter;
    Req->Size = Size;
    strcpy_s(Req->Uuid, sizeof(Req->Uuid), LvmStr(Lv, "id") ? LvmStr(Lv, "id") : Lv->Name);
    snprintf(Req->Name, sizeof(Req->Name), "%s/%s", G->VgName, Lv->Name);
    Req->Runs = R->Count;
    memcpy(Req->Run, R->Run, (SIZE_T)R->Count * sizeof(EXT4_LV_RUN));
    return Req;
}

int
CmdLvOpen(ULONG Crypt, const WCHAR *VolumeName, WCHAR Letter)
{
    LVM_VG          G;
    const LVM_NODE *Lv;
    char            Name[EXT4_LV_NAME_CHARS], *Slash;
    RUNS            R;
    UINT64          Size;
    ULONG           Mapped;
    PEXT4_LV_OPEN   Req;
    SIZE_T          Bytes;
    DWORD           Error;
    WCHAR           Device[64], Fs[32] = L"", Mounted;

    if (WideCharToMultiByte(CP_UTF8, 0, VolumeName, -1, Name, sizeof(Name), NULL, NULL) <= 0) {
        return 2;
    }
    if (!LvmLoad(&G, Crypt, FALSE)) {
        LvmClose(&G);
        return 1;
    }
    /* "vg/lv" or "lv" */
    Slash = strchr(Name, '/');
    if (Slash && (size_t)(Slash - Name) == strlen(G.VgName) && strncmp(Name, G.VgName, Slash - Name) == 0) {
        memmove(Name, Slash + 1, strlen(Slash + 1) + 1);
    }
    Lv = LvmFindLv(&G, Name);
    if (Lv == NULL || !LvmHasFlag(Lv, "status", "VISIBLE")) {
        Fail("no volume %s in VG %s (see ext4ctl lv list %lu)", Name, G.VgName, Crypt);
        LvmClose(&G);
        return 1;
    }
    if (!LvmRuns(&G, Lv, &R, &Size, &Mapped)) {
        LvmClose(&G);
        return 1;
    }
    Req = LvOpenRequest(&G, Lv, Crypt, Letter, &R, Size, &Bytes);
    RunsFree(&R);
    LvmClose(&G);
    if (Req == NULL) {
        return 1;
    }

    Error = DriverControl(IOCTL_APP_LV_OPEN, Req, (DWORD)Bytes, Req, FIELD_OFFSET(EXT4_LV_OPEN, Run));
    if (Error != NO_ERROR) {
        if (Error != EXT4CTL_NO_DRIVER) {
            Fail("the driver refused %s (error %lu)", Req->Name, Error);
        }
        HeapFree(GetProcessHeap(), 0, Req);
        return 1;
    }
    LvDevice(Req->Index, Device, ARRAYSIZE(Device));
    Say("%s opened read-only as %ls (%llu MiB, %lu runs)", Req->Name, Device, Size >> 20, Req->Runs);
    HeapFree(GetProcessHeap(), 0, Req);

    Mounted = WaitForMount(Device, Fs, ARRAYSIZE(Fs));
    if (Mounted) {
        Say("mounted as %lc: (%ls, read-only)", Mounted, Fs);
    } else {
        Say("no mounted drive letter yet");
    }
    return 0;
}

int
CmdLvClose(const WCHAR *Which, BOOL Force)
{
    EXT4_LV_QUERY   Q;
    EXT4_LV_CLOSE   Req;
    DWORD           Error;
    ULONG           i;
    BOOL            Found = FALSE;

    Error = LvQuery(&Q);
    if (Error != NO_ERROR) {
        if (Error != EXT4CTL_NO_DRIVER) {
            Fail("query failed (error %lu)", Error);
        }
        return 1;
    }
    memset(&Req, 0, sizeof(Req));
    Req.Magic = EXT4_CRYPT_MAGIC;
    Req.Flags = Force ? EXT4_CRYPT_FORCE : 0;
    for (i = 0; i < Q.Count && !Found; i++) {
        WCHAR Device[64];
        LvDevice(Q.Entries[i].Index, Device, ARRAYSIZE(Device));
        if (LetterNames(Which, Device) ||
            (iswdigit(Which[0]) && Q.Entries[i].Index == (ULONG)_wtoi(Which))) {
            Req.Index = Q.Entries[i].Index;
            Found = TRUE;
        }
    }
    if (!Found) {
        Fail("%ls is not an open LVM volume (see ext4ctl status)", Which);
        return 1;
    }
    Error = DriverControl(IOCTL_APP_LV_CLOSE, &Req, sizeof(Req), &Req, sizeof(Req));
    if (Error != NO_ERROR) {
        if (Error != EXT4CTL_NO_DRIVER) {
            Fail(Error == ERROR_BUSY ? "the volume is in use; close its files or use --force" :
                 "close failed (error %lu)", Error);
        }
        return 1;
    }
    Say("\\Device\\Ext4Lv%lu closed", Req.Index);
    return 0;
}

/* for ext4ctl status */
void
LvStatus(void)
{
    EXT4_LV_QUERY   Q;
    ULONG           i;

    if (LvQuery(&Q) != NO_ERROR) {
        return;
    }
    for (i = 0; i < Q.Count; i++) {
        WCHAR Device[64], L;

        LvDevice(Q.Entries[i].Index, Device, ARRAYSIZE(Device));
        L = DeviceLetter(Device);
        Say("lv %lu  %lc%s  %s on \\Device\\Ext4Crypt%lu  %.2f G, %lu runs, read-only",
            Q.Entries[i].Index, L ? L : L'-', L ? ":" : " ", Q.Entries[i].Name, Q.Entries[i].Crypt,
            Q.Entries[i].Size / EXT4CTL_GIB, Q.Entries[i].Runs);
    }
}
