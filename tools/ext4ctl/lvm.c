/**
 * lvm.c - LVM2 inside an unlocked LUKS volume: metadata, linear and thin volumes.
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
 *   Linear (striped, one stripe) segments map extents of the LV to extents
 *   of the PV directly. A thin volume lives in a pool: its chunks are found
 *   through dm-thin's metadata (the pool's _tmeta LV, 4 KiB blocks) - the
 *   superblock gives the root of a two-level btree, device id -> a btree of
 *   virtual block -> (data block << 24 | time); the data block is a chunk
 *   of the pool's _tdata LV. Chunks never written are holes (zeros).
 *
 * The result is a table of runs over the LUKS device, contiguous chunks
 * merged, which the driver serves read-only (volume\lvm.c).
 */

#include "ext4ctl.h"

#define LVM_SECTOR          512
#define LVM_LABEL_SCAN      4                   /* the label is in one of the first sectors */
#define LVM_MDA_HEADER      512
#define LVM_MAX_TEXT        (8 * 1024 * 1024)
#define THIN_BLOCK          4096
#define THIN_MAGIC          27022010ULL
#define THIN_INTERNAL_NODE  1
#define THIN_LEAF_NODE      2
#define THIN_MAX_DEPTH      16
#define THIN_TIME_BITS      24                  /* mapping value: data block << 24 | time */
#define LVM_GIB             1073741824.0

static const char LvmMdaMagic[] = " LVM2 x[5A%r0N*>";     /* 16 bytes, no NUL on disk */

/* ---------------------------------------------------------------- text metadata */

typedef enum { LVM_SECTION, LVM_VALUE, LVM_LIST } LVM_KIND;

typedef struct _LVM_NODE {
    LVM_KIND            Kind;
    const char         *Name;               /* NULL for list items */
    const char         *Value;              /* LVM_VALUE: text or number */
    struct _LVM_NODE   *Child;              /* LVM_SECTION, LVM_LIST */
    struct _LVM_NODE   *Next;
} LVM_NODE;

typedef struct _LVM_TEXT {
    char       *Arena;
    size_t      Used, Size;
    const char *p, *End;
    int         Depth;
    LVM_NODE   *Root;                       /* the top-level assignments and sections */
} LVM_TEXT;

static void *
TextTake(LVM_TEXT *T, size_t Size)
{
    void *Block;

    Size = (Size + 7) & ~(size_t)7;
    if (T->Used + Size > T->Size) {
        return NULL;
    }
    Block = T->Arena + T->Used;
    T->Used += Size;
    memset(Block, 0, Size);
    return Block;
}

static void
TextSkip(LVM_TEXT *T)
{
    while (T->p < T->End) {
        if (*T->p == '#') {
            while (T->p < T->End && *T->p != '\n') T->p++;
        } else if (*T->p == ' ' || *T->p == '\t' || *T->p == '\r' || *T->p == '\n') {
            T->p++;
        } else {
            break;
        }
    }
}

static BOOL
IsWordChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '+' || c == '-';
}

/* a word or a quoted string, copied into the arena */
static const char *
TextToken(LVM_TEXT *T, BOOL *Quoted)
{
    const char *Start;
    char       *Out, *o;

    TextSkip(T);
    *Quoted = FALSE;
    if (T->p >= T->End) {
        return NULL;
    }
    if (*T->p == '"') {
        Start = ++T->p;
        while (T->p < T->End && *T->p != '"') {
            if (*T->p == '\\') T->p++;
            T->p++;
        }
        if (T->p >= T->End) return NULL;
        Out = o = (char *)TextTake(T, (size_t)(T->p - Start) + 1);
        if (Out == NULL) return NULL;
        for (const char *s = Start; s < T->p; s++) {
            if (*s == '\\' && s + 1 < T->p) s++;
            *o++ = *s;
        }
        T->p++;
        *Quoted = TRUE;
        return Out;
    }
    Start = T->p;
    while (T->p < T->End && IsWordChar(*T->p)) T->p++;
    if (T->p == Start) return NULL;
    Out = (char *)TextTake(T, (size_t)(T->p - Start) + 1);
    if (Out) memcpy(Out, Start, (size_t)(T->p - Start));
    return Out;
}

/* the members of a section up to '}' (or the end of the text at the top) */
static LVM_NODE *
TextSection(LVM_TEXT *T, BOOL Top)
{
    LVM_NODE *First = NULL, **Tail = &First;

    if (++T->Depth > 32) return NULL;
    for (;;) {
        const char *Name;
        BOOL        Quoted;
        LVM_NODE   *Node;

        TextSkip(T);
        if (T->p >= T->End) {
            if (!Top) return NULL;
            break;
        }
        if (*T->p == '}') {
            if (Top) return NULL;
            T->p++;
            break;
        }
        Name = TextToken(T, &Quoted);
        if (Name == NULL || (Node = (LVM_NODE *)TextTake(T, sizeof(LVM_NODE))) == NULL) {
            return NULL;
        }
        Node->Name = Name;
        TextSkip(T);
        if (T->p < T->End && *T->p == '{') {
            T->p++;
            Node->Kind = LVM_SECTION;
            Node->Child = TextSection(T, FALSE);
            if (Node->Child == NULL && T->p > T->End) return NULL;
        } else if (T->p < T->End && *T->p == '=') {
            T->p++;
            TextSkip(T);
            if (T->p < T->End && *T->p == '[') {
                LVM_NODE **ItemTail = &Node->Child;
                T->p++;
                Node->Kind = LVM_LIST;
                for (;;) {
                    LVM_NODE *Item;
                    TextSkip(T);
                    if (T->p < T->End && *T->p == ']') { T->p++; break; }
                    if (T->p < T->End && *T->p == ',') { T->p++; continue; }
                    Item = (LVM_NODE *)TextTake(T, sizeof(LVM_NODE));
                    if (Item == NULL || (Item->Value = TextToken(T, &Quoted)) == NULL) return NULL;
                    Item->Kind = LVM_VALUE;
                    *ItemTail = Item;
                    ItemTail = &Item->Next;
                }
            } else {
                Node->Kind = LVM_VALUE;
                if ((Node->Value = TextToken(T, &Quoted)) == NULL) return NULL;
            }
        } else {
            return NULL;
        }
        *Tail = Node;
        Tail = &Node->Next;
    }
    T->Depth--;
    return First ? First : (LVM_NODE *)TextTake(T, sizeof(LVM_NODE));   /* empty section: a dummy */
}

static const LVM_NODE *
LvmGet(const LVM_NODE *Section, const char *Name)
{
    const LVM_NODE *n;
    for (n = Section ? Section->Child : NULL; n; n = n->Next) {
        if (n->Name && strcmp(n->Name, Name) == 0) return n;
    }
    return NULL;
}

static BOOL
LvmU64(const LVM_NODE *Section, const char *Name, UINT64 *Out)
{
    const LVM_NODE *n = LvmGet(Section, Name);
    UINT64 v = 0;
    const char *s;

    if (n == NULL || n->Kind != LVM_VALUE || n->Value == NULL || *n->Value == 0) return FALSE;
    for (s = n->Value; *s; s++) {
        if (*s < '0' || *s > '9') return FALSE;
        v = v * 10 + (UINT64)(*s - '0');
    }
    *Out = v;
    return TRUE;
}

static const char *
LvmStr(const LVM_NODE *Section, const char *Name)
{
    const LVM_NODE *n = LvmGet(Section, Name);
    return (n && n->Kind == LVM_VALUE) ? n->Value : NULL;
}

/* is Flag among the items of list Name ("status" = ["READ", "VISIBLE"]) */
static BOOL
LvmHasFlag(const LVM_NODE *Section, const char *Name, const char *Flag)
{
    const LVM_NODE *l = LvmGet(Section, Name), *i;
    for (i = (l && l->Kind == LVM_LIST) ? l->Child : NULL; i; i = i->Next) {
        if (i->Value && strcmp(i->Value, Flag) == 0) return TRUE;
    }
    return FALSE;
}

/* ---------------------------------------------------------------- the volume group */

typedef struct _LVM_VG {
    LUKS_DEVICE     Dev;                    /* the LUKS device = the PV */
    char           *Text;
    LVM_TEXT        Parse;
    const LVM_NODE *Vg;                     /* the VG section */
    const char     *VgName;
    const char     *PvName;                 /* "pv0": the one on this device */
    UINT64          ExtentBytes;
    UINT64          PeStart;                /* bytes */
} LVM_VG;

static UINT32 Le32(const UINT8 *p) { UINT32 v; memcpy(&v, p, 4); return v; }
static UINT64 Le64(const UINT8 *p) { UINT64 v; memcpy(&v, p, 8); return v; }

static void
LvmClose(LVM_VG *G)
{
    if (G->Parse.Arena) HeapFree(GetProcessHeap(), 0, G->Parse.Arena);
    if (G->Text) HeapFree(GetProcessHeap(), 0, G->Text);
    LuksCloseDevice(&G->Dev);
    memset(G, 0, sizeof(*G));
}

/* label, PV header, metadata area, current text; parsed. Quiet on "no LVM". */
static BOOL
LvmLoad(LVM_VG *G, ULONG CryptIndex, BOOL Quiet)
{
    WCHAR       Name[64];
    UINT8       Sectors[LVM_LABEL_SCAN * LVM_SECTOR], Mda[LVM_MDA_HEADER];
    const UINT8 *Label = NULL, *Pv, *Loc;
    UINT64      MdaOffset = 0, MdaSize = 0, TextOffset, TextSize;
    UINT32      i;
    const LVM_NODE *n;

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
    for (i = 0; i < LVM_LABEL_SCAN; i++) {
        if (memcmp(Sectors + i * LVM_SECTOR, "LABELONE", 8) == 0 &&
            memcmp(Sectors + i * LVM_SECTOR + 24, "LVM2 001", 8) == 0) {
            Label = Sectors + i * LVM_SECTOR;
            break;
        }
    }
    if (Label == NULL) {
        if (!Quiet) Fail("no LVM physical volume inside \\Device\\Ext4Crypt%lu", CryptIndex);
        return FALSE;
    }

    /* PV header: uuid[32], device size, data areas {offset, size}... 0, metadata areas ... 0 */
    Pv = Label + Le32(Label + 20);
    if (Pv < Label || Pv + 40 > Sectors + sizeof(Sectors)) return FALSE;
    for (Loc = Pv + 40; Loc + 16 <= Sectors + sizeof(Sectors) && (Le64(Loc) || Le64(Loc + 8)); Loc += 16) {
    }
    Loc += 16;                              /* past the terminator: the metadata areas */
    if (Loc + 16 > Sectors + sizeof(Sectors) || Le64(Loc) == 0) {
        Fail("the PV has no metadata area");
        return FALSE;
    }
    MdaOffset = Le64(Loc);
    MdaSize = Le64(Loc + 8);

    if (!LuksReadAt(&G->Dev, MdaOffset, Mda, sizeof(Mda)) || memcmp(Mda + 4, LvmMdaMagic, sizeof(LvmMdaMagic) - 1) != 0) {
        Fail("LVM metadata header not found");
        return FALSE;
    }
    /* raw_locn[0]: the current copy, relative to the area */
    TextOffset = Le64(Mda + 40);
    TextSize = Le64(Mda + 48);
    if (TextSize == 0 || TextSize > LVM_MAX_TEXT || TextOffset >= MdaSize) {
        Fail("LVM metadata has no current copy");
        return FALSE;
    }
    G->Text = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)TextSize + 1);
    if (G->Text == NULL) return FALSE;
    if (TextOffset + TextSize <= MdaSize) {
        if (!LuksReadAt(&G->Dev, MdaOffset + TextOffset, G->Text, (ULONG)TextSize)) return FALSE;
    } else {
        /* wrapped: the rest continues right after the header sector */
        UINT64 First = MdaSize - TextOffset;
        if (!LuksReadAt(&G->Dev, MdaOffset + TextOffset, G->Text, (ULONG)First) ||
            !LuksReadAt(&G->Dev, MdaOffset + LVM_MDA_HEADER, G->Text + First, (ULONG)(TextSize - First))) {
            return FALSE;
        }
    }
    G->Text[TextSize] = 0;

    G->Parse.Size = (SIZE_T)TextSize * 4 + 4096;
    G->Parse.Arena = (char *)HeapAlloc(GetProcessHeap(), 0, G->Parse.Size);
    if (G->Parse.Arena == NULL) return FALSE;
    G->Parse.p = G->Text;
    G->Parse.End = G->Text + strnlen(G->Text, (size_t)TextSize);
    G->Parse.Root = (LVM_NODE *)TextTake(&G->Parse, sizeof(LVM_NODE));
    if (G->Parse.Root == NULL) return FALSE;
    G->Parse.Root->Kind = LVM_SECTION;
    G->Parse.Root->Child = TextSection(&G->Parse, TRUE);

    /* the VG is the one section at the top */
    for (n = G->Parse.Root->Child; n; n = n->Next) {
        if (n->Kind == LVM_SECTION) { G->Vg = n; G->VgName = n->Name; break; }
    }
    if (G->Vg == NULL) {
        Fail("cannot parse the LVM metadata");
        return FALSE;
    }
    {
        UINT64 Extent, PeStart;
        const LVM_NODE *Pvs = LvmGet(G->Vg, "physical_volumes"), *P;
        char  Id[40];

        if (!LvmU64(G->Vg, "extent_size", &Extent) || Pvs == NULL) {
            Fail("LVM metadata without extent size or PVs");
            return FALSE;
        }
        G->ExtentBytes = Extent * LVM_SECTOR;
        /* this device's PV: the one whose id is the label's uuid (dashes in the text) */
        for (P = Pvs->Child; P; P = P->Next) {
            const char *PvId = LvmStr(P, "id");
            const char *s;
            size_t      k = 0;
            if (PvId == NULL) continue;
            for (s = PvId; *s && k < sizeof(Id) - 1; s++) if (*s != '-') Id[k++] = *s;
            Id[k] = 0;
            if (k == 32 && memcmp(Id, Pv, 32) == 0 && LvmU64(P, "pe_start", &PeStart)) {
                G->PvName = P->Name;
                G->PeStart = PeStart * LVM_SECTOR;
                break;
            }
        }
        if (G->PvName == NULL) {
            Fail("the PV of this device is not in the VG metadata");
            return FALSE;
        }
    }
    return TRUE;
}

static const LVM_NODE *
LvmFindLv(LVM_VG *G, const char *Name)
{
    const LVM_NODE *Lvs = LvmGet(G->Vg, "logical_volumes");
    const LVM_NODE *n;
    for (n = Lvs ? Lvs->Child : NULL; n; n = n->Next) {
        if (n->Kind == LVM_SECTION && strcmp(n->Name, Name) == 0) return n;
    }
    return NULL;
}

/*
 * Byte Off of linear LV Lv to a byte on the PV. Every segment must be a
 * single stripe on this PV. *Left: bytes contiguous from there.
 */
static BOOL
LvmLinearToPv(LVM_VG *G, const LVM_NODE *Lv, UINT64 Off, UINT64 *Pv, UINT64 *Left)
{
    const LVM_NODE *Seg;

    for (Seg = Lv->Child; Seg; Seg = Seg->Next) {
        UINT64 Start, Count, Stripes, Pe;
        const LVM_NODE *List;
        const char *Type;

        if (Seg->Kind != LVM_SECTION || strncmp(Seg->Name, "segment", 7) != 0) continue;
        if (!LvmU64(Seg, "start_extent", &Start) || !LvmU64(Seg, "extent_count", &Count)) return FALSE;
        if (Off < Start * G->ExtentBytes || Off >= (Start + Count) * G->ExtentBytes) continue;

        Type = LvmStr(Seg, "type");
        List = LvmGet(Seg, "stripes");
        if (Type == NULL || strcmp(Type, "striped") != 0 || !LvmU64(Seg, "stripe_count", &Stripes) ||
            Stripes != 1 || List == NULL || List->Child == NULL || List->Child->Next == NULL ||
            strcmp(List->Child->Value, G->PvName) != 0) {
            return FALSE;                   /* striped, mirrored or on another PV */
        }
        {
            const char *s; Pe = 0;
            for (s = List->Child->Next->Value; *s; s++) {
                if (*s < '0' || *s > '9') return FALSE;
                Pe = Pe * 10 + (UINT64)(*s - '0');
            }
        }
        *Pv = G->PeStart + Pe * G->ExtentBytes + (Off - Start * G->ExtentBytes);
        *Left = (Start + Count) * G->ExtentBytes - Off;
        return TRUE;
    }
    return FALSE;
}

/* the one segment of an LV that has a type other than striped (pool, thin) */
static const LVM_NODE *
LvmOnlySegment(const LVM_NODE *Lv, UINT64 *Extents)
{
    const LVM_NODE *Seg, *Found = NULL;
    for (Seg = Lv->Child; Seg; Seg = Seg->Next) {
        if (Seg->Kind == LVM_SECTION && strncmp(Seg->Name, "segment", 7) == 0) {
            if (Found) return NULL;
            Found = Seg;
        }
    }
    if (Found && Extents && !LvmU64(Found, "extent_count", Extents)) return NULL;
    return Found;
}

/* ---------------------------------------------------------------- runs */

typedef struct _RUNS {
    EXT4_LV_RUN *Run;
    ULONG        Count, Room;
    BOOL         Failed;
} RUNS;

static void
RunsAdd(RUNS *R, UINT64 Start, UINT64 Length, UINT64 Target)
{
    if (R->Failed || Length == 0) return;
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
        ULONG Room = R->Room ? R->Room * 2 : 1024;
        EXT4_LV_RUN *New;
        if (Room > EXT4_LV_MAX_RUNS) { R->Failed = TRUE; return; }
        New = (EXT4_LV_RUN *)(R->Run ? HeapReAlloc(GetProcessHeap(), 0, R->Run, Room * sizeof(EXT4_LV_RUN)) :
                                       HeapAlloc(GetProcessHeap(), 0, Room * sizeof(EXT4_LV_RUN)));
        if (New == NULL) { R->Failed = TRUE; return; }
        R->Run = New;
        R->Room = Room;
    }
    R->Run[R->Count].Start = Start;
    R->Run[R->Count].Length = Length;
    R->Run[R->Count].Target = Target;
    R->Count++;
}

/* a linear LV: its segments, one run each (merged where they touch) */
static BOOL
LvmLinearRuns(LVM_VG *G, const LVM_NODE *Lv, UINT64 Size, RUNS *R)
{
    UINT64 Off = 0;
    while (Off < Size) {
        UINT64 Pv, Left;
        if (!LvmLinearToPv(G, Lv, Off, &Pv, &Left)) return FALSE;
        Left = min(Left, Size - Off);
        RunsAdd(R, Off, Left, Pv);
        Off += Left;
    }
    return !R->Failed;
}

/* ---------------------------------------------------------------- dm-thin */

typedef struct _THIN {
    LVM_VG         *G;
    const LVM_NODE *Meta;                   /* the pool's _tmeta LV (linear) */
    const LVM_NODE *Data;                   /* the pool's _tdata LV (linear) */
    UINT64          ChunkBytes;
    UINT8           Block[THIN_BLOCK];
    RUNS           *R;
    UINT64          VolumeChunks;
    UINT64          NextChunk;              /* the first chunk not yet in R */
    ULONG           Mapped;
} THIN;

static BOOL
ThinRead(THIN *T, UINT64 Block)
{
    UINT64 Pv, Left;
    if (!LvmLinearToPv(T->G, T->Meta, Block * THIN_BLOCK, &Pv, &Left) || Left < THIN_BLOCK) return FALSE;
    return LuksReadAt(&T->G->Dev, Pv, T->Block, THIN_BLOCK);
}

/* a btree node, sanity-checked; the buffer is T->Block */
static BOOL
ThinNode(THIN *T, UINT64 Block, UINT32 *Flags, UINT32 *Entries, UINT32 *MaxEntries, UINT32 *ValueSize)
{
    if (!ThinRead(T, Block) || Le64(T->Block + 8) != Block) return FALSE;
    *Flags = Le32(T->Block + 4);
    *Entries = Le32(T->Block + 16);
    *MaxEntries = Le32(T->Block + 20);
    *ValueSize = Le32(T->Block + 24);
    return (*Flags == THIN_INTERNAL_NODE || *Flags == THIN_LEAF_NODE) &&
           *Entries <= *MaxEntries && *MaxEntries > 0 && *ValueSize == 8 &&
           32 + (UINT64)*MaxEntries * 16 <= THIN_BLOCK;
}

/* value of Key in the btree at Root */
static BOOL
ThinLookup(THIN *T, UINT64 Root, UINT64 Key, UINT64 *Value)
{
    int Depth;
    for (Depth = 0; Depth < THIN_MAX_DEPTH; Depth++) {
        UINT32 Flags, Entries, Max, Vs, i, Pick = MAXUINT32;
        if (!ThinNode(T, Root, &Flags, &Entries, &Max, &Vs) || Entries == 0) return FALSE;
        for (i = 0; i < Entries; i++) {
            UINT64 K = Le64(T->Block + 32 + 8 * i);
            if (K <= Key) Pick = i; else break;
        }
        if (Pick == MAXUINT32) return FALSE;
        if (Flags == THIN_LEAF_NODE) {
            if (Le64(T->Block + 32 + 8 * Pick) != Key) return FALSE;
            *Value = Le64(T->Block + 32 + 8 * Max + 8 * Pick);
            return TRUE;
        }
        Root = Le64(T->Block + 32 + 8 * Max + 8 * Pick);
    }
    return FALSE;
}

/* one mapped chunk, in virtual order: the holes before it, then the chunk */
static BOOL
ThinChunk(THIN *T, UINT64 Virtual, UINT64 DataBlock)
{
    UINT64 Pv, Left;

    if (Virtual < T->NextChunk || Virtual >= T->VolumeChunks) return Virtual >= T->VolumeChunks;
    if (Virtual > T->NextChunk) {
        RunsAdd(T->R, T->NextChunk * T->ChunkBytes, (Virtual - T->NextChunk) * T->ChunkBytes, EXT4_LV_HOLE);
    }
    if (!LvmLinearToPv(T->G, T->Data, DataBlock * T->ChunkBytes, &Pv, &Left) || Left < T->ChunkBytes) {
        return FALSE;
    }
    RunsAdd(T->R, Virtual * T->ChunkBytes, T->ChunkBytes, Pv);
    T->NextChunk = Virtual + 1;
    T->Mapped++;
    return TRUE;
}

/* every mapping of the subtree at Block, in key order */
static BOOL
ThinWalk(THIN *T, UINT64 Block, int Depth)
{
    UINT32  Flags, Entries, Max, Vs, i;
    UINT64 *Keys, *Values;
    BOOL    Ok = TRUE;

    if (Depth > THIN_MAX_DEPTH || !ThinNode(T, Block, &Flags, &Entries, &Max, &Vs)) return FALSE;
    /* the buffer is reused by the recursion: keep this node's entries */
    Keys = (UINT64 *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)Entries * 16 + 16);
    if (Keys == NULL) return FALSE;
    Values = Keys + Entries;
    for (i = 0; i < Entries; i++) {
        Keys[i] = Le64(T->Block + 32 + 8 * i);
        Values[i] = Le64(T->Block + 32 + 8 * Max + 8 * i);
    }
    for (i = 0; i < Entries && Ok; i++) {
        Ok = Flags == THIN_INTERNAL_NODE ? ThinWalk(T, Values[i], Depth + 1) :
                                           ThinChunk(T, Keys[i], Values[i] >> THIN_TIME_BITS);
    }
    HeapFree(GetProcessHeap(), 0, Keys);
    return Ok;
}

static BOOL
LvmThinRuns(LVM_VG *G, const LVM_NODE *Lv, UINT64 Size, RUNS *R, ULONG *Mapped)
{
    const LVM_NODE *Seg = LvmOnlySegment(Lv, NULL), *Pool, *PoolSeg;
    const char     *PoolName, *MetaName, *DataName;
    UINT64          DeviceId, Chunk, MappingRoot, DataBlock, Subtree;
    THIN            T;

    if (Seg == NULL || (PoolName = LvmStr(Seg, "thin_pool")) == NULL ||
        !LvmU64(Seg, "device_id", &DeviceId) || (Pool = LvmFindLv(G, PoolName)) == NULL ||
        (PoolSeg = LvmOnlySegment(Pool, NULL)) == NULL ||
        (MetaName = LvmStr(PoolSeg, "metadata")) == NULL || (DataName = LvmStr(PoolSeg, "pool")) == NULL ||
        !LvmU64(PoolSeg, "chunk_size", &Chunk) || Chunk == 0) {
        Fail("thin volume %s: pool metadata incomplete", Lv->Name);
        return FALSE;
    }

    memset(&T, 0, sizeof(T));
    T.G = G;
    T.Meta = LvmFindLv(G, MetaName);
    T.Data = LvmFindLv(G, DataName);
    T.ChunkBytes = Chunk * LVM_SECTOR;
    T.R = R;
    T.VolumeChunks = Size / T.ChunkBytes;
    if (T.Meta == NULL || T.Data == NULL || Size % T.ChunkBytes) {
        Fail("thin pool %s: metadata or data volume missing", PoolName);
        return FALSE;
    }

    /* superblock: magic, data mapping root, data block size = chunk size */
    if (!ThinRead(&T, 0) || Le64(T.Block + 8) != 0 || Le64(T.Block + 32) != THIN_MAGIC) {
        Fail("thin pool %s: no dm-thin superblock", PoolName);
        return FALSE;
    }
    MappingRoot = Le64(T.Block + 320);
    DataBlock = Le32(T.Block + 336);
    if (DataBlock != Chunk) {
        Fail("thin pool %s: block size %llu, LVM says %llu", PoolName, DataBlock, Chunk);
        return FALSE;
    }

    if (!ThinLookup(&T, MappingRoot, DeviceId, &Subtree)) {
        /* a thin volume never written to has no mappings at all */
        RunsAdd(R, 0, Size, EXT4_LV_HOLE);
        *Mapped = 0;
        return !R->Failed;
    }
    if (!ThinWalk(&T, Subtree, 0)) {
        Fail("thin volume %s: the mapping btree is damaged", Lv->Name);
        return FALSE;
    }
    if (T.NextChunk < T.VolumeChunks) {
        RunsAdd(R, T.NextChunk * T.ChunkBytes, (T.VolumeChunks - T.NextChunk) * T.ChunkBytes, EXT4_LV_HOLE);
    }
    *Mapped = T.Mapped;
    return !R->Failed;
}

/* ---------------------------------------------------------------- per LV */

typedef enum { LV_LINEAR, LV_THIN, LV_OTHER } LV_TYPE;

static LV_TYPE
LvmType(const LVM_NODE *Lv, UINT64 ExtentBytes, UINT64 *Size)
{
    const LVM_NODE *Seg;
    UINT64 Extents = 0, Start, Count;
    BOOL   Linear = TRUE, Thin = FALSE;

    for (Seg = Lv->Child; Seg; Seg = Seg->Next) {
        const char *Type;
        if (Seg->Kind != LVM_SECTION || strncmp(Seg->Name, "segment", 7) != 0) continue;
        Type = LvmStr(Seg, "type");
        if (!LvmU64(Seg, "start_extent", &Start) || !LvmU64(Seg, "extent_count", &Count) || Type == NULL) {
            return LV_OTHER;
        }
        Extents = max(Extents, Start + Count);
        if (strcmp(Type, "thin") == 0) Thin = TRUE;
        else if (strcmp(Type, "striped") != 0) Linear = FALSE;
    }
    *Size = Extents * ExtentBytes;
    if (Thin) return LvmOnlySegment(Lv, NULL) ? LV_THIN : LV_OTHER;
    return Linear && Extents ? LV_LINEAR : LV_OTHER;
}

/* runs of an LV; Size out */
static BOOL
LvmRuns(LVM_VG *G, const LVM_NODE *Lv, RUNS *R, UINT64 *Size, ULONG *Mapped)
{
    LV_TYPE Type = LvmType(Lv, G->ExtentBytes, Size);

    memset(R, 0, sizeof(*R));
    *Mapped = 0;
    if (Type == LV_LINEAR) return LvmLinearRuns(G, Lv, *Size, R);
    if (Type == LV_THIN) return LvmThinRuns(G, Lv, *Size, R, Mapped);
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
            if (Run->Target == EXT4_LV_HOLE) memset(Out, 0, n);
            else if (!LuksReadAt(&G->Dev, Run->Target + (Off - Run->Start), Out, n)) return FALSE;
            Out += n; Off += n; Length -= n;
        }
    }
    return Length == 0;
}

static const char *
FsName(LVM_VG *G, const RUNS *R)
{
    UINT8 Super[2];
    if (RunsRead(G, R, EXT2_SUPER_MAGIC_OFFSET, Super, sizeof(Super)) && (Super[0] | (Super[1] << 8)) == EXT2_SUPER_MAGIC) return "ext2/3/4";
    return "-";
}

/* ---------------------------------------------------------------- commands */

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

        if (Lv->Kind != LVM_SECTION || !LvmHasFlag(Lv, "status", "VISIBLE")) continue;
        Type = LvmType(Lv, G.ExtentBytes, &Size);
        snprintf(Full, sizeof(Full), "%s/%s", G.VgName, Lv->Name);
        if (Type == LV_OTHER) {
            Say("%-32s %-7s %8.2f G  %-9s", Full, "pool", Size / LVM_GIB, "-");
            continue;
        }
        if (!LvmRuns(&G, Lv, &R, &Size, &Mapped)) {
            Say("%-32s %-7s %8.2f G  (cannot map)", Full, Type == LV_THIN ? "thin" : "linear", Size / LVM_GIB);
        } else if (Type == LV_THIN) {
            Say("%-32s %-7s %8.2f G  %-9s %lu chunks, %lu runs", Full, "thin", Size / LVM_GIB,
                FsName(&G, &R), Mapped, R.Count);
        } else {
            Say("%-32s %-7s %8.2f G  %-9s %lu run(s)", Full, "linear", Size / LVM_GIB,
                FsName(&G, &R), R.Count);
        }
        if (R.Run) HeapFree(GetProcessHeap(), 0, R.Run);
    }
    LvmClose(&G);
    return 0;
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
    HANDLE          Driver;
    DWORD           Got;
    BOOL            Ok;
    ULONGLONG       Start;

    if (WideCharToMultiByte(CP_UTF8, 0, VolumeName, -1, Name, sizeof(Name), NULL, NULL) <= 0) return 2;
    Slash = strchr(Name, '/');
    if (!LvmLoad(&G, Crypt, FALSE)) {
        LvmClose(&G);
        return 1;
    }
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

    Bytes = FIELD_OFFSET(EXT4_LV_OPEN, Run) + (SIZE_T)R.Count * sizeof(EXT4_LV_RUN);
    Req = (PEXT4_LV_OPEN)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Bytes);
    if (Req == NULL) { LvmClose(&G); return 1; }
    Req->Magic = EXT4_CRYPT_MAGIC;
    Req->Version = EXT4_CRYPT_VERSION;
    Req->Crypt = Crypt;
    Req->Letter = Letter;
    Req->Size = Size;
    strcpy_s(Req->Uuid, sizeof(Req->Uuid), LvmStr(Lv, "id") ? LvmStr(Lv, "id") : Name);
    snprintf(Req->Name, sizeof(Req->Name), "%s/%s", G.VgName, Lv->Name);
    Req->Runs = R.Count;
    memcpy(Req->Run, R.Run, (SIZE_T)R.Count * sizeof(EXT4_LV_RUN));
    HeapFree(GetProcessHeap(), 0, R.Run);

    Driver = OpenDriver();
    Ok = Driver && DeviceIoControl(Driver, IOCTL_APP_LV_OPEN, Req, (DWORD)Bytes, Req,
                                   FIELD_OFFSET(EXT4_LV_OPEN, Run), &Got, NULL);
    if (!Ok && Driver) Fail("the driver refused %s (error %lu)", Req->Name, GetLastError());
    if (Driver) CloseHandle(Driver);
    if (Ok) {
        WCHAR Device[64], Target[512], Drive[3] = L"A:";
        swprintf_s(Device, ARRAYSIZE(Device), EXT4_LV_DEVICE_PREFIX L"%lu", Req->Index);
        Say("%s opened read-only as %ls (%llu MiB, %lu runs)", Req->Name, Device, Size >> 20, Req->Runs);
        for (Start = GetTickCount64(); GetTickCount64() - Start < EXT4CTL_LETTER_WAIT_MS;
             Sleep(EXT4CTL_LETTER_POLL_MS)) {
            for (Drive[0] = L'A'; Drive[0] <= L'Z'; Drive[0]++) {
                if (QueryDosDeviceW(Drive, Target, ARRAYSIZE(Target)) && _wcsicmp(Target, Device) == 0) {
                    WCHAR Root[4] = { Drive[0], L':', L'\\', 0 }, Fs[32] = L"";
                    if (GetVolumeInformationW(Root, NULL, 0, NULL, NULL, NULL, Fs, ARRAYSIZE(Fs))) {
                        Say("mounted as %lc: (%ls, read-only)", Drive[0], Fs);
                        HeapFree(GetProcessHeap(), 0, Req);
                        LvmClose(&G);
                        return 0;
                    }
                }
            }
        }
        Say("no mounted drive letter yet");
    }
    HeapFree(GetProcessHeap(), 0, Req);
    LvmClose(&G);
    return Ok ? 0 : 1;
}

int
CmdLvClose(const WCHAR *Which, BOOL Force)
{
    EXT4_LV_QUERY   Q;
    EXT4_LV_CLOSE   Req;
    HANDLE          Driver = OpenDriver();
    DWORD           Got;
    ULONG           i;
    BOOL            Found = FALSE, Ok;

    if (Driver == NULL) return 1;
    memset(&Q, 0, sizeof(Q));
    Q.Magic = EXT4_CRYPT_MAGIC;
    if (!DeviceIoControl(Driver, IOCTL_APP_LV_QUERY, &Q, sizeof(Q), &Q, sizeof(Q), &Got, NULL)) {
        Fail("query failed (error %lu)", GetLastError());
        CloseHandle(Driver);
        return 1;
    }
    memset(&Req, 0, sizeof(Req));
    Req.Magic = EXT4_CRYPT_MAGIC;
    Req.Flags = Force ? EXT4_CRYPT_FORCE : 0;
    for (i = 0; i < Q.Count && !Found; i++) {
        WCHAR Device[64], Target[512], Drive[3] = L"A:";
        swprintf_s(Device, ARRAYSIZE(Device), EXT4_LV_DEVICE_PREFIX L"%lu", Q.Entries[i].Index);
        Drive[0] = towupper(Which[0]);
        if ((iswalpha(Which[0]) && Which[1] == L':' &&
             QueryDosDeviceW(Drive, Target, ARRAYSIZE(Target)) && _wcsicmp(Target, Device) == 0) ||
            (iswdigit(Which[0]) && Q.Entries[i].Index == (ULONG)_wtoi(Which))) {
            Req.Index = Q.Entries[i].Index;
            Found = TRUE;
        }
    }
    if (!Found) {
        Fail("%ls is not an open LVM volume (see ext4ctl status)", Which);
        CloseHandle(Driver);
        return 1;
    }
    Ok = DeviceIoControl(Driver, IOCTL_APP_LV_CLOSE, &Req, sizeof(Req), &Req, sizeof(Req), &Got, NULL);
    if (!Ok) {
        DWORD e = GetLastError();
        Fail(e == ERROR_BUSY ? "the volume is in use; close its files or use --force" :
             "close failed (error %lu)", e);
    } else {
        Say("\\Device\\Ext4Lv%lu closed", Req.Index);
    }
    CloseHandle(Driver);
    return Ok ? 0 : 1;
}

/* for ext4ctl status */
void
LvStatus(void)
{
    EXT4_LV_QUERY   Q;
    HANDLE          Driver = OpenDriver();
    DWORD           Got;
    ULONG           i;

    if (Driver == NULL) return;
    memset(&Q, 0, sizeof(Q));
    Q.Magic = EXT4_CRYPT_MAGIC;
    if (DeviceIoControl(Driver, IOCTL_APP_LV_QUERY, &Q, sizeof(Q), &Q, sizeof(Q), &Got, NULL)) {
        for (i = 0; i < Q.Count; i++) {
            WCHAR Device[64], Target[512], Drive[3] = L"A:", L = 0;
            swprintf_s(Device, ARRAYSIZE(Device), EXT4_LV_DEVICE_PREFIX L"%lu", Q.Entries[i].Index);
            for (Drive[0] = L'A'; Drive[0] <= L'Z' && !L; Drive[0]++) {
                if (QueryDosDeviceW(Drive, Target, ARRAYSIZE(Target)) && _wcsicmp(Target, Device) == 0) L = Drive[0];
            }
            Say("lv %lu  %lc%s  %s on \\Device\\Ext4Crypt%lu  %.2f G, %lu runs, read-only",
                Q.Entries[i].Index, L ? L : L'-', L ? ":" : " ", Q.Entries[i].Name, Q.Entries[i].Crypt,
                Q.Entries[i].Size / LVM_GIB, Q.Entries[i].Runs);
        }
    }
    CloseHandle(Driver);
}
