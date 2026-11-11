#!/usr/bin/env python3
"""Read-only Nokia Phoenix A2/DCT4 unpacker; Python 3.9+, stdlib only.

Record layouts researched from g3gg0/TriX src/fmt_phoenix.c, revision
f35f8bfa90641e17ab4b33ba644482d303faa5cd. EKA1 on-disk structures cross-checked
against EKA2L1 loader/rom.h and rom.cpp. This is a new implementation.
Does NOT descramble S40 MCU code or decompress Symbian ROMs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys


class FormatError(ValueError):
    pass


def take(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise FormatError(f"out-of-bounds read: offset=0x{offset:x}, size=0x{size:x}")
    return data[offset:offset + size]


def be(data, offset, size):
    return int.from_bytes(take(data, offset, size), 'big')


def le(data, offset):
    return int.from_bytes(take(data, offset, 4), 'little')


def checksum(data):
    return (sum(data) & 255) ^ 255


def parse(data):
    if not data or data[0] != 0xa2:
        raise FormatError('expected Phoenix/DCT4 container magic A2; not BB5, RAR, or a raw ROM')
    end = 5 + be(data, 1, 4)
    take(data, 5, end - 5)
    count = be(data, 5, 4)
    pos = 9
    tokens = []
    for _ in range(count):
        if pos + 2 > end:
            raise FormatError('truncated header token')
        tid = data[pos]
        pos += 1
        width = 2 if tid == 0xf3 or data[pos] == 0 else 1
        size = be(data, pos, width)
        pos += width
        if pos + size > end:
            raise FormatError('token extends past header')
        value = take(data, pos, size)
        tokens.append(dict(id=f'0x{tid:02x}', offset=pos, size=size,
                           hex=value.hex(), text=''.join(chr(c) if 32 <= c < 127 else '.' for c in value)))
        pos += size
    if pos != end:
        raise FormatError(f'header token count/length mismatch at 0x{pos:x}, expected 0x{end:x}')
    records = []
    pos = end
    while pos < len(data):
        kind = data[pos]
        address = None
        device = None
        checks = {}
        if kind in (0x14, 0x21, 0x2e):
            hlen = 16 if kind == 0x21 else 10
            h = take(data, pos, hlen)
            if kind == 0x2e:
                device = be(h, 1, 2)
                address = be(h, 3, 2) << 9
            else:
                address = be(h, 7 if kind == 0x21 else 1, 4)
            size = be(h, hlen - 4, 3)
            payload = take(data, pos + hlen, size)
            # TriX only enables checksum validation for type 14.
            if kind == 0x14:
                calculated = checksum(payload)
                checks = dict(header_ok=checksum(h[1:-1]) == h[-1],
                              data_ok=calculated == h[5], data_stored=h[5],
                              data_calculated=calculated,
                              legacy_plus_one=((h[5] - calculated) & 255) == 1)
        elif kind == 0x20:
            hlen = 6
            take(data, pos, hlen)
            size = be(data, pos + 3, 2)
        elif kind == 0x25:
            hlen = 0
            size = 2 + be(data, pos + 1, 1)
        elif kind == 0x26:
            hlen, size = 0, 10
        else:
            raise FormatError(f'unknown record type 0x{kind:02x} at file offset 0x{pos:x}; refusing to guess boundaries')
        take(data, pos + hlen, size)
        records.append(dict(index=len(records), offset=pos, type=f'0x{kind:02x}',
                            header_size=hlen, size=size, address=address,
                            device=device, checksums=checks,
                            header_hex=data[pos:pos+hlen].hex()))
        pos += hlen + size
    return dict(header_size=end, tokens=tokens, records=records)


def swap16(data):
    result = bytearray(data)
    limit = len(data) & ~1
    result[0:limit:2], result[1:limit:2] = data[1:limit:2], data[0:limit:2]
    return bytes(result)


def safe_name(name):
    if not name or name in ('.', '..') or any(c in name for c in '/\\:\x00'):
        raise FormatError(f'unsafe ROM filename {name!r}')
    if any(ord(c) < 32 for c in name):
        raise FormatError('control character in ROM filename')
    return name


def read_eka1(image):
    """Validate an uncompressed EKA1 ROM and stage its complete file tree."""
    base, size, root_ptr = (le(image, n) for n in (0x8c, 0x90, 0x94))
    if base != 0x50000000 or not 0x100 <= size <= len(image):
        raise FormatError('not a complete EKA1 image')
    image = image[:size]
    # EKA1 has no EKA2 version field before compression: EKA1 compression type is at 0xe8.
    if le(image, 0xe8) != 0:
        raise FormatError('compressed EKA1 ROM; decompression is not implemented')
    root = root_ptr - base
    count = le(image, root)
    if not 1 <= count <= 64:
        raise FormatError('invalid ROM root count')
    files = []
    entries = []
    total = 0

    def walk(ptr, path, ancestors):
        nonlocal total
        off = ptr - base
        if off in ancestors or len(ancestors) > 64:
            raise FormatError('cyclic or excessively deep ROM directory')
        length = le(image, off)
        if length < 4:
            raise FormatError('invalid directory size')
        take(image, off, length)
        end, pos = off + length, off + 4
        used = set()
        while pos < end:
            hdr = take(image, pos, 10)
            nbytes, addr, attr, nchars = struct.unpack('<IIBB', hdr)
            if pos + 10 + 2*nchars > end:
                raise FormatError('directory name outside directory')
            name = safe_name(take(image, pos + 10, 2*nchars).decode('utf-16le'))
            if name.casefold() in used:
                raise FormatError('duplicate ROM directory entry')
            used.add(name.casefold())
            child = path + (name,)
            entries.append(dict(path='/'.join(child), address=addr, size=nbytes, attributes=attr))
            total += 1
            if total > 100000:
                raise FormatError('too many ROM entries')
            if attr & 0x10:
                walk(addr, child, ancestors | {off})
            else:
                files.append((child, take(image, addr - base, nbytes)))
            pos = (pos + 10 + 2*nchars + 3) & ~3
        if pos != end:
            raise FormatError('directory alignment mismatch')

    for i in range(count):
        hardware, ptr = struct.unpack('<II', take(image, root + 4 + i*8, 8))
        walk(ptr, (f'root_{i:02d}_{hardware:08x}',), set())
    return files, entries, size


def probe_eka1(blob, destination):
    found = []
    for swapped in (False, True):
        candidate = swap16(blob) if swapped else blob
        start = 0
        while True:
            marker = candidate.find(b'\x00\x00\x00\x50', start)
            if marker < 0:
                break
            start = marker + 1
            offset = marker - 0x8c
            if offset < 0 or offset % 4:
                continue
            try:
                files, entries, size = read_eka1(candidate[offset:])
            except (FormatError, UnicodeError):
                continue
            folder = destination / f'eka1_{offset:08x}_{"swap16" if swapped else "native"}'
            folder.mkdir()
            (folder / 'SYM.ROM').write_bytes(candidate[offset:offset+size])
            for path, contents in files:
                target = folder / 'z' / Path(*path)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(contents)
            (folder / 'files.json').write_text(json.dumps(entries, indent=2), encoding='utf-8')
            found.append(dict(directory=folder.name, offset=offset, swapped16=swapped,
                              size=size, file_count=len(files)))
    return found


def extract(source, destination, strict=False, rom=True):
    data = source.read_bytes()
    manifest = parse(data)  # Validate boundaries before creating output.
    failures = [r['index'] for r in manifest['records'] if r['checksums'] and
                (not r['checksums']['header_ok'] or not r['checksums']['data_ok'])]
    if failures and strict:
        raise FormatError(f'checksum mismatch in records {failures}')
    destination.mkdir(parents=True, exist_ok=False)
    (destination / 'header.bin').write_bytes(data[:manifest['header_size']])
    manifest.update(source=source.name, source_size=len(data),
                    sha256=hashlib.sha256(data).hexdigest(), checksum_mismatch_records=failures,
                    segments=[], symbian_roms=[])
    blockdir = destination / 'blocks'
    blockdir.mkdir()
    segments = []
    for r in manifest['records']:
        payload = take(data, r['offset'] + r['header_size'], r['size'])
        address = r['address']
        filename = f"{r['index']:05d}_type{r['type'][2:]}_{address if address is not None else 0:08x}.bin"
        (blockdir / filename).write_bytes(payload)
        r['file'] = 'blocks/' + filename
        if address is None:
            continue
        key = (r['type'], r['device'])
        # Merge only truly adjacent records on the same device/type.
        match = next((s for s in reversed(segments)
                      if s['key'] == key and s['address'] + len(s['data']) == address), None)
        if match is None:
            match = dict(key=key, address=address, data=bytearray())
            segments.append(match)
        match['data'].extend(payload)
    for i, s in enumerate(segments):
        blob = bytes(s['data'])
        name = f"segment_{i:03d}_type{s['key'][0][2:]}_{s['address']:08x}.bin"
        (destination / name).write_bytes(blob)
        manifest['segments'].append(dict(file=name, type=s['key'][0], device=s['key'][1],
                                         address=s['address'], size=len(blob)))
        if rom:
            sub = destination / f'roms_{i:03d}'
            sub.mkdir()
            result = probe_eka1(blob, sub)
            if not result:
                sub.rmdir()
            for item in result:
                item['segment'] = name
                item['directory'] = sub.name + '/' + item['directory']
                manifest['symbian_roms'].append(item)
    (destination / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(f"{source.name}: {len(manifest['records'])} records, {len(segments)} segments -> {destination}")
    print(f"  checksum mismatches: {len(failures)}; validated EKA1 ROMs: {len(manifest['symbian_roms'])}")
    if not manifest['symbian_roms'] and rom:
        print('  No complete uncompressed EKA1 tree detected. Raw segments are preserved; this does not prove scrambling.')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', nargs='+', type=Path)
    parser.add_argument('-o', '--output', type=Path, default=Path('dct4_out'))
    parser.add_argument('--strict-checksums', action='store_true')
    parser.add_argument('--no-rom', action='store_true', help='skip automatic uncompressed EKA1 extraction')
    args = parser.parse_args()
    failed = False
    for source in args.files:
        try:
            extract(source, args.output / source.name, args.strict_checksums, not args.no_rom)
        except (OSError, ValueError, RecursionError) as exc:
            print(f'{source}: {exc}', file=sys.stderr)
            failed = True
    return int(failed)


if __name__ == '__main__':
    sys.exit(main())
