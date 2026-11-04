#!/usr/bin/env python3
"""Minimal EKA2 XIP ROM image reader (TRomHeader / TRomDir / TRomImageHeader, see e32rom.h).

  romimg.py IMG                 header summary + file list (address, size, name)
  romimg.py IMG --images        also TRomImageHeader per executable (code/data/exports/deps)
  romimg.py IMG --extract DIR   write every file (raw XIP bytes, as in ROM) under DIR
  romimg.py IMG --json OUT      machine-readable dump (used by tools/xip_analysis.py)

IMG must start at the TRomHeader (offset 0 == iRomBase).
"""
import argparse, json, os, struct, sys

E32_UIDS_EXE = 0x1000007A
E32_UIDS_DLL = 0x10000079


class Rom:
    def __init__(self, data):
        self.d = data
        h = struct.unpack_from('<I Q I I I I I I I I I I Q I I I I I I I I I I I I I I I I', data, 124)
        (self.restart, self.time, self.timehi, self.base, self.size, self.rootlist, self.kerndata,
         self.kernlimit, self.primary, self.secondary, self.checksum, self.hardware, self.language,
         self.kcfgflags, self.excsearch, self.hdrsize, self.sectionhdr, self.svdata, self.variant,
         self.extension, self.relocinfo, self.oldtrace, self.userdata, self.userdatasize,
         self.debugport, self.version, self.comptype, self.compsize, self.uncompsize) = h

    def inrom(self, a):
        return self.base <= a < self.base + len(self.d)

    def u32(self, a):
        return struct.unpack_from('<I', self.d, a - self.base)[0]

    def bytes(self, a, n):
        o = a - self.base
        return self.d[o:o + n]

    def walk(self):
        n = self.u32(self.rootlist)
        seen = set()
        for i in range(n):
            hw, addr = struct.unpack_from('<II', self.d, self.rootlist - self.base + 4 + 8 * i)
            yield from self._dir(addr, '', hw, seen)

    def _dir(self, addr, path, hw, seen):
        if addr in seen or not self.inrom(addr):
            return
        seen.add(addr)
        size = self.u32(addr)
        p = addr + 4
        end = p + size
        while p < end:
            esz, ealin, att, nl = struct.unpack_from('<IIBB', self.d, p - self.base)
            name = self.bytes(p + 10, nl * 2).decode('utf-16le', 'replace')
            if att & 0x10:
                yield from self._dir(ealin, path + name + '/', hw, seen)
            else:
                yield dict(path=path + name, addr=ealin, size=esz, att=att, hw=hw)
            p += (10 + nl * 2 + 3) & ~3

    def image(self, addr):
        f = struct.unpack_from('<4I 10I I I I 4I I I I I I I I I I', self.d, addr - self.base)
        keys = ('uid1 uid2 uid3 uidck entry code data codesize textsize datasize bsssize heapmin heapmax '
                'stack dllref expcount expdir sid vid cap0 cap1 toolsver flags priority databsslin '
                'nextext hwvar totaldata modver excdesc').split()
        r = dict(zip(keys, f))
        deps = []
        if r['dllref'] and self.inrom(r['dllref']):
            fl, cnt = struct.unpack_from('<HH', self.d, r['dllref'] - self.base)
            deps = list(struct.unpack_from('<%dI' % cnt, self.d, r['dllref'] - self.base + 4))
        r['deps'] = deps
        return r


def is_image(rom, e):
    if e['size'] < 0x60 or not rom.inrom(e['addr']):
        return False
    u1 = rom.u32(e['addr'])
    return u1 in (E32_UIDS_EXE, E32_UIDS_DLL)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('img')
    ap.add_argument('--images', action='store_true')
    ap.add_argument('--extract')
    ap.add_argument('--json')
    a = ap.parse_args()
    data = open(a.img, 'rb').read()
    rom = Rom(data)
    print('# romBase=0x%08x romSize=0x%x root=0x%08x kernData=0x%08x kernLimit=0x%08x primary=0x%08x'
          % (rom.base, rom.size, rom.rootlist, rom.kerndata, rom.kernlimit, rom.primary))
    print('# hardware=0x%08x variantFile=0x%08x extensionFile=0x%08x version=%s compression=0x%x (%d->%d) hdrsize=0x%x'
          % (rom.hardware, rom.variant, rom.extension, hex(rom.version), rom.comptype, rom.compsize,
             rom.uncompsize, rom.hdrsize))
    files = list(rom.walk())
    byaddr = {}
    out = []
    for e in files:
        rec = dict(e)
        if is_image(rom, e):
            im = rom.image(e['addr'])
            rec['image'] = im
            byaddr[e['addr']] = e['path']
        out.append(rec)
    for rec in out:
        line = '0x%08x %9d %s' % (rec['addr'], rec['size'], rec['path'])
        if a.images and 'image' in rec:
            im = rec['image']
            line += ('\n    %s uid3=%08x code=%08x+%x text=%x data=%08x+%x bss=%x exports=%d@%08x sid=%08x vid=%08x '
                     'caps=%08x%08x tools=%08x flags=%08x dbl=%08x\n    deps: %s') % (
                'EXE' if im['uid1'] == E32_UIDS_EXE else 'DLL', im['uid3'], im['code'], im['codesize'],
                im['textsize'], im['data'], im['datasize'], im['bsssize'], im['expcount'], im['expdir'],
                im['sid'], im['vid'], im['cap1'], im['cap0'], im['toolsver'], im['flags'], im['databsslin'],
                ' '.join(byaddr.get(d, '?%08x' % d).split('/')[-1] for d in im['deps']))
        print(line)
    if a.extract:
        for rec in out:
            fn = os.path.join(a.extract, rec['path'].replace('\\', '/'))
            os.makedirs(os.path.dirname(fn), exist_ok=True)
            open(fn, 'wb').write(rom.bytes(rec['addr'], rec['size']))
    if a.json:
        json.dump(dict(base=rom.base, size=rom.size, kerndata=rom.kerndata, files=out), open(a.json, 'w'))


if __name__ == '__main__':
    main()
