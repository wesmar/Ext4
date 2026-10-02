/**
 * luks2.c - LUKS2: two header copies, the JSON metadata, keyslots, digests, the data segment.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Two copies of the header, each with a checksum over the binary part and
 * the JSON area; the valid one with the higher sequence id wins.
 *
 *   passphrase --KDF of the keyslot (Argon2i/Argon2id, PBKDF2)--> keyslot key
 *   keyslot key --> candidate volume key (luks_keyslot.c)
 *   candidate --PBKDF2 of a digest that lists the keyslot--> must equal it
 */

#include "luks_internal.h"

typedef struct _LUKS2_HEADER {
    UINT8      *Binary;             /* LUKS2_BINARY_HDR, the JSON area after it */
    char       *Json;
    ULONGLONG   Size;               /* binary + JSON */
    ULONGLONG   SeqId;
} LUKS2_HEADER;

static void
Luks2Free(LUKS2_HEADER *H)
{
    if (H->Binary) {
        VirtualFree(H->Binary, 0, MEM_RELEASE);
    }
    memset(H, 0, sizeof(*H));
}

/* the copy's checksum: over all of it with the checksum field zeroed */
static BOOL
Luks2ChecksumOk(LUKS2_HEADER *H)
{
    LUKS2_BINARY_HDR   *B = (LUKS2_BINARY_HDR *)H->Binary;
    UINT8               Csum[sizeof(B->Checksum)], Saved[sizeof(B->Checksum)];
    char                Alg[sizeof(B->ChecksumAlg) + 1];
    LPCWSTR             HashId;
    ULONG               CsumLength;
    BOOL                Ok;

    LuksCopyField(Alg, sizeof(Alg), B->ChecksumAlg, sizeof(B->ChecksumAlg));
    HashId = CryptoHashId(Alg);
    CsumLength = HashId ? CryptoHashLength(HashId) : 0;
    if (CsumLength == 0 || CsumLength > sizeof(Csum)) {
        return FALSE;
    }
    memcpy(Saved, B->Checksum, sizeof(Saved));
    memset(B->Checksum, 0, sizeof(B->Checksum));
    Ok = CryptoHash(HashId, H->Binary, (ULONG)H->Size, NULL, 0, Csum, CsumLength) &&
         memcmp(Csum, Saved, CsumLength) == 0;
    memcpy(B->Checksum, Saved, sizeof(Saved));
    return Ok;
}

/* one copy of the header at Offset, checksum verified */
static BOOL
Luks2Read(LUKS_DEVICE *Dev, ULONGLONG Offset, BOOL Secondary, LUKS2_HEADER *H)
{
    LUKS2_BINARY_HDR    Head;
    ULONGLONG           Size;

    memset(H, 0, sizeof(*H));
    if (!LuksReadAt(Dev, Offset, &Head, sizeof(Head)) ||
        memcmp(Head.Magic, Secondary ? LuksMagic2 : LuksMagic, LUKS_MAGIC_BYTES) != 0 ||
        Be16(Head.Version) != 2) {
        return FALSE;
    }
    Size = Be64(Head.HeaderSize);
    if (Size <= LUKS2_BINARY || Size > LUKS2_MAX_HEADER || Be64(Head.HeaderOffset) != Offset) {
        return FALSE;
    }
    H->Binary = (UINT8 *)VirtualAlloc(NULL, (SIZE_T)Size + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (H->Binary == NULL || !LuksReadAt(Dev, Offset, H->Binary, (ULONG)Size)) {
        Luks2Free(H);
        return FALSE;
    }
    H->Json = (char *)H->Binary + LUKS2_BINARY;
    H->Size = Size;
    H->SeqId = Be64(((LUKS2_BINARY_HDR *)H->Binary)->SeqId);
    if (!Luks2ChecksumOk(H)) {
        Luks2Free(H);
        return FALSE;
    }
    return TRUE;
}

/* the better of the two copies */
static BOOL
Luks2Load(LUKS_DEVICE *Dev, LUKS2_HEADER *H)
{
    LUKS2_HEADER        Primary, Second;
    LUKS2_BINARY_HDR    Head;
    ULONGLONG           Offset = 0;
    BOOL                P, S;

    memset(&Second, 0, sizeof(Second));
    P = Luks2Read(Dev, 0, FALSE, &Primary);
    /* the second copy starts where the first ends (its size is in either) */
    if (P) {
        Offset = Primary.Size;
    } else if (LuksReadAt(Dev, 0, &Head, sizeof(Head))) {
        Offset = Be64(Head.HeaderSize);
    }
    S = Offset != 0 && Luks2Read(Dev, Offset, TRUE, &Second);

    if (P && (!S || Primary.SeqId >= Second.SeqId)) {
        *H = Primary;
        if (S) {
            Luks2Free(&Second);
        }
        return TRUE;
    }
    if (S) {
        *H = Second;
        if (P) {
            Luks2Free(&Primary);
        }
        return TRUE;
    }
    return FALSE;
}

/* the header loaded and its JSON parsed; Luks2Close undoes both */
static BOOL
Luks2Open(LUKS_DEVICE *Dev, LUKS2_HEADER *H, JSON_DOC *Doc)
{
    if (!Luks2Load(Dev, H)) {
        return FALSE;
    }
    if (!JsonParse(Doc, H->Json, (size_t)(H->Size - LUKS2_BINARY))) {
        Luks2Free(H);
        return FALSE;
    }
    return TRUE;
}

static void
Luks2Close(LUKS2_HEADER *H, JSON_DOC *Doc)
{
    JsonFree(Doc);
    Luks2Free(H);
}

/*
 * The data segment: where the data is and how it is encrypted. FALSE: the
 * header is not one this tool reads. A volume it must not open is still
 * described, with V->Problem saying why: data in a cipher the driver does
 * not have would be "unlocked" into garbage - and written to with it - so
 * the segment's own cipher is checked (not only the keyslots'), and so are
 * the things cryptsetup refuses to open without knowing them: authenticated
 * encryption (dm-integrity), a reencryption in progress (more than one
 * segment), and mandatory requirements in the configuration.
 */
static BOOL
Luks2Segment(const JSON *Root, LUKS_VOLUME *V)
{
    const JSON *Segments = JsonGet(Root, "segments");
    const JSON *Seg = Segments ? Segments->Child : NULL;
    const JSON *Need = JsonGet(JsonGet(JsonGet(Root, "config"), "requirements"), "mandatory");
    const char *Size;
    UINT64      Value;

    if (Seg == NULL || JsonText(JsonGet(Seg, "type")) == NULL ||
        strcmp(JsonText(JsonGet(Seg, "type")), "crypt") != 0 ||
        !JsonU64(JsonGet(Seg, "offset"), &V->PayloadOffset)) {
        return FALSE;
    }
    Size = JsonText(JsonGet(Seg, "size"));
    V->PayloadSize = 0;
    if (Size && strcmp(Size, "dynamic") != 0 && JsonU64(JsonGet(Seg, "size"), &Value)) {
        V->PayloadSize = Value;
    }
    V->IvOffset = 0;
    if (JsonU64(JsonGet(Seg, "iv_tweak"), &Value)) {
        V->IvOffset = Value;
    }
    V->SectorSize = LUKS_SECTOR;
    if (JsonU64(JsonGet(Seg, "sector_size"), &Value)) {
        V->SectorSize = (ULONG)Value;
    }
    if (JsonText(JsonGet(Seg, "encryption")) == NULL ||
        !LuksCopyText(V->Cipher, sizeof(V->Cipher), JsonText(JsonGet(Seg, "encryption")))) {
        return FALSE;
    }

    if (Seg->Next != NULL) {
        snprintf(V->Problem, sizeof(V->Problem), "a reencryption is in progress (more than one data segment)");
    } else if (Need != NULL && Need->Child != NULL) {
        snprintf(V->Problem, sizeof(V->Problem), "the header has mandatory requirements (%s)",
                 JsonText(Need->Child) ? JsonText(Need->Child) : "?");
    } else if (JsonGet(Seg, "integrity") != NULL) {
        snprintf(V->Problem, sizeof(V->Problem), "authenticated encryption (dm-integrity) is not supported");
    } else if (LuksParseCipher(V->Cipher) == 0) {
        snprintf(V->Problem, sizeof(V->Problem), "cipher %s is not supported (aes-xts-plain64 is)", V->Cipher);
    }
    return TRUE;
}

static const JSON *
Luks2FirstKeyslot(const JSON *Root)
{
    const JSON *Slots = JsonGet(Root, "keyslots");
    return Slots ? Slots->Child : NULL;
}

int
Luks2Probe(LUKS_DEVICE *Dev, LUKS_VOLUME *V)
{
    LUKS2_HEADER        H;
    JSON_DOC            Doc;
    LUKS2_BINARY_HDR   *B;
    const JSON         *Slot;
    UINT64              Value;
    int                 Result = 1;

    if (!Luks2Open(Dev, &H, &Doc)) {
        return -1;
    }
    B = (LUKS2_BINARY_HDR *)H.Binary;
    LuksCopyField(V->Uuid, sizeof(V->Uuid), B->Uuid, sizeof(B->Uuid));
    LuksCopyField(V->Label, sizeof(V->Label), B->Label, sizeof(B->Label));
    if (!Luks2Segment(Doc.Root, V)) {
        Result = -1;
    }
    for (Slot = Luks2FirstKeyslot(Doc.Root); Slot && Result == 1; Slot = Slot->Next) {
        if (V->Keyslots++ == 0) {
            const JSON *Kdf = JsonGet(Slot, "kdf");
            if (JsonText(JsonGet(Kdf, "type")) &&
                !LuksCopyText(V->Kdf, sizeof(V->Kdf), JsonText(JsonGet(Kdf, "type")))) {
                Result = -1;
            }
            if (JsonU64(JsonGet(Slot, "key_size"), &Value)) {
                V->KeyBytes = (ULONG)Value;
            }
        }
    }
    Luks2Close(&H, &Doc);
    return Result;
}

/* the keyslot key from the passphrase, as the keyslot's kdf says */
static BOOL
Luks2Derive(const JSON *Kdf, const void *Pass, ULONG PassLength, UINT8 *Out, ULONG OutLength)
{
    const char *Type = JsonText(JsonGet(Kdf, "type"));
    UINT8       Salt[LUKS_MAX_SALT];
    ULONG       SaltLength = sizeof(Salt);
    UINT64      A, B, C;

    if (Type == NULL || JsonText(JsonGet(Kdf, "salt")) == NULL ||
        !Base64Decode(JsonText(JsonGet(Kdf, "salt")), Salt, &SaltLength)) {
        return FALSE;
    }

    if (strcmp(Type, "pbkdf2") == 0) {
        LPCWSTR HashId = JsonText(JsonGet(Kdf, "hash")) ? CryptoHashId(JsonText(JsonGet(Kdf, "hash"))) : NULL;
        return HashId && JsonU64(JsonGet(Kdf, "iterations"), &A) &&
               CryptoPbkdf2(HashId, Pass, PassLength, Salt, SaltLength, A, Out, OutLength);
    }

    if (strcmp(Type, "argon2i") == 0 || strcmp(Type, "argon2id") == 0) {
        ARGON2_INPUT In;
        if (!JsonU64(JsonGet(Kdf, "time"), &A) || !JsonU64(JsonGet(Kdf, "memory"), &B) ||
            !JsonU64(JsonGet(Kdf, "cpus"), &C) || A > MAXUINT32 || B > MAXUINT32 || C == 0 ||
            C > ARGON2_MAX_LANES) {
            return FALSE;
        }
        memset(&In, 0, sizeof(In));
        In.Password = Pass;
        In.PasswordLength = PassLength;
        In.Salt = Salt;
        In.SaltLength = SaltLength;
        In.Type = strcmp(Type, "argon2i") == 0 ? ARGON2_I : ARGON2_ID;
        In.Passes = (UINT32)A;
        In.MemoryKiB = (UINT32)B;
        In.Lanes = (UINT32)C;
        return Argon2(&In, Out, OutLength);
    }

    Fail("keyslot kdf %s is not supported", Type);
    return FALSE;
}

/* is keyslot Name listed by digest D? */
static BOOL
DigestCovers(const JSON *D, const char *Name)
{
    const JSON *Slots = JsonGet(D, "keyslots");
    const JSON *Item;

    for (Item = Slots ? Slots->Child : NULL; Item; Item = Item->Next) {
        if (JsonText(Item) && strcmp(JsonText(Item), Name) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL
Luks2Verify(const JSON *Root, const char *SlotName, const UINT8 *Key, ULONG KeyBytes)
{
    const JSON *Digests = JsonGet(Root, "digests");
    const JSON *D;

    for (D = Digests ? Digests->Child : NULL; D; D = D->Next) {
        UINT8       Salt[LUKS_MAX_SALT], Want[LUKS_MAX_DIGEST], Got[LUKS_MAX_DIGEST];
        ULONG       SaltLength = sizeof(Salt), WantLength = sizeof(Want);
        LPCWSTR     HashId;
        UINT64      Iterations;

        if (!DigestCovers(D, SlotName) || JsonText(JsonGet(D, "type")) == NULL ||
            strcmp(JsonText(JsonGet(D, "type")), "pbkdf2") != 0 ||
            JsonText(JsonGet(D, "hash")) == NULL ||
            (HashId = CryptoHashId(JsonText(JsonGet(D, "hash")))) == NULL ||
            !JsonU64(JsonGet(D, "iterations"), &Iterations) ||
            JsonText(JsonGet(D, "salt")) == NULL || JsonText(JsonGet(D, "digest")) == NULL ||
            !Base64Decode(JsonText(JsonGet(D, "salt")), Salt, &SaltLength) ||
            !Base64Decode(JsonText(JsonGet(D, "digest")), Want, &WantLength) ||
            WantLength < LUKS_MIN_DIGEST || Iterations == 0) {
            /* an empty or short digest compared nothing - any key "matched"
               it, and the volume opened with a wrong key */
            continue;
        }
        if (CryptoPbkdf2(HashId, Key, KeyBytes, Salt, SaltLength, Iterations, Got, WantLength) &&
            memcmp(Got, Want, WantLength) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/* the volume key from one keyslot, if the passphrase opens it */
static BOOL
Luks2TryKeyslot(LUKS_DEVICE *Dev, const JSON *Root, const JSON *Slot, const void *Pass,
                ULONG PassLength, UINT8 *Key, ULONG *KeyBytes)
{
    const JSON *Area = JsonGet(Slot, "area");
    const JSON *Af = JsonGet(Slot, "af");
    UINT64      Size, AreaKey, Offset, Stripes, Priority;
    ULONG       AreaCipher;
    LPCWSTR     AfHash;
    UINT8       SlotKey[LUKS_MAX_KEY];
    BOOL        Ok;

    if (JsonU64(JsonGet(Slot, "priority"), &Priority) && Priority == 0) {
        return FALSE;                               /* "ignore" */
    }
    if (JsonText(JsonGet(Slot, "type")) == NULL || strcmp(JsonText(JsonGet(Slot, "type")), "luks2") != 0 ||
        !JsonU64(JsonGet(Slot, "key_size"), &Size) || Size == 0 || Size > LUKS_MAX_KEY ||
        !JsonU64(JsonGet(Area, "key_size"), &AreaKey) || AreaKey == 0 || AreaKey > LUKS_MAX_KEY ||
        !JsonU64(JsonGet(Area, "offset"), &Offset) ||
        JsonText(JsonGet(Area, "encryption")) == NULL ||
        (AreaCipher = LuksParseCipher(JsonText(JsonGet(Area, "encryption")))) == 0 ||
        !JsonU64(JsonGet(Af, "stripes"), &Stripes) ||
        JsonText(JsonGet(Af, "hash")) == NULL ||
        (AfHash = CryptoHashId(JsonText(JsonGet(Af, "hash")))) == NULL) {
        return FALSE;
    }

    Ok = Luks2Derive(JsonGet(Slot, "kdf"), Pass, PassLength, SlotKey, (ULONG)AreaKey) &&
         LuksOpenKeyslot(Dev, AreaCipher, SlotKey, (ULONG)AreaKey, Offset, AfHash,
                         (ULONG)Size, (ULONG)Stripes, Key) &&
         Luks2Verify(Root, Slot->Key, Key, (ULONG)Size);
    SecureZeroMemory(SlotKey, sizeof(SlotKey));
    if (Ok) {
        *KeyBytes = (ULONG)Size;
    }
    return Ok;
}

BOOL
Luks2Unlock(LUKS_DEVICE *Dev, const void *Pass, ULONG PassLength, UINT8 *Key,
            ULONG *KeyBytes, int *Keyslot)
{
    LUKS2_HEADER    H;
    JSON_DOC        Doc;
    const JSON     *Slot;
    BOOL            Found = FALSE;

    if (!Luks2Open(Dev, &H, &Doc)) {
        return FALSE;
    }
    for (Slot = Luks2FirstKeyslot(Doc.Root); Slot && !Found; Slot = Slot->Next) {
        if (Luks2TryKeyslot(Dev, Doc.Root, Slot, Pass, PassLength, Key, KeyBytes)) {
            *Keyslot = atoi(Slot->Key);
            Found = TRUE;
        }
    }
    if (!Found) {
        SecureZeroMemory(Key, LUKS_MAX_KEY);
    }
    Luks2Close(&H, &Doc);
    return Found;
}
