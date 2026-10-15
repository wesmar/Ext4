/**
 * CharsetMap.h - code page maps: pages of 32-entry slices.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * tools/nls/make-nls.py builds the maps; CharsetMap.c reads them. A map is
 * pages of 256 entries, found through a byte per high byte that holds the
 * page's number plus one (0: no page). A page is eight slices of 32 entries,
 * each one 32-bit word whose two high bits select its form:
 *   constant  every entry is the payload
 *   step      entry i is the payload + i
 *   dense     entry i is Words[payload + i]
 *   entry     bits 27..29 a kind, bits 0..26 an NLS_MAP_ENTRY
 * An entry's Mask marks exceptions, the other positions are members; r of
 * the exceptions and j of the members precede position i. An exception is
 * Words[Offset + r] (kind run: Offset + r); the members' data follows the
 * exceptions in Words. A member is, by kind:
 *   constant  Base               step    Base + i
 *   rank      Base + j           run     Base + j
 *   narrow    Base + byte j of the data
 *   select    Base + position of the bit after j set bits of the data
 * Every value is 16 bits; lookups take a bounded number of steps and no
 * optional CPU instruction.
 */
#ifndef EXT4_CHARSET_MAP_H
#define EXT4_CHARSET_MAP_H

/* Fixed-width kernel and host types without mixing CRT and WDK headers. */
typedef unsigned char NLS_BYTE;
typedef unsigned short NLS_WORD;
typedef unsigned int NLS_BITS;
static_assert(sizeof(NLS_BYTE) == 1 && sizeof(NLS_WORD) == 2 && sizeof(NLS_BITS) == 4);

typedef struct {
    NLS_BITS Mask;      /* the exceptions */
    NLS_WORD Base;      /* the members' origin */
    NLS_WORD Offset;    /* the exceptions in Words, then the members' data */
} NLS_MAP_ENTRY;

typedef struct {
    const NLS_WORD *Words;
    const NLS_MAP_ENTRY *Entries;
    const NLS_BITS *DecodeSlices;   /* code to Unicode */
    const NLS_BITS *EncodeSlices;   /* Unicode to code */
    const NLS_BYTE *DecodePages;    /* lead byte -> page + 1; NULL: one page */
    const NLS_BYTE *EncodePages;    /* Unicode high byte -> page + 1 */
} NLS_CHARSET;

/* Set bits of a word: the number of exceptions before a position. */
static inline unsigned NlsMapRank(NLS_BITS bits)
{
    bits -= (bits >> 1) & 0x55555555u;
    bits = (bits & 0x33333333u) + ((bits >> 2) & 0x33333333u);
    bits = (bits + (bits >> 4)) & 0x0f0f0f0fu;
    return (bits * 0x01010101u) >> 24;
}

/* Converters of the code page families; nls_<name>.c binds them to a map. */
int NlsSingleToUnicode(const NLS_CHARSET *map, const unsigned char *raw, int bound, wchar_t *uni);
int NlsUnicodeToSingle(const NLS_CHARSET *map, wchar_t uni, unsigned char *out, int bound);
int NlsDoubleToUnicode(const NLS_CHARSET *map, const unsigned char *raw, int bound, wchar_t *uni);
int NlsUnicodeToDouble(const NLS_CHARSET *map, wchar_t uni, unsigned char *out, int bound);
int NlsShiftJisToUnicode(const NLS_CHARSET *map, const unsigned char *raw, int bound, wchar_t *uni);
int NlsUnicodeToShiftJis(const NLS_CHARSET *map, wchar_t uni, unsigned char *out, int bound);

#endif
