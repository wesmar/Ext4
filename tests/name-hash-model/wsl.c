/*
 * wsl.c - the production name hashes (src/ext4/NameHash.c) against
 * e2fsprogs: writes debugfs dx_hash commands and what each must print.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * usage: wsl <count> <seed> <commands> <expected>
 *
 * Names of 1 to 255 bytes from every byte value debugfs takes in a command
 * (not blanks, quotes, '#' or the backslash; no '-' first), every hash version (the
 * unsigned ones by number), with a random seed or none. Built and run by
 * tests/name-hash-wsl.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include "../../src/ext4/NameHash.c"

static unsigned long long Rng;

static unsigned int Next(void)
{
    Rng ^= Rng >> 12;
    Rng ^= Rng << 25;
    Rng ^= Rng >> 27;
    return (unsigned int)((Rng * 2685821657736338717ull) >> 32);
}

static unsigned char NameByte(void)
{
    for (;;) {
        unsigned int b = 0x21 + Next() % (0x100 - 0x21);
        if (b != '"' && b != '\'' && b != '#' && b != '\\' && b != 0x7F)
            return (unsigned char)b;
    }
}

int main(int argc, char **argv)
{
    unsigned int count, i, k;
    FILE *cmds, *want;

    if (argc < 5) {
        fprintf(stderr, "usage: wsl <count> <seed> <commands> <expected>\n");
        return 2;
    }
    count = (unsigned int)strtoul(argv[1], NULL, 10);
    Rng = strtoull(argv[2], NULL, 10) * 0x9E3779B97F4A7C15ull + 1;
    cmds = fopen(argv[3], "wb");
    want = fopen(argv[4], "w");
    if (!cmds || !want)
        return 2;

    for (i = 0; i < count; i++) {
        unsigned char name[256], uuid[16];
        unsigned int length, version, seed[4], major, minor;
        int seeded = Next() % 3 != 0;

        /* short names often: the padding of a partial piece */
        length = Next() % 4 ? 1 + Next() % 40 : 1 + Next() % 255;
        version = Next() % 6;
        for (k = 0; k < length; k++) {
            do name[k] = NameByte();
            while (k == 0 && name[k] == '-');   /* an option to debugfs */
        }
        for (k = 0; k < 16; k++)
            uuid[k] = (unsigned char)Next();
        /* the seed words are the uuid's bytes, little-endian (s_hash_seed) */
        for (k = 0; k < 4; k++)
            seed[k] = uuid[4 * k] | uuid[4 * k + 1] << 8 | uuid[4 * k + 2] << 16 | (unsigned int)uuid[4 * k + 3] << 24;
        if (!seeded || !(seed[0] | seed[1] | seed[2] | seed[3]))
            seeded = 0;

        /* always a seed: without one debugfs takes the open volume's; zero
           is the default */
        fprintf(cmds, "dx_hash -h %u -s ", version);
        for (k = 0; k < 16; k++)
            fprintf(cmds, "%02x%s", seeded ? uuid[k] : 0, k == 3 || k == 5 || k == 7 || k == 9 ? "-" : "");
        fprintf(cmds, " ");
        fwrite(name, 1, length, cmds);
        fprintf(cmds, "\n");

        NhHash(name, length, version, seeded ? seed : NULL, &major, &minor);
        fprintf(want, "0x%x 0x%x\n", major, minor);
    }
    fclose(cmds);
    fclose(want);
    return 0;
}
