/**
 * lvm_vg.c - the volume group inside an unlocked LUKS volume: label, metadata, linear volumes.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Reads, never writes:
 *
 *   PV label ("LABELONE", one of the first four sectors) -> PV header ->
 *   metadata area -> the current copy of the VG's text metadata (a circular
 *   buffer; the copy may wrap) -> the logical volumes and their segments.
 *
 * Linear (striped, one stripe) segments map extents of the LV to extents of
 * the PV directly; thin volumes are mapped in lvm_thin.c. The result is a
 * table of runs over the LUKS device, contiguous pieces merged, which the
 * driver serves read-only (volume\lvm.c).
 */

#include "lvm_internal.h"

static const char LvmLabelId[] = "LABELONE";
static const char LvmLabelType[] = "LVM2 001";
static const char LvmMdaMagic[] = " LVM2 x[5A%r0N*>";

void
LvmClose(LVM_VG *G)
{
    LvmFreeText(&G->Parse);
    if (G->Text) {
        HeapFree(GetProcessHeap(), 0, G->Text);
    }
    LuksCloseDevice(&G->Dev);
    memset(G, 0, sizeof(*G));
}

/* the label in one of the first sectors, NULL if this is no LVM PV */
static const LVM_LABEL_HEADER *
LvmFindLabel(const UINT8 *Sectors)
{
    UINT32 i;

    for (i = 0; i < LVM_LABEL_SCAN; i++) {
        const LVM_LABEL_HEADER *Label = (const LVM_LABEL_HEADER *)(Sectors + i * LVM_SECTOR);
        if (memcmp(Label->Id, LvmLabelId, sizeof(Label->Id)) == 0 &&
            memcmp(Label->Type, LvmLabelType, sizeof(Label->Type)) == 0) {
            return Label;
        }
    }
    return NULL;
}

/* the first metadata area of the PV header: past the data areas and their terminator */
static BOOL
LvmMetadataArea(const LVM_PV_HEADER *Pv, const UINT8 *End, UINT64 *Offset, UINT64 *Size)
{
    const LVM_DISK_LOCN *Loc = Pv->Areas;

    while ((const UINT8 *)(Loc + 1) <= End && (Le64(Loc->Offset) || Le64(Loc->Size))) {
        Loc++;
    }
    Loc++;
    if ((const UINT8 *)(Loc + 1) > End || Le64(Loc->Offset) == 0) {
        return FALSE;
    }
    *Offset = Le64(Loc->Offset);
    *Size = Le64(Loc->Size);
    return TRUE;
}

/* the current copy of the text, which may wrap round to right after the header sector */
static BOOL
LvmReadText(LVM_VG *G, UINT64 MdaOffset, UINT64 MdaSize, UINT64 TextOffset, UINT64 TextSize)
{
    G->Text = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)TextSize + 1);
    if (G->Text == NULL) {
        return FALSE;
    }
    if (TextOffset + TextSize <= MdaSize) {
        if (!LuksReadAt(&G->Dev, MdaOffset + TextOffset, G->Text, (ULONG)TextSize)) {
            return FALSE;
        }
    } else {
        UINT64 First = MdaSize - TextOffset;
        if (!LuksReadAt(&G->Dev, MdaOffset + TextOffset, G->Text, (ULONG)First) ||
            !LuksReadAt(&G->Dev, MdaOffset + LVM_MDA_HEADER_BYTES, G->Text + First,
                        (ULONG)(TextSize - First))) {
            return FALSE;
        }
    }
    G->Text[TextSize] = 0;
    return TRUE;
}

/* this device's PV in the VG: the one whose id is the label's uuid (dashes in the text) */
static BOOL
LvmFindPv(LVM_VG *G, const LVM_PV_HEADER *Pv)
{
    const LVM_NODE *Pvs = LvmGet(G->Vg, "physical_volumes"), *P;
    UINT64          Extent, PeStart;
    char            Id[2 * LVM_PV_ID_CHARS];   /* a longer id must not match by its start */

    if (!LvmU64(G->Vg, "extent_size", &Extent) || Pvs == NULL) {
        Fail("LVM metadata without extent size or PVs");
        return FALSE;
    }
    G->ExtentBytes = Extent * LVM_SECTOR;
    for (P = Pvs->Child; P; P = P->Next) {
        const char *PvId = LvmStr(P, "id");
        const char *s;
        size_t      k = 0;

        if (PvId == NULL) {
            continue;
        }
        for (s = PvId; *s && k < sizeof(Id) - 1; s++) {
            if (*s != '-') {
                Id[k++] = *s;
            }
        }
        Id[k] = 0;
        if (k == LVM_PV_ID_CHARS && memcmp(Id, Pv->Uuid, LVM_PV_ID_CHARS) == 0 &&
            LvmU64(P, "pe_start", &PeStart)) {
            G->PvName = P->Name;
            G->PeStart = PeStart * LVM_SECTOR;
            return TRUE;
        }
    }
    Fail("the PV of this device is not in the VG metadata");
    return FALSE;
}

/* label, PV header, metadata area, current text; parsed. Quiet on "no LVM". */
BOOL
LvmLoad(LVM_VG *G, ULONG CryptIndex, BOOL Quiet)
{
    WCHAR                   Name[64];
    UINT8                   Sectors[LVM_LABEL_SCAN * LVM_SECTOR];
    UINT8                   MdaSector[LVM_MDA_HEADER_BYTES];
    const LVM_MDA_HEADER   *Mda = (const LVM_MDA_HEADER *)MdaSector;
    const LVM_LABEL_HEADER *Label;
    const LVM_PV_HEADER    *Pv;
    UINT64                  MdaOffset, MdaSize, TextOffset, TextSize;
    const LVM_NODE         *n;

    memset(G, 0, sizeof(*G));
    swprintf_s(Name, ARRAYSIZE(Name), EXT4_CRYPT_DEVICE_PREFIX L"%lu", CryptIndex);
    if (!LuksOpenDevice(&G->Dev, Name)) {
        Fail("cannot open %ls (error %lu); is it unlocked? (ext4ctl status)", Name, GetLastError());
        return FALSE;
    }
    if (!LuksReadAt(&G->Dev, 0, Sectors, sizeof(Sectors))) {
        Fail("cannot read %ls", Name);
        return FALSE;
    }
    Label = LvmFindLabel(Sectors);
    if (Label == NULL) {
        if (!Quiet) {
            Fail("no LVM physical volume inside \\Device\\Ext4Crypt%lu", CryptIndex);
        }
        return FALSE;
    }

    Pv = (const LVM_PV_HEADER *)((const UINT8 *)Label + Le32(Label->Offset));
    if ((const UINT8 *)Pv < (const UINT8 *)Label ||
        (const UINT8 *)Pv->Areas > Sectors + sizeof(Sectors)) {
        return FALSE;
    }
    if (!LvmMetadataArea(Pv, Sectors + sizeof(Sectors), &MdaOffset, &MdaSize)) {
        Fail("the PV has no metadata area");
        return FALSE;
    }

    if (!LuksReadAt(&G->Dev, MdaOffset, MdaSector, sizeof(MdaSector)) ||
        memcmp(Mda->Magic, LvmMdaMagic, sizeof(Mda->Magic)) != 0) {
        Fail("LVM metadata header not found");
        return FALSE;
    }
    TextOffset = Le64(Mda->Raw[0].Offset);
    TextSize = Le64(Mda->Raw[0].Size);
    if (TextSize == 0 || TextSize > LVM_MAX_TEXT || TextOffset >= MdaSize) {
        Fail("LVM metadata has no current copy");
        return FALSE;
    }
    if (!LvmReadText(G, MdaOffset, MdaSize, TextOffset, TextSize) ||
        !LvmParseText(&G->Parse, G->Text, (size_t)TextSize)) {
        return FALSE;
    }

    /* the VG is the one section at the top */
    for (n = G->Parse.Root->Child; n; n = n->Next) {
        if (n->Kind == LVM_SECTION) {
            G->Vg = n;
            G->VgName = n->Name;
            break;
        }
    }
    if (G->Vg == NULL) {
        Fail("cannot parse the LVM metadata");
        return FALSE;
    }
    return LvmFindPv(G, Pv);
}

const LVM_NODE *
LvmFindLv(LVM_VG *G, const char *Name)
{
    const LVM_NODE *Lvs = LvmGet(G->Vg, "logical_volumes");
    const LVM_NODE *n;

    for (n = Lvs ? Lvs->Child : NULL; n; n = n->Next) {
        if (n->Kind == LVM_SECTION && strcmp(n->Name, Name) == 0) {
            return n;
        }
    }
    return NULL;
}

/*
 * Byte Off of linear LV Lv to a byte on the PV. Every segment must be a
 * single stripe on this PV. *Left: bytes contiguous from there.
 */
BOOL
LvmLinearToPv(LVM_VG *G, const LVM_NODE *Lv, UINT64 Off, UINT64 *Pv, UINT64 *Left)
{
    const LVM_NODE *Seg;

    for (Seg = Lv->Child; Seg; Seg = Seg->Next) {
        UINT64          Start, Count, Stripes, Pe;
        const LVM_NODE *List;
        const char     *Type;

        if (!LvmIsSegment(Seg)) {
            continue;
        }
        if (!LvmU64(Seg, "start_extent", &Start) || !LvmU64(Seg, "extent_count", &Count)) {
            return FALSE;
        }
        if (Off < Start * G->ExtentBytes || Off >= (Start + Count) * G->ExtentBytes) {
            continue;
        }

        /* "stripes" = ["pv0", 0]: the PV and the first extent on it */
        Type = LvmStr(Seg, "type");
        List = LvmGet(Seg, "stripes");
        if (Type == NULL || strcmp(Type, "striped") != 0 || !LvmU64(Seg, "stripe_count", &Stripes) ||
            Stripes != 1 || List == NULL || List->Child == NULL || List->Child->Next == NULL ||
            strcmp(List->Child->Value, G->PvName) != 0 ||
            !LvmDecimal(List->Child->Next->Value, &Pe)) {
            return FALSE;                   /* striped, mirrored or on another PV */
        }
        *Pv = G->PeStart + Pe * G->ExtentBytes + (Off - Start * G->ExtentBytes);
        *Left = (Start + Count) * G->ExtentBytes - Off;
        return TRUE;
    }
    return FALSE;
}

/* the one segment of an LV that has a type other than striped (pool, thin) */
const LVM_NODE *
LvmOnlySegment(const LVM_NODE *Lv, UINT64 *Extents)
{
    const LVM_NODE *Seg, *Found = NULL;

    for (Seg = Lv->Child; Seg; Seg = Seg->Next) {
        if (LvmIsSegment(Seg)) {
            if (Found) {
                return NULL;
            }
            Found = Seg;
        }
    }
    if (Found && Extents && !LvmU64(Found, "extent_count", Extents)) {
        return NULL;
    }
    return Found;
}

LV_TYPE
LvmType(const LVM_NODE *Lv, UINT64 ExtentBytes, UINT64 *Size)
{
    const LVM_NODE *Seg;
    UINT64          Extents = 0, Start, Count;
    BOOL            Linear = TRUE, Thin = FALSE;

    for (Seg = Lv->Child; Seg; Seg = Seg->Next) {
        const char *Type;

        if (!LvmIsSegment(Seg)) {
            continue;
        }
        Type = LvmStr(Seg, "type");
        if (!LvmU64(Seg, "start_extent", &Start) || !LvmU64(Seg, "extent_count", &Count) || Type == NULL) {
            return LV_OTHER;
        }
        Extents = max(Extents, Start + Count);
        if (strcmp(Type, "thin") == 0) {
            Thin = TRUE;
        } else if (strcmp(Type, "striped") != 0) {
            Linear = FALSE;
        }
    }
    *Size = Extents * ExtentBytes;
    if (Thin) {
        return LvmOnlySegment(Lv, NULL) ? LV_THIN : LV_OTHER;
    }
    return Linear && Extents ? LV_LINEAR : LV_OTHER;
}

/* ---------------------------------------------------------------- runs */

void
RunsAdd(RUNS *R, UINT64 Start, UINT64 Length, UINT64 Target)
{
    if (R->Failed || Length == 0) {
        return;
    }
    if (R->Count) {
        EXT4_LV_RUN *Last = &R->Run[R->Count - 1];
        if (Last->Start + Last->Length == Start &&
            ((Target == EXT4_LV_HOLE && Last->Target == EXT4_LV_HOLE) ||
             (Target != EXT4_LV_HOLE && Last->Target != EXT4_LV_HOLE &&
              Last->Target + Last->Length == Target))) {
            Last->Length += Length;
            return;
        }
    }
    if (R->Count == R->Room) {
        ULONG        Room = R->Room ? R->Room * 2 : EXT4CTL_FIRST_RUNS;
        EXT4_LV_RUN *New;

        if (Room > EXT4_LV_MAX_RUNS) {
            R->Failed = TRUE;
            return;
        }
        New = (EXT4_LV_RUN *)(R->Run ? HeapReAlloc(GetProcessHeap(), 0, R->Run, Room * sizeof(EXT4_LV_RUN)) :
                                       HeapAlloc(GetProcessHeap(), 0, Room * sizeof(EXT4_LV_RUN)));
        if (New == NULL) {
            R->Failed = TRUE;
            return;
        }
        R->Run = New;
        R->Room = Room;
    }
    R->Run[R->Count].Start = Start;
    R->Run[R->Count].Length = Length;
    R->Run[R->Count].Target = Target;
    R->Count++;
}

void
RunsFree(RUNS *R)
{
    if (R->Run) {
        HeapFree(GetProcessHeap(), 0, R->Run);
    }
    memset(R, 0, sizeof(*R));
}

/* a linear LV: its segments, one run each (merged where they touch) */
static BOOL
LvmLinearRuns(LVM_VG *G, const LVM_NODE *Lv, UINT64 Size, RUNS *R)
{
    UINT64 Off = 0;

    while (Off < Size) {
        UINT64 Pv, Left;
        if (!LvmLinearToPv(G, Lv, Off, &Pv, &Left)) {
            return FALSE;
        }
        Left = min(Left, Size - Off);
        RunsAdd(R, Off, Left, Pv);
        Off += Left;
    }
    return !R->Failed;
}

/* runs of an LV; Size out */
BOOL
LvmRuns(LVM_VG *G, const LVM_NODE *Lv, RUNS *R, UINT64 *Size, ULONG *Mapped)
{
    LV_TYPE Type = LvmType(Lv, G->ExtentBytes, Size);

    memset(R, 0, sizeof(*R));
    *Mapped = 0;
    if (Type == LV_LINEAR) {
        return LvmLinearRuns(G, Lv, *Size, R);
    }
    if (Type == LV_THIN) {
        return LvmThinRuns(G, Lv, *Size, R, Mapped);
    }
    Fail("%s: only linear and thin volumes are supported", Lv->Name);
    return FALSE;
}

/* read Length bytes at Off of the volume through its runs (holes: zeros) */
static BOOL
RunsRead(LVM_VG *G, const RUNS *R, UINT64 Off, UINT8 *Out, ULONG Length)
{
    ULONG i;

    for (i = 0; i < R->Count && Length; i++) {
        const EXT4_LV_RUN *Run = &R->Run[i];
        if (Off >= Run->Start && Off < Run->Start + Run->Length) {
            ULONG n = (ULONG)min((UINT64)Length, Run->Start + Run->Length - Off);
            if (Run->Target == EXT4_LV_HOLE) {
                memset(Out, 0, n);
            } else if (!LuksReadAt(&G->Dev, Run->Target + (Off - Run->Start), Out, n)) {
                return FALSE;
            }
            Out += n;
            Off += n;
            Length -= n;
        }
    }
    return Length == 0;
}

const char *
LvmFsName(LVM_VG *G, const RUNS *R)
{
    UINT8 Super[2];

    if (RunsRead(G, R, EXT2_SUPER_MAGIC_OFFSET, Super, sizeof(Super)) &&
        (Super[0] | (Super[1] << 8)) == EXT2_SUPER_MAGIC) {
        return "ext2/3/4";
    }
    return "-";
}
