/**
 * NameHash.h - the hashes an indexed (htree) directory orders its names by.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * Plain C, no system header: the driver and the host models compile it.
 */

#ifndef _EXT4_NAME_HASH_H_
#define _EXT4_NAME_HASH_H_

/* the versions an index root names; the unsigned ones are the same hashes
   over names read as unsigned bytes (a volume made where char is unsigned) */
#define NH_LEGACY               0u
#define NH_HALF_MD4             1u
#define NH_TEA                  2u
#define NH_LEGACY_UNSIGNED      3u
#define NH_HALF_MD4_UNSIGNED    4u
#define NH_TEA_UNSIGNED         5u
#define NH_UNSIGNED_DELTA       3u      /* signed version + this: unsigned */

/* a major hash no name gets: the position past every name of a listing */
#define NH_END                  0xFFFFFFFEu

/*
 * The major and minor hash of a name. Seed: the volume's s_hash_seed (four
 * words; all zero, or NULL, means the default). Returns 0, or -1 for a
 * version this code does not know.
 */
int NhHash(const unsigned char *Name, unsigned int Length, unsigned int Version,
           const unsigned int *Seed, unsigned int *Major, unsigned int *Minor);

#endif /* _EXT4_NAME_HASH_H_ */
