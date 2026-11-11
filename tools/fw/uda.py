#!/usr/bin/env python3
"""User data area (UDA) flash files: the drive the phone's flasher writes after the ROM layers
(docs/devices.md "User data layer"). A UDA .fpsx is a FIASCO container whose data chunks are pieces of a
FAT file system (the boot sector at the lowest flash location), written sparse: only the boot sector,
the FATs and the used clusters are in the file. On the N8 it is C: (234 MB FAT32, empty on 111.040).

  tools/fw/uda.py info FPSX...            chunks, FAT facts, the files in it
  tools/fw/uda.py image FPSX OUT [--min-size BYTES]
                                         a sparse drive image of the file system's size (at least
                                         --min-size), the chunks written at their places
Run with python3 -I (firmware is untrusted input).
"""
import argparse, mmap, os, shutil, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fiasco


class UdaError(Exception):
    pass


def read(path):
    """(base location, [(offset in the drive, bytes)]) of a UDA flash file"""
    with open(path, 'rb') as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            chunks = [(loc, bytes(m[q:q + size])) for p, tag, d, name, size, loc, q in fiasco.blocks(m)
                      if tag not in (0x27, 0x28) and size]
        except ValueError as e:
            raise UdaError('%s: not a FIASCO file (%s)' % (path, e))
        finally:
            m.close()
    if not chunks:
        raise UdaError('%s: no data chunks' % path)
    base = min(loc for loc, _ in chunks)
    return base, sorted((loc - base, data) for loc, data in chunks)


def fat_facts(boot):
    """FAT boot sector -> dict (size in bytes, type, label, oem); raises if it is not one"""
    if len(boot) < 512 or boot[510:512] != b'\x55\xaa':
        raise UdaError('no FAT boot sector at the start of the user data (MBR or another format)')
    bps, spc, res, nfats, roots, tot16 = struct.unpack_from('<HBHBHH', boot, 11)
    tot32 = struct.unpack_from('<I', boot, 32)[0]
    total = tot16 or tot32
    fat32 = boot[82:87] == b'FAT32'
    if not bps or not total:
        raise UdaError('FAT boot sector with zero sector size or count')
    label = boot[71:82] if fat32 else boot[43:54]
    return {'bytes': total * bps, 'sector': bps, 'cluster': bps * spc, 'type': 'FAT32' if fat32 else 'FAT12/16',
            'oem': boot[3:11].decode('latin1').strip(), 'label': label.decode('latin1').strip()}


def write_image(path, out, min_size=0):
    """the drive image: sparse, the file system's size (or min_size if larger); returns the facts"""
    base, chunks = read(path)
    if chunks[0][0] != 0:
        raise UdaError('%s: the lowest chunk is not at the start of the drive' % path)
    facts = fat_facts(chunks[0][1][:512])
    size = max(facts['bytes'], min_size)
    end = max(off + len(data) for off, data in chunks)
    if end > size:
        raise UdaError('%s: data runs to 0x%x, past the file system (0x%x)' % (path, end, size))
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.truncate(size)
        for off, data in chunks:
            f.seek(off)
            f.write(data)
    os.replace(tmp, out)
    facts.update(base=base, size=size, data=sum(len(d) for _, d in chunks), chunks=len(chunks))
    return facts


def files(img):
    """recursive listing (mtools), hidden and system files included"""
    if not shutil.which('mdir'):
        return ['(mtools not installed: no file listing)']
    r = subprocess.run(['mdir', '-/', '-a', '-b', '-i', img, '::/'], capture_output=True, text=True,
                       env=dict(os.environ, MTOOLS_SKIP_CHECK='1'), stdin=subprocess.DEVNULL)
    return [l for l in r.stdout.splitlines() if l.strip()]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    i = sub.add_parser('info'); i.add_argument('fpsx', nargs='+')
    m = sub.add_parser('image'); m.add_argument('fpsx'); m.add_argument('out')
    m.add_argument('--min-size', type=int, default=0)
    a = ap.parse_args()
    try:
        if a.cmd == 'image':
            f = write_image(a.fpsx, a.out, a.min_size)
            print('%s: %s %s "%s", %d bytes (%d of data in %d chunks)' % (a.out, f['type'], f['oem'], f['label'],
                                                                         f['size'], f['data'], f['chunks']))
            return
        for p in a.fpsx:
            with tempfile.TemporaryDirectory() as d:
                img = os.path.join(d, 'uda.img')
                f = write_image(p, img)
                ls = files(img)
            print('%s\n  flash 0x%08x, %s by "%s", label "%s", %d MB, cluster %d, %d bytes of data in %d chunks'
                  % (p, f['base'], f['type'], f['oem'], f['label'], f['size'] >> 20, f['cluster'], f['data'], f['chunks']))
            print('  files: %d%s' % (len(ls), ''.join('\n    ' + l for l in ls[:40])))
    except UdaError as e:
        sys.exit('uda: %s' % e)


if __name__ == '__main__':
    main()
