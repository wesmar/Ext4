/**
 * luks.c - LUKS1 and LUKS2 headers: probe, keyslots, volume key.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * As cryptsetup does it (and as the on-disk format documents describe it):
 *
 *   passphrase --KDF(salt)--> keyslot key
 *   keyslot area --AES-XTS(keyslot key, 512-byte sectors)--> AF material
 *   AF material --AF-merge(stripes, hash)--> candidate volume key
 *   candidate --PBKDF2(digest salt, iterations)--> must equal the digest
 *
 * The KDF is PBKDF2 (LUKS1, LUKS2 on request) or Argon2i/Argon2id (the
 * LUKS2 default). Integers in both binary headers are big-endian. LUKS2
 * keeps two copies of its header, each with a checksum over the binary
 * part and the JSON area; the valid one with the higher sequence id wins.
 */

#include "ext4ctl.h"

#define LUKS_SECTOR             512
#define LUKS1_HEADER            592
#define LUKS1_KEYSLOTS          8
#define LUKS1_KEY_ENABLED       0x00AC71F3
#define LUKS1_DIGEST            20
#define LUKS2_BINARY            4096
#define LUKS2_MAX_HEADER        (4 * 1024 * 1024)
#define LUKS_MAX_STRIPES        4000
#define LUKS_MAX_SALT           64
#define LUKS_MIN_DIGEST         20      /* SHA-1's, the shortest hash LUKS uses */

static const UINT8 Luks1Magic[6] = { 'L', 'U', 'K', 'S', 0xBA, 0xBE };
static const UINT8 Luks2Magic2[6] = { 'S', 'K', 'U', 'L', 0xBA, 0xBE };

static UINT16 Be16(const UINT8 *p) { return (UINT16)((p[0] << 8) | p[1]); }
static UINT32 Be32(const UINT8 *p) { return ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16) | ((UINT32)p[2] << 8) | p[3]; }
static UINT64 Be64(const UINT8 *p) { return ((UINT64)Be32(p) << 32) | Be32(p + 4); }

/* a text from the header, whole or not at all: a header with a longer one
   than the field holds is not a header this tool reads (strcpy_s would end
   the process, and every "list" probes every volume) */
static BOOL
CopyText(char *Out, size_t OutSize, const char *In)
{
    size_t n = strlen(In);

    if (n >= OutSize) {
        return FALSE;
    }
    memcpy(Out, In, n + 1);
    return TRUE;
}

static void
CopyField(char *Out, size_t OutSize, const UINT8 *Field, size_t FieldSize)
{
    size_t n = strnlen((const char *)Field, FieldSize);
    if (n >= OutSize) n = OutSize - 1;
    memcpy(Out, Field, n);
    Out[n] = 0;
}

/* ---------------------------------------------------------------- device */

BOOL
LuksOpenDevice(LUKS_DEVICE *Dev, const WCHAR *Name)
{
    WCHAR                   Path[EXT4_CRYPT_DEVICE_CHARS + 32];
    GET_LENGTH_INFORMATION  Length;
    DWORD                   Bytes;

    memset(Dev, 0, sizeof(*Dev));
    wcsncpy_s(Dev->NtName, ARRAYSIZE(Dev->NtName), Name, _TRUNCATE);
    swprintf_s(Path, ARRAYSIZE(Path), L"\\\\?\\GLOBALROOT%ls", Name);
    Dev->Handle = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
    if (Dev->Handle == INVALID_HANDLE_VALUE) {
        Dev->Handle = NULL;
        return FALSE;
    }
    if (DeviceIoControl(Dev->Handle, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                        &Length, sizeof(Length), &Bytes, NULL)) {
        Dev->Size = (ULONGLONG)Length.Length.QuadPart;
    }
    return TRUE;
}

void
LuksCloseDevice(LUKS_DEVICE *Dev)
{
    if (Dev->Handle) {
        CloseHandle(Dev->Handle);
    }
    memset(Dev, 0, sizeof(*Dev));
}

/* any range: raw volume reads must be whole sectors, so read around it */
BOOL
LuksReadAt(LUKS_DEVICE *Dev, ULONGLONG Offset, void *Buffer, ULONG Length)
{
    ULONGLONG   First = Offset & ~(ULONGLONG)(LUKS_SECTOR - 1);
    ULONG       Head = (ULONG)(Offset - First);
    ULONG       Span = (Head + Length + LUKS_SECTOR - 1) & ~(ULONG)(LUKS_SECTOR - 1);
    UINT8      *Aligned;
    OVERLAPPED  Ov;
    DWORD       Got = 0;
    BOOL        Ok;

    Aligned = (UINT8 *)VirtualAlloc(NULL, Span, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (Aligned == NULL) {
        return FALSE;
    }
    memset(&Ov, 0, sizeof(Ov));
    Ov.Offset = (DWORD)First;
    Ov.OffsetHigh = (DWORD)(First >> 32);
    Ok = ReadFile(Dev->Handle, Aligned, Span, &Got, &Ov) && Got == Span;
    if (Ok) {
        memcpy(Buffer, Aligned + Head, Length);
    }
    SecureZeroMemory(Aligned, Span);
    VirtualFree(Aligned, 0, MEM_RELEASE);
    return Ok;
}

/* ---------------------------------------------------------------- cipher */

typedef struct _LUKS_CIPHER {
    ULONG   Cipher;                 /* EXT4_CIPHER_* */
} LUKS_CIPHER;

/* "aes-xts-plain64" (LUKS2) or "aes" + "xts-plain64" (LUKS1) */
static BOOL
ParseCipher(const char *Spec, LUKS_CIPHER *Out)
{
    if (_stricmp(Spec, "aes-xts-plain64") == 0) {
        Out->Cipher = EXT4_CIPHER_AES_XTS_PLAIN64;
        return TRUE;
    }
    if (_stricmp(Spec, "aes-xts-plain") == 0) {
        Out->Cipher = EXT4_CIPHER_AES_XTS_PLAIN;
        return TRUE;
    }
    return FALSE;
}

/* decrypt Length bytes of a keyslot area in place, sectors numbered from 0 */
static BOOL
DecryptArea(const LUKS_CIPHER *Cipher, const UINT8 *Key, ULONG KeyBytes, UINT8 *Area, ULONG Length)
{
    EXT4_XTS    Xts;
    UINT8       Scratch[LUKS_SECTOR];
    ULONG       Sector;
    BOOL        Ok = TRUE;

    if (CryptoAesEcb() == NULL || !NT_SUCCESS(Ext4XtsInit(&Xts, CryptoAesEcb(), Key, KeyBytes))) {
        return FALSE;
    }
    for (Sector = 0; Sector * LUKS_SECTOR < Length && Ok; Sector++) {
        ULONGLONG Iv = Sector;
        if (Cipher->Cipher == EXT4_CIPHER_AES_XTS_PLAIN) {
            Iv &= MAXUINT32;              /* "plain": 32-bit sector numbers */
        }
        Ok = NT_SUCCESS(Ext4XtsUnit(&Xts, FALSE, Iv, Area + (SIZE_T)Sector * LUKS_SECTOR,
                                    LUKS_SECTOR, Scratch));
    }
    Ext4XtsFree(&Xts);
    SecureZeroMemory(Scratch, sizeof(Scratch));
    return Ok;
}

/* ---------------------------------------------------------------- AF */

/* each digest-sized block replaced by Hash(BE32(index) || block) */
static BOOL
Diffuse(LPCWSTR HashId, UINT8 *Buffer, ULONG Size)
{
    ULONG   Digest = CryptoHashLength(HashId);
    ULONG   Blocks = Size / Digest, Rest = Size % Digest, i;
    UINT8   Index[4];

    for (i = 0; i <= Blocks; i++) {
        ULONG Length = i < Blocks ? Digest : Rest;
        if (Length == 0) {
            break;
        }
        Index[0] = (UINT8)(i >> 24);
        Index[1] = (UINT8)(i >> 16);
        Index[2] = (UINT8)(i >> 8);
        Index[3] = (UINT8)i;
        if (!CryptoHash(HashId, Index, 4, Buffer + (SIZE_T)i * Digest, Length,
                        Buffer + (SIZE_T)i * Digest, Length)) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL
AfMerge(LPCWSTR HashId, const UINT8 *Material, ULONG KeyBytes, ULONG Stripes, UINT8 *Key)
{
    UINT8   Block[LUKS_MAX_KEY];
    ULONG   i, j;

    memset(Block, 0, sizeof(Block));
    for (i = 0; i + 1 < Stripes; i++) {
        for (j = 0; j < KeyBytes; j++) {
            Block[j] ^= Material[(SIZE_T)i * KeyBytes + j];
        }
        if (!Diffuse(HashId, Block, KeyBytes)) {
            SecureZeroMemory(Block, sizeof(Block));
            return FALSE;
        }
    }
    for (j = 0; j < KeyBytes; j++) {
        Key[j] = Block[j] ^ Material[(SIZE_T)(Stripes - 1) * KeyBytes + j];
    }
    SecureZeroMemory(Block, sizeof(Block));
    return TRUE;
}

/*
 * Read the area, decrypt it, merge: the candidate volume key of one
 * keyslot, from its already derived keyslot key.
 */
static BOOL
OpenKeyslot(LUKS_DEVICE *Dev, const LUKS_CIPHER *AreaCipher, const UINT8 *SlotKey,
            ULONG SlotKeyBytes, ULONGLONG AreaOffset, LPCWSTR AfHash,
            ULONG KeyBytes, ULONG Stripes, UINT8 *Key)
{
    ULONG   AfBytes = KeyBytes * Stripes;
    ULONG   Span = (AfBytes + LUKS_SECTOR - 1) & ~(ULONG)(LUKS_SECTOR - 1);
    UINT8  *Area;
    BOOL    Ok;

    if (Stripes == 0 || Stripes > LUKS_MAX_STRIPES || KeyBytes == 0 || KeyBytes > LUKS_MAX_KEY) {
        return FALSE;
    }
    Area = (UINT8 *)VirtualAlloc(NULL, Span, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (Area == NULL) {
        return FALSE;
    }
    Ok = LuksReadAt(Dev, AreaOffset, Area, Span) &&
         DecryptArea(AreaCipher, SlotKey, SlotKeyBytes, Area, Span) &&
         AfMerge(AfHash, Area, KeyBytes, Stripes, Key);
    SecureZeroMemory(Area, Span);
    VirtualFree(Area, 0, MEM_RELEASE);
    return Ok;
}

/* ---------------------------------------------------------------- LUKS1 */

static int
Luks1Probe(const UINT8 *H, LUKS_VOLUME *V)
{
    char        Name[33], Mode[33];
    LUKS_CIPHER Cipher;
    int         i;

    CopyField(Name, sizeof(Name), H + 8, 32);
    CopyField(Mode, sizeof(Mode), H + 40, 32);
    snprintf(V->Cipher, sizeof(V->Cipher), "%s-%s", Name, Mode);
    V->KeyBytes = Be32(H + 108);
    V->SectorSize = LUKS_SECTOR;
    V->PayloadOffset = (ULONGLONG)Be32(H + 104) * LUKS_SECTOR;
    CopyField(V->Uuid, sizeof(V->Uuid), H + 168, 40);
    strcpy_s(V->Kdf, sizeof(V->Kdf), "pbkdf2");
    for (i = 0; i < LUKS1_KEYSLOTS; i++) {
        if (Be32(H + 208 + 48 * i) == LUKS1_KEY_ENABLED) {
            V->Keyslots++;
        }
    }
    if (!ParseCipher(V->Cipher, &Cipher)) {
        /* a LUKS volume all the same: listed, not opened */
        snprintf(V->Problem, sizeof(V->Problem), "cipher %s is not supported (aes-xts-plain64 is)", V->Cipher);
    }
    return 1;
}

static BOOL
Luks1Unlock(LUKS_DEVICE *Dev, const UINT8 *H, const void *Pass, ULONG PassLength,
            UINT8 *Key, int *Keyslot)
{
    char        Name[33], Mode[33], Spec[70], HashName[33];
    LUKS_CIPHER Cipher;
    LPCWSTR     HashId;
    ULONG       KeyBytes = Be32(H + 108);
    UINT8       SlotKey[LUKS_MAX_KEY], Digest[LUKS1_DIGEST];
    int         i;

    CopyField(Name, sizeof(Name), H + 8, 32);
    CopyField(Mode, sizeof(Mode), H + 40, 32);
    CopyField(HashName, sizeof(HashName), H + 72, 32);
    snprintf(Spec, sizeof(Spec), "%s-%s", Name, Mode);
    if (!ParseCipher(Spec, &Cipher)) {
        Fail("cipher %s is not supported (aes-xts-plain64 is)", Spec);
        return FALSE;
    }
    HashId = CryptoHashId(HashName);
    if (HashId == NULL || KeyBytes == 0 || KeyBytes > LUKS_MAX_KEY) {
        Fail("hash %s / key size %lu is not supported", HashName, KeyBytes);
        return FALSE;
    }

    for (i = 0; i < LUKS1_KEYSLOTS; i++) {
        const UINT8 *Slot = H + 208 + 48 * i;
        BOOL         Ok;

        if (Be32(Slot) != LUKS1_KEY_ENABLED) {
            continue;
        }
        Ok = CryptoPbkdf2(HashId, Pass, PassLength, Slot + 8, 32, Be32(Slot + 4),
                          SlotKey, KeyBytes) &&
             OpenKeyslot(Dev, &Cipher, SlotKey, KeyBytes, (ULONGLONG)Be32(Slot + 40) * LUKS_SECTOR,
                         HashId, KeyBytes, Be32(Slot + 44), Key) &&
             CryptoPbkdf2(HashId, Key, KeyBytes, H + 132, 32, Be32(H + 164),
                          Digest, LUKS1_DIGEST) &&
             memcmp(Digest, H + 112, LUKS1_DIGEST) == 0;
        SecureZeroMemory(SlotKey, sizeof(SlotKey));
        if (Ok) {
            *Keyslot = i;
            return TRUE;
        }
    }
    SecureZeroMemory(Key, LUKS_MAX_KEY);
    return FALSE;
}

/* ---------------------------------------------------------------- LUKS2 */

typedef struct _LUKS2_HEADER {
    UINT8      *Binary;             /* LUKS2_BINARY bytes */
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

/* one copy of the header at Offset, checksum verified */
static BOOL
Luks2Read(LUKS_DEVICE *Dev, ULONGLONG Offset, BOOL Secondary, LUKS2_HEADER *H)
{
    UINT8       Head[LUKS2_BINARY];
    UINT8       Csum[64], Saved[64];
    char        Alg[33];
    LPCWSTR     HashId;
    ULONGLONG   Size;
    ULONG       CsumLength;

    memset(H, 0, sizeof(*H));
    if (!LuksReadAt(Dev, Offset, Head, sizeof(Head)) ||
        memcmp(Head, Secondary ? Luks2Magic2 : Luks1Magic, 6) != 0 ||
        Be16(Head + 6) != 2) {
        return FALSE;
    }
    Size = Be64(Head + 8);
    if (Size <= LUKS2_BINARY || Size > LUKS2_MAX_HEADER || Be64(Head + 256) != Offset) {
        return FALSE;
    }
    H->Binary = (UINT8 *)VirtualAlloc(NULL, (SIZE_T)Size + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (H->Binary == NULL || !LuksReadAt(Dev, Offset, H->Binary, (ULONG)Size)) {
        Luks2Free(H);
        return FALSE;
    }
    H->Json = (char *)H->Binary + LUKS2_BINARY;
    H->Size = Size;
    H->SeqId = Be64(H->Binary + 16);

    /* checksum over everything with the checksum field zeroed */
    CopyField(Alg, sizeof(Alg), H->Binary + 72, 32);
    HashId = CryptoHashId(Alg);
    CsumLength = HashId ? CryptoHashLength(HashId) : 0;
    if (CsumLength == 0) {
        Luks2Free(H);
        return FALSE;
    }
    memcpy(Saved, H->Binary + 448, 64);
    memset(H->Binary + 448, 0, 64);
    if (!CryptoHash(HashId, H->Binary, (ULONG)Size, NULL, 0, Csum, CsumLength) ||
        memcmp(Csum, Saved, CsumLength) != 0) {
        Luks2Free(H);
        return FALSE;
    }
    memcpy(H->Binary + 448, Saved, 64);
    return TRUE;
}

/* the better of the two copies */
static BOOL
Luks2Load(LUKS_DEVICE *Dev, LUKS2_HEADER *H)
{
    LUKS2_HEADER    Primary, Second;
    UINT8           Head[LUKS2_BINARY];
    ULONGLONG       Offset = 0;
    BOOL            P, S;

    memset(&Second, 0, sizeof(Second));
    P = Luks2Read(Dev, 0, FALSE, &Primary);
    /* the second copy starts where the first ends (its size is in either) */
    if (P) {
        Offset = Primary.Size;
    } else if (LuksReadAt(Dev, 0, Head, sizeof(Head))) {
        Offset = Be64(Head + 8);
    }
    S = Offset != 0 && Luks2Read(Dev, Offset, TRUE, &Second);

    if (P && (!S || Primary.SeqId >= Second.SeqId)) {
        *H = Primary;
        if (S) Luks2Free(&Second);
        return TRUE;
    }
    if (S) {
        *H = Second;
        if (P) Luks2Free(&Primary);
        return TRUE;
    }
    return FALSE;
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
    LUKS_CIPHER Cipher;

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
        !CopyText(V->Cipher, sizeof(V->Cipher), JsonText(JsonGet(Seg, "encryption")))) {
        return FALSE;
    }

    if (Seg->Next != NULL) {
        snprintf(V->Problem, sizeof(V->Problem), "a reencryption is in progress (more than one data segment)");
    } else if (Need != NULL && Need->Child != NULL) {
        snprintf(V->Problem, sizeof(V->Problem), "the header has mandatory requirements (%s)",
                 JsonText(Need->Child) ? JsonText(Need->Child) : "?");
    } else if (JsonGet(Seg, "integrity") != NULL) {
        snprintf(V->Problem, sizeof(V->Problem), "authenticated encryption (dm-integrity) is not supported");
    } else if (!ParseCipher(V->Cipher, &Cipher)) {
        snprintf(V->Problem, sizeof(V->Problem), "cipher %s is not supported (aes-xts-plain64 is)", V->Cipher);
    }
    return TRUE;
}

static int
Luks2Probe(LUKS_DEVICE *Dev, LUKS_VOLUME *V)
{
    LUKS2_HEADER    H;
    JSON_DOC        Doc;
    const JSON     *Slot;
    UINT64          Value;

    if (!Luks2Load(Dev, &H)) {
        return -1;
    }
    CopyField(V->Uuid, sizeof(V->Uuid), H.Binary + 168, 40);
    CopyField(V->Label, sizeof(V->Label), H.Binary + 24, 48);
    if (!JsonParse(&Doc, H.Json, (size_t)(H.Size - LUKS2_BINARY))) {
        Luks2Free(&H);
        return -1;
    }
    if (!Luks2Segment(Doc.Root, V)) {
        JsonFree(&Doc);
        Luks2Free(&H);
        return -1;
    }
    for (Slot = JsonGet(Doc.Root, "keyslots") ? JsonGet(Doc.Root, "keyslots")->Child : NULL;
         Slot; Slot = Slot->Next) {
        if (V->Keyslots++ == 0) {
            const JSON *Kdf = JsonGet(Slot, "kdf");
            if (JsonText(JsonGet(Kdf, "type")) &&
                !CopyText(V->Kdf, sizeof(V->Kdf), JsonText(JsonGet(Kdf, "type")))) {
                JsonFree(&Doc);
                Luks2Free(&H);
                return -1;
            }
            if (JsonU64(JsonGet(Slot, "key_size"), &Value)) {
                V->KeyBytes = (ULONG)Value;
            }
        }
    }
    JsonFree(&Doc);
    Luks2Free(&H);
    return 1;
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
            !JsonU64(JsonGet(Kdf, "cpus"), &C) || A > MAXUINT32 || B > MAXUINT32 || C == 0 || C > 64) {
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
        UINT8       Salt[LUKS_MAX_SALT], Want[64], Got[64];
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

static BOOL
Luks2Unlock(LUKS_DEVICE *Dev, const void *Pass, ULONG PassLength, UINT8 *Key,
            ULONG *KeyBytes, int *Keyslot)
{
    LUKS2_HEADER    H;
    JSON_DOC        Doc;
    const JSON     *Slot;
    BOOL            Found = FALSE;

    if (!Luks2Load(Dev, &H)) {
        return FALSE;
    }
    if (!JsonParse(&Doc, H.Json, (size_t)(H.Size - LUKS2_BINARY))) {
        Luks2Free(&H);
        return FALSE;
    }

    for (Slot = JsonGet(Doc.Root, "keyslots") ? JsonGet(Doc.Root, "keyslots")->Child : NULL;
         Slot && !Found; Slot = Slot->Next) {
        const JSON *Area = JsonGet(Slot, "area");
        const JSON *Af = JsonGet(Slot, "af");
        UINT64      Size, AreaKey, Offset, Stripes, Priority;
        LUKS_CIPHER AreaCipher;
        LPCWSTR     AfHash;
        UINT8       SlotKey[LUKS_MAX_KEY];

        if (JsonU64(JsonGet(Slot, "priority"), &Priority) && Priority == 0) {
            continue;                               /* "ignore" */
        }
        if (JsonText(JsonGet(Slot, "type")) == NULL || strcmp(JsonText(JsonGet(Slot, "type")), "luks2") != 0 ||
            !JsonU64(JsonGet(Slot, "key_size"), &Size) || Size == 0 || Size > LUKS_MAX_KEY ||
            !JsonU64(JsonGet(Area, "key_size"), &AreaKey) || AreaKey == 0 || AreaKey > LUKS_MAX_KEY ||
            !JsonU64(JsonGet(Area, "offset"), &Offset) ||
            JsonText(JsonGet(Area, "encryption")) == NULL ||
            !ParseCipher(JsonText(JsonGet(Area, "encryption")), &AreaCipher) ||
            !JsonU64(JsonGet(Af, "stripes"), &Stripes) ||
            JsonText(JsonGet(Af, "hash")) == NULL ||
            (AfHash = CryptoHashId(JsonText(JsonGet(Af, "hash")))) == NULL) {
            continue;
        }

        if (Luks2Derive(JsonGet(Slot, "kdf"), Pass, PassLength, SlotKey, (ULONG)AreaKey) &&
            OpenKeyslot(Dev, &AreaCipher, SlotKey, (ULONG)AreaKey, Offset, AfHash,
                        (ULONG)Size, (ULONG)Stripes, Key) &&
            Luks2Verify(Doc.Root, Slot->Key, Key, (ULONG)Size)) {
            *KeyBytes = (ULONG)Size;
            *Keyslot = atoi(Slot->Key);
            Found = TRUE;
        }
        SecureZeroMemory(SlotKey, sizeof(SlotKey));
    }

    if (!Found) {
        SecureZeroMemory(Key, LUKS_MAX_KEY);
    }
    JsonFree(&Doc);
    Luks2Free(&H);
    return Found;
}

/* ---------------------------------------------------------------- public */

int
LuksProbe(LUKS_DEVICE *Dev, LUKS_VOLUME *V)
{
    UINT8 H[LUKS1_HEADER];

    memset(V, 0, sizeof(*V));
    if (!LuksReadAt(Dev, 0, H, sizeof(H))) {
        return -1;
    }
    if (memcmp(H, Luks1Magic, 6) != 0) {
        return 0;
    }
    V->Version = Be16(H + 6);
    if (V->Version == 1) {
        return Luks1Probe(H, V);
    }
    if (V->Version == 2) {
        return Luks2Probe(Dev, V);
    }
    return -1;
}

ULONG
LuksCipher(const LUKS_VOLUME *V)
{
    LUKS_CIPHER Cipher;

    return V->Problem[0] == 0 && ParseCipher(V->Cipher, &Cipher) ? Cipher.Cipher : 0;
}

BOOL
LuksUnlockKey(LUKS_DEVICE *Dev, const LUKS_VOLUME *V, const void *Pass, ULONG PassLength,
              UINT8 Key[LUKS_MAX_KEY], int *Keyslot)
{
    if (V->Version == 1) {
        UINT8 H[LUKS1_HEADER];
        return LuksReadAt(Dev, 0, H, sizeof(H)) && Luks1Unlock(Dev, H, Pass, PassLength, Key, Keyslot);
    }
    if (V->Version == 2) {
        ULONG KeyBytes = 0;
        return Luks2Unlock(Dev, Pass, PassLength, Key, &KeyBytes, Keyslot) && KeyBytes == V->KeyBytes;
    }
    return FALSE;
}
