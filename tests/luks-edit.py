#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# luks-edit.py <device> zero-digest | segment-cipher <spec>
# Rewrite the JSON metadata of a LUKS2 header - both copies, checksums
# recomputed, so the header still reads as valid - into what a corrupt or
# foreign header may say:
#   zero-digest            the volume-key digest emptied: a digest of no bytes
#                          compares equal to anything, so a tool that does not
#                          insist on a real one takes every passphrase
#   segment-cipher <spec>  the data segment encrypted with another cipher than
#                          the keyslots (cryptsetup itself would need the
#                          kernel to have it in order to format so)
import hashlib, json, struct, sys

dev, what = sys.argv[1], sys.argv[2]
with open(dev, 'r+b') as f:
    first = f.read(4096)
    if first[:6] != b'LUKS\xba\xbe' or struct.unpack('>H', first[6:8])[0] != 2:
        sys.exit('not a LUKS2 header')
    hdr_size = struct.unpack('>Q', first[8:16])[0]
    for offset in (0, hdr_size):
        f.seek(offset)
        area = bytearray(f.read(hdr_size))
        meta = json.loads(area[4096:].rstrip(b'\0').decode())
        if what == 'zero-digest':
            for d in meta['digests'].values():
                d['digest'] = ''
        elif what == 'segment-cipher':
            for s in meta['segments'].values():
                s['encryption'] = sys.argv[3]
        else:
            sys.exit('unknown edit ' + what)
        text = json.dumps(meta, separators=(',', ':')).encode()
        if len(text) > hdr_size - 4096:
            sys.exit('metadata does not fit')
        area[4096:] = text + b'\0' * (hdr_size - 4096 - len(text))
        area[448:448 + 64] = b'\0' * 64          # csum, zero while hashing
        area[448:448 + 32] = hashlib.sha256(area).digest()
        f.seek(offset)
        f.write(area)
print(what, 'written to both header copies')
