#!/usr/bin/env python3
"""Flash files out of Nokia's Windows firmware installers, without running them (untrusted input:
python3 -I, nothing from the package is executed).

  unpack.py EXE OUT     InstallShield setup.exe (N97 RM-505 v11 "RM-505 v.11.00 N-97.exe"): the files
                        appended to the PE (Disk1/setup.ini, data1.hdr, data1.cab, data2.cab ...) and then
                        every file of its InstallShield cabinets (data1.hdr + dataN.cab) -> OUT/files/,
                        then the flash files of Nokia's compressed_files.dpc and the package metadata
                        (.vpl .dcp signature .pcx .ini .txt .xml) -> OUT/firmware/: import that directory
                        (tools/fw/store.py import OUT/firmware)
  unpack.py EXE OUT     Inno Setup 5.2 installer (E71 RM-346 "RM-346 v.300.2 E-71.exe"): every file it
                        installs into {app} -> OUT/firmware/ (a name given twice with other bytes, as the
                        per-product .xml files: name~2, name~3 ...)
  unpack.py --cab HDR OUT   only the cabinet set of a data1.hdr (dataN.cab next to it) -> OUT/files
  unpack.py --dpc DPC OUT   only a compressed_files.dpc -> OUT/firmware

Formats [verified on RM-505 11.0.021, InstallShield 11.50]:
  setup.exe overlay (after the last PE section): records  name\\0 path\\0 version\\0 size(decimal)\\0 data[size]
  InstallShield cabinet "ISc(" (version 0x0100xxxx, major (v >> 12) & 0xf >= 6): common header
    (signature, version, volume info, cab descriptor offset +0x0c), cab descriptor: file table offset +0x0c,
    directory count +0x1c, file count +0x28, file descriptor offset +0x2c; the file table starts with the
    directory name offsets; descriptors 0x57 bytes: flags u16 (1 split, 2 obfuscated, 4 compressed,
    8 invalid), expanded u64 @2, compressed u64 @0xa, data offset u64 @0x12, name offset u32 @0x3a,
    directory u16 @0x3e, volume u16 @0x55. Compressed data: chunks of <u16 length><raw deflate>.
  compressed_files.dpc (Nokia DPcompactor): LZMA properties[5], u32 file count, per file <u32 name length>
    <name> <u32 0> <u64 size>, then one raw LZMA1 stream: the files concatenated in that order.
  Inno Setup 5.2.3 [verified on RM-346 300.21.012]: the loader table "rDlPtS\xcd\xe6\xd7\x7b\x0b\x2a"
    (version 1 at +12: setup-0 offset +32, setup-1 offset +36); setup-0 = 64-byte id "Inno Setup Setup Data
    (5.2.3)", then two blocks <u32 crc><u32 stored size><u8 compressed> of 4096-byte pieces each after a
    u32 crc, LZMA1 (5 property bytes, raw stream): the header with the file entries (9 strings
    <u32 length><bytes>: source, destination ...; two 10-byte version records; u32 location index) and
    the file locations, 70 bytes each (first/last slice, chunk start u32 @8 from setup-1, offset in the
    chunk u64 @12, size u64 @20, chunk compressed size u64 @28, md5, time, version, flags u16 @68: 0x80
    compressed, 0x10 x86 call filter (not undone: such files are reported)). A chunk = "zlb\x1a" + LZMA1.
Split files (spanning two dataN.cab) and obfuscated ones are reported, not written [inferred: none in
the Nokia packages seen].
"""
import argparse, mmap, os, re, struct, sys, zlib


def safe_join(root, rel):
    parts = [p for p in rel.replace('\\', '/').split('/') if p not in ('', '.')]
    if not parts or '..' in parts or ':' in parts[0]:
        raise ValueError('unsafe path in package: %r' % rel)
    return os.path.join(root, *parts)


def pe_overlay(m):
    if m[:2] != b'MZ':
        raise ValueError('not a PE executable')
    pe = struct.unpack_from('<I', m, 0x3c)[0]
    if m[pe:pe + 4] != b'PE\0\0':
        raise ValueError('no PE header')
    ns, = struct.unpack_from('<H', m, pe + 6)
    opt, = struct.unpack_from('<H', m, pe + 20)
    return max(sum(struct.unpack_from('<II', m, pe + 24 + opt + 40 * i + 16)) for i in range(ns))


def is_overlay(path, out):
    """the files an InstallShield setup.exe carries after its PE image -> [written paths]"""
    written = []
    with open(path, 'rb') as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        p = pe_overlay(m)

        def z(p):
            e = m.find(b'\0', p, p + 1024)
            if e < 0:
                raise ValueError('no InstallShield file record at 0x%x' % p)
            return m[p:e].decode('latin1'), e + 1
        while p < len(m):
            name, p = z(p)
            rel, p = z(p)
            ver, p = z(p)
            size, p = z(p)
            if not size.isdigit() or p + int(size) > len(m):
                raise ValueError('bad InstallShield file record %r at 0x%x' % (name, p))
            size = int(size)
            dst = safe_join(out, rel or name)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as g:
                for o in range(p, p + size, 1 << 24):
                    g.write(m[o:min(o + (1 << 24), p + size)])
            written.append(dst)
            p += size
    return written


def cab_files(hdr_path):
    """[(directory, name, flags, expanded, compressed, data offset, volume)] of an InstallShield cabinet set"""
    h = open(hdr_path, 'rb').read()
    sig, ver, _, cdo = struct.unpack_from('<4sIII', h, 0)
    if sig != b'ISc(':
        raise ValueError('%s: not an InstallShield cabinet header' % hdr_path)
    major = (ver >> 12) & 0xf if ver >> 24 == 1 else (ver & 0xffff) // 100
    if major < 6:
        raise ValueError('%s: InstallShield cabinet version %d (only 6 and later read)' % (hdr_path, major))
    cd = cdo
    fto, = struct.unpack_from('<I', h, cd + 0x0c)
    ndir, = struct.unpack_from('<I', h, cd + 0x1c)
    nfile, fto2 = struct.unpack_from('<II', h, cd + 0x28)
    ft = cd + fto

    def cstr(off):
        e = h.index(b'\0', off)
        return h[off:e].decode('latin1')
    dirs = [cstr(ft + struct.unpack_from('<I', h, ft + 4 * i)[0]) for i in range(ndir)]
    out = []
    for i in range(nfile):
        p = ft + fto2 + i * 0x57
        flags, exp, comp, off = struct.unpack_from('<HQQQ', h, p)
        noff, di = struct.unpack_from('<IH', h, p + 0x3a)
        vol, = struct.unpack_from('<H', h, p + 0x55)
        if flags & 8 or not noff:
            continue
        out.append((dirs[di] if di < len(dirs) else '', cstr(ft + noff), flags, exp, comp, off, vol))
    return out


def cab_extract(hdr_path, out):
    """every file of the cabinet set -> out/<directory>/<name>; returns ([written], [problems])"""
    base = os.path.dirname(hdr_path)
    stem = os.path.basename(hdr_path)[:-len('1.hdr')]
    written, problems, vols = [], [], {}
    for d, name, flags, exp, comp, off, vol in cab_files(hdr_path):
        rel = (d + '\\' if d else '') + name
        if flags & 3:
            problems.append('%s: %s, not extracted' % (rel, 'split over cabinets' if flags & 1 else 'obfuscated'))
            continue
        if vol not in vols:
            f = open(os.path.join(base, '%s%d.cab' % (stem, vol)), 'rb')
            vols[vol] = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        m = vols[vol]
        dst = safe_join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as g:
            if flags & 4:
                p, end, n = off, off + comp, 0
                while p < end:
                    ln, = struct.unpack_from('<H', m, p)
                    z = zlib.decompressobj(-15)
                    b = z.decompress(m[p + 2:p + 2 + ln])
                    g.write(b)
                    n += len(b)
                    p += 2 + ln
            else:
                g.write(m[off:off + exp])
                n = exp
        if n != exp:
            problems.append('%s: %d bytes out, %d expected' % (rel, n, exp))
        written.append(dst)
    return written, problems


META = ('.vpl', '.dcp', '.bin', '.pcx', '.ini', '.txt', '.xml', '.spr')


def dpc_extract(path, out):
    """Nokia compressed_files.dpc -> out/<name> for each file it holds; returns [written]"""
    import lzma
    with open(path, 'rb') as f:
        head = f.read(1 << 16)
        props, n = head[:5], struct.unpack_from('<I', head, 5)[0]
        if props[0] >= 9 * 5 * 5 or not 0 < n < 1000:
            raise ValueError('%s: not a DPcompactor file' % path)
        p, ents = 9, []
        for _ in range(n):
            ln, = struct.unpack_from('<I', head, p)
            name = head[p + 4:p + 4 + ln].decode('latin1')
            _, size = struct.unpack_from('<IQ', head, p + 4 + ln)
            ents.append((name, size))
            p += 16 + ln
        dict_size, = struct.unpack_from('<I', props, 1)
        dec = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[{
            'id': lzma.FILTER_LZMA1, 'dict_size': dict_size, 'lc': props[0] % 9,
            'lp': props[0] // 9 % 5, 'pb': props[0] // 45}])
        f.seek(p)
        pending, written = b'', []
        for name, size in ents:
            dst = safe_join(out, name)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as g:
                left = size
                while left:
                    if not pending:
                        if dec.needs_input:
                            data = f.read(1 << 20)
                            if not data:
                                raise ValueError('%s: the stream ends inside %s' % (path, name))
                        else:
                            data = b''
                        pending = dec.decompress(data, max_length=1 << 22)
                        continue
                    k = min(left, len(pending))
                    g.write(pending[:k])
                    pending, left = pending[k:], left - k
            written.append(dst)
    return written


INNO_LOADER = b'rDlPtS\xcd\xe6\xd7\x7b\x0b\x2a'


def lzma1(props):
    ds, = struct.unpack_from('<I', props, 1)
    import lzma
    return lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[{
        'id': lzma.FILTER_LZMA1, 'dict_size': ds, 'lc': props[0] % 9, 'lp': props[0] // 9 % 5, 'pb': props[0] // 45}])


def inno_block(m, p):
    """one setup-0 block at p -> (its bytes, the offset after it)"""
    crc, size, comp = struct.unpack_from('<IIB', m, p)
    p += 9
    end, raw = p + size, bytearray()
    while p < end:
        n = min(4096, end - p - 4)
        raw += m[p + 4:p + 4 + n]
        p += 4 + n
    if comp:
        raw = lzma1(raw[:5]).decompress(bytes(raw[5:]))
    return bytes(raw), end


def inno_extract(path, out):
    """the {app} files of an Inno Setup 5.2 installer -> out; returns ([written], [problems])"""
    with open(path, 'rb') as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        t = m.find(INNO_LOADER)
        if t < 0 or struct.unpack_from('<I', m, t + 12)[0] != 1:
            raise ValueError('%s: no Inno Setup loader table (version 1)' % path)
        off0, off1 = struct.unpack_from('<II', m, t + 32)
        ident = m[off0:off0 + 64].rstrip(b'\0').decode('latin1')
        if not ident.startswith('Inno Setup Setup Data (5.2.'):
            raise ValueError('%s: %r (only Inno Setup 5.2 is read)' % (path, ident))
        hdr, p = inno_block(m, off0 + 64)
        locs, _ = inno_block(m, p)
        if len(locs) % 70:
            raise ValueError('%s: file location block of %d bytes is not 70-byte records' % (path, len(locs)))
        loc = [struct.unpack_from('<iiIQQQ', locs, i) + struct.unpack_from('<H', locs, i + 68)
               for i in range(0, len(locs), 70)]
        files = {}				# location index -> destination names
        for mt in re.finditer(rb'\{app\}\\', hdr):
            ln, = struct.unpack_from('<I', hdr, mt.start() - 4)
            name = hdr[mt.start() + 6:mt.start() + ln].decode('latin1')
            if struct.unpack_from('<I', hdr, mt.start() - 8)[0] != 0:
                continue				# not a file entry (its source name is empty: the file is inside)
            q = mt.start() + ln
            for _ in range(7):				# font, assembly, components, tasks, languages, check, after-install
                n, = struct.unpack_from('<I', hdr, q)
                q += 4 + n
                if n > 0x10000:
                    break
            if n > 0x10000:
                continue				# {app}\... in another kind of entry (icons, run, ini)
            li, = struct.unpack_from('<i', hdr, q + 20)	# after the two 10-byte version records
            if 0 <= li < len(loc) and name:
                files.setdefault(li, []).append(name)
        written, problems, seen = [], [], {}
        chunks = {}
        for li, names in files.items():
            chunks.setdefault((loc[li][0], loc[li][2]), []).append(li)
        for (_, start), lis in sorted(chunks.items()):
            c = off1 + start
            if m[c:c + 4] != b'zlb\x1a':
                raise ValueError('%s: no chunk at 0x%x' % (path, c))
            comp = loc[lis[0]][5]
            dec, src, pos, pending = lzma1(m[c + 4:c + 9]), c + 9, 0, b''
            for li in sorted(lis, key=lambda i: loc[i][3]):
                sub, size, flags = loc[li][3], loc[li][4], loc[li][6]
                while pos + len(pending) < sub + size:		# decompress up to the end of this file
                    data = m[src:min(src + (1 << 20), c + 4 + comp)] if dec.needs_input else b''
                    src += len(data)
                    if not data and dec.needs_input:
                        raise ValueError('%s: chunk at 0x%x ends early' % (path, c))
                    pending += dec.decompress(data, max_length=1 << 24)
                    if sub > pos and len(pending) > (1 << 26):		# drop what lies before this file
                        k = min(sub - pos, len(pending))
                        pending, pos = pending[k:], pos + k
                body = pending[sub - pos:sub - pos + size]
                if flags & 0x10:
                    problems.append('%s: x86 call filter, not extracted' % files[li][0])
                    continue
                for name in dict.fromkeys(files[li]):
                    key = name.lower()
                    seen.setdefault(key, [])
                    if body in seen[key]:
                        continue
                    seen[key].append(body)
                    dst = safe_join(out, name + ('~%d' % len(seen[key]) if len(seen[key]) > 1 else ''))
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    open(dst, 'wb').write(body)
                    written.append(dst)
    return written, problems


def firmware_dir(files, out):
    """the flash files of every .dpc and the Nokia package metadata among files -> out (store import input)"""
    import shutil
    got = []
    for p in files:
        if p.lower().endswith('.dpc'):
            got += dpc_extract(p, out)
    for p in files:
        if p.lower().endswith(META) and os.path.getsize(p):
            dst = os.path.join(out, os.path.basename(p))
            shutil.copy2(p, dst)
            got.append(dst)
    return got


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('src', help='setup.exe (or data1.hdr with --cab)')
    ap.add_argument('out', help='a new, empty directory')
    ap.add_argument('--cab', action='store_true', help='src is an InstallShield data1.hdr')
    ap.add_argument('--dpc', action='store_true', help='src is a Nokia compressed_files.dpc')
    a = ap.parse_args()
    if os.path.exists(a.out) and os.listdir(a.out):
        raise SystemExit('%s is not empty' % a.out)
    os.makedirs(a.out, exist_ok=True)
    try:
        if a.dpc:
            print('%d files' % len(dpc_extract(a.src, os.path.join(a.out, 'firmware'))))
            return 0
        if a.cab:
            hdrs = [a.src]
        elif open(a.src, 'rb').read(1 << 16).find(b'Inno Setup') >= 0:
            w, problems = inno_extract(a.src, os.path.join(a.out, 'firmware'))
            print('Inno Setup: %d files in %s' % (len(w), os.path.join(a.out, 'firmware')))
            for p in problems:
                print('PROBLEM: %s' % p)
            return 1 if problems else 0
        else:
            got = is_overlay(a.src, os.path.join(a.out, 'setup'))
            print('setup.exe: %d files' % len(got))
            hdrs = [p for p in got if p.lower().endswith('1.hdr')]
        problems, files = [], []
        for h in hdrs:
            w, p = cab_extract(h, os.path.join(a.out, 'files'))
            print('%s: %d files' % (os.path.relpath(h, a.out), len(w)))
            problems += p
            files += w
        fw = firmware_dir(files, os.path.join(a.out, 'firmware'))
        if fw:
            print('firmware: %d files in %s' % (len(fw), os.path.join(a.out, 'firmware')))
    except (ValueError, OSError, struct.error, zlib.error) as e:
        raise SystemExit('unpack: %s' % e)
    for p in problems:
        print('PROBLEM: %s' % p)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
