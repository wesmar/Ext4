#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# mmp-seq.py <device> <seq>: set the sequence number of an ext4 MMP block, as
# another node would leave it, with the metadata checksum made again.
# debugfs cannot: it takes the volume through MMP itself and writes CLEAN
# when it closes. Used by features-image.sh to stage the MMP test cases.
import struct, sys

def crc32c(crc, data):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc

dev, seq = sys.argv[1], int(sys.argv[2], 0)
with open(dev, 'r+b') as f:
    f.seek(1024)
    sb = f.read(1024)
    log_block = struct.unpack_from('<I', sb, 0x18)[0]
    block_size = 1024 << log_block
    incompat, ro_compat = struct.unpack_from('<I', sb, 0x60)[0], struct.unpack_from('<I', sb, 0x64)[0]
    mmp_block = struct.unpack_from('<Q', sb, 0x168)[0]
    if ro_compat & 0x400:                       # metadata_csum
        seed = struct.unpack_from('<I', sb, 0x270)[0] if incompat & 0x2000 else crc32c(0xFFFFFFFF, sb[0x68:0x78])
    f.seek(mmp_block * block_size)
    mmp = bytearray(f.read(1024))
    struct.pack_into('<I', mmp, 4, seq)
    if ro_compat & 0x400:
        struct.pack_into('<I', mmp, 1020, crc32c(seed, mmp[:1020]))
    f.seek(mmp_block * block_size)
    f.write(mmp)
print('mmp seq 0x%08x at block %d' % (seq, mmp_block))
