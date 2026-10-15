/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Fixed-size code-page slices: affine defaults and indexed exceptions.
 */
#ifndef EXT4_CHARSET_MAP_H
#define EXT4_CHARSET_MAP_H

/* Fixed-width kernel and host types without mixing CRT and WDK headers. */
typedef unsigned char NLS_BYTE;
typedef unsigned short NLS_WORD;
typedef unsigned int NLS_BITS;
static_assert(sizeof(NLS_BYTE) == 1 && sizeof(NLS_WORD) == 2 && sizeof(NLS_BITS) == 4);
#define NLS_BITS_C(value) value##u

/* A slice covers 32 input positions. Its two high bits select literal,
 * constant, ascending or sparse storage; the remaining bits hold an offset
 * or a value. Sparse exceptions preserve the original zero/unmapped slots.
 * All input indices come from bounded bytes in the conversion adapters.
 */
typedef struct {
    NLS_BITS Mask;
    NLS_BITS Base;       /* low 16 bits: value; bit 16: ascending default */
    NLS_BITS Offset;     /* exception values, in increasing input order */
} NLS_MAP_EXCEPTIONS;

static inline unsigned NlsMapRank(NLS_BITS bits)
{
    /* Number of exceptions preceding a position. No CPU feature required. */
    bits -= (bits >> 1) & NLS_BITS_C(0x55555555);
    bits = (bits & NLS_BITS_C(0x33333333)) + ((bits >> 2) & NLS_BITS_C(0x33333333));
    bits = (bits + (bits >> 4)) & NLS_BITS_C(0x0f0f0f0f);
    return (unsigned)((bits * NLS_BITS_C(0x01010101)) >> 24);
}

static inline NLS_BITS NlsMapSlot(const NLS_BITS *slices,
    const NLS_MAP_EXCEPTIONS *exceptions, unsigned index, unsigned *offset)
{
    NLS_BITS slice = slices[index >> 5];
    unsigned local = index & 31;
    NLS_BITS payload = slice & NLS_BITS_C(0x3fffffff);

    switch (slice >> 30) {
    case 1: return payload;                         /* f(i) = a */
    case 2: return payload + local;                 /* f(i) = a + i */
    case 3: {
        const NLS_MAP_EXCEPTIONS *entry = &exceptions[payload];
        NLS_BITS bit = NLS_BITS_C(1) << local;
        if (!(entry->Mask & bit))
            return (entry->Base & NLS_BITS_C(0xffff)) +
                   ((entry->Base >> 16) & 1) * local;
        *offset = entry->Offset + NlsMapRank(entry->Mask & (bit - 1));
        break;
    }
    default:
        *offset = payload + local;                 /* dense slice */
        break;
    }
    return NLS_BITS_C(0xffffffff);                              /* value in the literal pool */
}

static inline NLS_WORD NlsMapWord(const NLS_BITS *slices, const NLS_WORD *values,
    const NLS_MAP_EXCEPTIONS *exceptions, unsigned index)
{
    unsigned offset = 0;
    NLS_BITS value = NlsMapSlot(slices, exceptions, index, &offset);
    return value == NLS_BITS_C(0xffffffff) ? values[offset] : (NLS_WORD)value;
}

static inline NLS_BYTE NlsMapByte(const NLS_BITS *slices, const NLS_BYTE *values,
    const NLS_MAP_EXCEPTIONS *exceptions, unsigned index)
{
    unsigned offset = 0;
    NLS_BITS value = NlsMapSlot(slices, exceptions, index, &offset);
    return value == NLS_BITS_C(0xffffffff) ? values[offset] : (NLS_BYTE)value;
}

/* A page directory: the page of high byte i, or NULL. dir_index holds each
 * page's place in dir_list plus one, 0 where there is no page.
 */
#define NLS_PAGE(dir, i) \
    (dir##_index[(i)] ? dir##_list[dir##_index[(i)] - 1] : NULL)

/* Constant-expression spelling for identity runs in the retained case maps.
 * These fields retain their contiguous 256-byte NLS interface.
 */
#define NLS_ASCENDING_8(a) \
    (a), (a)+1, (a)+2, (a)+3, (a)+4, (a)+5, (a)+6, (a)+7

#endif
