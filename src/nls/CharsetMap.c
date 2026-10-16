/**
 * CharsetMap.c - code page converters over the maps of CharsetMap.h.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * Three families: single byte; double byte (936, 949, 950), where a lead
 * byte with a page starts a two-byte code; and Shift JIS (932), which adds
 * half-width katakana as single bytes. The results, including the bytes
 * written before a failure and the errors for short bounds, are those of
 * the converters the driver has always used (tests/nls-model.ps1 compares
 * every input).
 */

#include <linux/module.h>
#include <linux/errno.h>
#include "CharsetMap.h"

#define NLS_SLICE_SHIFT     5           /* 32 entries a slice */
#define NLS_SLICES_PER_PAGE 8
#define NLS_PAYLOAD         0x3fffffffu
#define NLS_ENTRY_INDEX     0x07ffffffu
#define NLS_KIND_SHIFT      27
#define NLS_KATAKANA_FIRST  0xa1        /* bytes A1..DF are U+FF61..U+FF9F */
#define NLS_KATAKANA_LAST   0xdf
#define NLS_KATAKANA_SHIFT  0xfec0
#define NLS_LATIN1_FIRST    0xa0        /* Shift JIS: U+00A0.. have two-byte codes */

enum { NLS_DENSE, NLS_CONSTANT, NLS_STEP, NLS_ENTRY };
enum { NLS_KIND_CONSTANT, NLS_KIND_STEP, NLS_KIND_RANK, NLS_KIND_RUN, NLS_KIND_NARROW, NLS_KIND_SELECT };

/* Position of the set bit that follows j others in a bitmap of words. */
static unsigned NlsSelect(const NLS_WORD *bitmap, unsigned j)
{
    unsigned position = 0, count;
    NLS_BITS bits;

    while (j >= (count = NlsMapRank(*bitmap))) {
        j -= count;
        bitmap++;
        position += 16;
    }
    for (bits = *bitmap; j; j--)
        bits &= bits - 1;               /* drop the lowest set bit */
    return position + NlsMapRank((bits & (0u - bits)) - 1);
}

static NLS_WORD NlsValue(const NLS_CHARSET *map, const NLS_BITS *slices, unsigned page, unsigned low)
{
    NLS_BITS slice = slices[page * NLS_SLICES_PER_PAGE + (low >> NLS_SLICE_SHIFT)];
    NLS_BITS payload = slice & NLS_PAYLOAD;
    unsigned i = low & ((1u << NLS_SLICE_SHIFT) - 1);
    const NLS_MAP_ENTRY *entry;
    unsigned kind, before, j;
    NLS_BITS bit;

    switch (slice >> 30) {
    case NLS_DENSE:
        return map->Words[payload + i];
    case NLS_CONSTANT:
        return (NLS_WORD)payload;
    case NLS_STEP:
        return (NLS_WORD)(payload + i);
    }
    entry = &map->Entries[payload & NLS_ENTRY_INDEX];
    kind = payload >> NLS_KIND_SHIFT;
    bit = 1u << i;
    before = NlsMapRank(entry->Mask & (bit - 1));
    if (entry->Mask & bit) {
        if (kind == NLS_KIND_RUN)
            return (NLS_WORD)(entry->Offset + before);
        return map->Words[entry->Offset + before];
    }
    j = i - before;
    switch (kind) {
    case NLS_KIND_CONSTANT:
        return entry->Base;
    case NLS_KIND_STEP:
        return (NLS_WORD)(entry->Base + i);
    case NLS_KIND_NARROW: {
        const NLS_WORD *bytes = map->Words + entry->Offset + NlsMapRank(entry->Mask);
        return (NLS_WORD)(entry->Base + (NLS_BYTE)(bytes[j >> 1] >> (8 * (j & 1))));
    }
    case NLS_KIND_SELECT:
        return (NLS_WORD)(entry->Base + NlsSelect(map->Words + entry->Offset + NlsMapRank(entry->Mask), j));
    default:                            /* rank, run */
        return (NLS_WORD)(entry->Base + j);
    }
}

/* Unicode to a two-byte code from a page: written even when it is 0. */
static int NlsPutPair(const NLS_CHARSET *map, unsigned page, wchar_t uni, unsigned char *out)
{
    NLS_WORD code = NlsValue(map, map->EncodeSlices, page - 1, uni & 0xff);

    out[0] = (unsigned char)(code >> 8);
    out[1] = (unsigned char)code;
    return code ? 2 : -EINVAL;
}

/* The byte is read whatever the bound: the established converters did. */
int NlsSingleToUnicode(const NLS_CHARSET *map, const unsigned char *raw, int bound, wchar_t *uni)
{
    UNREFERENCED_PARAMETER(bound);
    if ((unsigned)raw[0] - map->IdentityFirst < map->IdentityCount) {
        *uni = raw[0];
        return 1;
    }
    *uni = (wchar_t)NlsValue(map, map->DecodeSlices, 0, raw[0]);
    return *uni ? 1 : -EINVAL;
}

int NlsUnicodeToSingle(const NLS_CHARSET *map, wchar_t uni, unsigned char *out, int bound)
{
    unsigned page = map->EncodePages[uni >> 8];
    NLS_WORD code;

    if (bound <= 0)
        return -ENAMETOOLONG;
    if ((unsigned)uni - map->IdentityFirst < map->IdentityCount) {
        out[0] = (unsigned char)uni;
        return 1;
    }
    if (!page)
        return -EINVAL;
    code = NlsValue(map, map->EncodeSlices, page - 1, uni & 0xff);
    if (!code)
        return -EINVAL;
    out[0] = (unsigned char)code;
    return 1;
}

/* A lead byte with a page and a nonzero second byte make a two-byte code;
 * any other byte stands for itself.
 */
int NlsDoubleToUnicode(const NLS_CHARSET *map, const unsigned char *raw, int bound, wchar_t *uni)
{
    unsigned page;

    if (bound <= 0)
        return -ENAMETOOLONG;
    if (bound > 1 && raw[1] && (page = map->DecodePages[raw[0]]) != 0) {
        *uni = (wchar_t)NlsValue(map, map->DecodeSlices, page - 1, raw[1]);
        return *uni ? 2 : -EINVAL;
    }
    *uni = raw[0];
    return 1;
}

/* Characters of a page take two bytes; other ones up to U+00FF one. */
int NlsUnicodeToDouble(const NLS_CHARSET *map, wchar_t uni, unsigned char *out, int bound)
{
    unsigned page;

    if (bound <= 0)
        return -ENAMETOOLONG;
    page = map->EncodePages[uni >> 8];
    if (page) {
        if (bound < 2)
            return -ENAMETOOLONG;
        return NlsPutPair(map, page, uni, out);
    }
    if (!uni || uni > 0xff)
        return -EINVAL;
    out[0] = (unsigned char)uni;
    return 1;
}

int NlsShiftJisToUnicode(const NLS_CHARSET *map, const unsigned char *raw, int bound, wchar_t *uni)
{
    unsigned page;

    if (bound <= 0)
        return -ENAMETOOLONG;
    if (raw[0] < 0x80) {
        *uni = raw[0];
        return 1;
    }
    if (raw[0] >= NLS_KATAKANA_FIRST && raw[0] <= NLS_KATAKANA_LAST) {
        *uni = (wchar_t)(raw[0] + NLS_KATAKANA_SHIFT);
        return 1;
    }
    if (bound < 2)
        return -ENAMETOOLONG;
    page = map->DecodePages[raw[0]];
    if (!page || !raw[1])
        return -EINVAL;
    *uni = (wchar_t)NlsValue(map, map->DecodeSlices, page - 1, raw[1]);
    return *uni ? 2 : -EINVAL;
}

/* U+0000..U+00FF never use a regular page: below U+0080 they are single
 * bytes, from U+00A0 the page of high byte 0 holds their two-byte codes,
 * which count only with both bytes nonzero. The established converter
 * wrote those two bytes whatever the bound; this one refuses a bound of 1.
 */
int NlsUnicodeToShiftJis(const NLS_CHARSET *map, wchar_t uni, unsigned char *out, int bound)
{
    unsigned page;

    if (bound <= 0)
        return -ENAMETOOLONG;
    if (uni >= NLS_KATAKANA_FIRST + NLS_KATAKANA_SHIFT && uni <= NLS_KATAKANA_LAST + NLS_KATAKANA_SHIFT) {
        out[0] = (unsigned char)(uni - NLS_KATAKANA_SHIFT);
        return 1;
    }
    if (uni <= 0xff) {
        if (uni < 0x80) {
            out[0] = (unsigned char)uni;
            return 1;
        }
        if (uni < NLS_LATIN1_FIRST)
            return -EINVAL;
        if (bound < 2)
            return -ENAMETOOLONG;
        NlsPutPair(map, map->EncodePages[0], uni, out);
        return out[0] && out[1] ? 2 : -EINVAL;
    }
    page = map->EncodePages[uni >> 8];
    if (!page)
        return -EINVAL;
    if (bound < 2)
        return -ENAMETOOLONG;
    return NlsPutPair(map, page, uni, out);
}
