#!/usr/bin/env python3
"""Builds an MMC card image for the N-Gage board: MBR with one FAT16 partition, then the contents of folders copied to
the card root (a game folder with System/Apps/..., as N-Gage games are distributed for the memory card).

  python3 eka1/tools/ngage_card.py OUT.img [--mb 64] [--label NGAGE] DIR...

Each DIR's children are copied to the card root (so pass the folder that contains `System`). Needs mtools
(mformat, mcopy). The image is the card as a block device: the board serves it as E: (-M ngage,mmc=OUT.img).
"""
import argparse
import os
import struct
import subprocess
import sys

START = 63                     # first partition sector (classic CHS alignment, as SD/MMC formatters wrote)


def write_mbr(path, total):
    mbr = bytearray(512)
    mbr[0x1BE:0x1BE + 16] = struct.pack('<B3sB3sII', 0x00, b'\x01\x01\x00', 0x06, b'\xfe\xff\xff', START,
                                        total - START)
    mbr[510:512] = b'\x55\xaa'
    with open(path, 'r+b') as f:
        f.write(mbr)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('out')
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('--mb', type=int, default=64)
    ap.add_argument('--label', default='NGAGE')
    a = ap.parse_args()
    total = a.mb * 2048
    with open(a.out, 'wb') as f:
        f.truncate(total * 512)
    write_mbr(a.out, total)
    part = '%s@@%d' % (a.out, START * 512)
    subprocess.run(['mformat', '-i', part, '-H', str(START), '-T', str(total - START), '-h', '16', '-s', '63',
                    '-v', a.label, '::'], check=True)
    for d in a.dirs:
        for name in sorted(os.listdir(d)):
            subprocess.run(['mcopy', '-i', part, '-s', '-o', os.path.join(d, name), '::/'], check=True)
    subprocess.run(['mdir', '-i', part, '-/', '::/'], check=False, stdout=sys.stderr)
    print('%s: %d MB, FAT16 at sector %d' % (a.out, a.mb, START))


if __name__ == '__main__':
    main()
