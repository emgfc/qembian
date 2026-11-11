#!/usr/bin/env python3
"""Inspect a syborg GPU transport capture without booting the phone.

Default: stream a summary. --record N: decode one record including its full payload.
--extract N FILE: extract the original command bytes (LE). Records are numbered from 0.
The first marked presentation ends a partial frame; the next 30 are complete frames.
"""
import argparse
import collections
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parent.parent


def records(path):
    with open(path, 'rb') as f:
        if f.read(8) != b'SYGLCAP1':
            raise ValueError('Not a SYGLCAP1 capture')
        index = 0
        while True:
            offset = f.tell()
            header = f.read(8)
            if not header:
                return
            if len(header) != 8:
                raise ValueError(f'Truncated record header at {offset}')
            owner, count = struct.unpack('<II', header)
            if not 2 <= count <= 16 * 1024 * 1024:
                raise ValueError(f'Invalid word count at {offset}: {count}')
            data = f.read(count * 4)
            if len(data) != count * 4:
                raise ValueError(f'Truncated payload at {offset}')
            op, declared = struct.unpack_from('<II', data)
            if declared != count:
                raise ValueError(f'Command length mismatch at {offset}')
            yield index, offset, owner, op, data
            index += 1


def names(path):
    schema = Path(str(path) + '.gl2.h')
    if not schema.exists():
        schema = ROOT / 'src/qemu-syborg/hw/arm/syborg_gl2_fn.h'
    return {int(value): name for name, value in re.findall(r'GL2_FN_(\w+) = (\d+)', schema.read_text())}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture', type=Path)
    ap.add_argument('--record', type=int)
    ap.add_argument('--extract', nargs=2, metavar=('RECORD', 'FILE'))
    args = ap.parse_args()
    functions = names(args.capture)
    counts = collections.Counter()
    draws = []
    frames = 0
    started = False
    total = 0
    for index, offset, owner, op, data in records(args.capture):
        total += 1
        fn = struct.unpack_from('<I', data, 12)[0] if op == 20 and len(data) >= 16 else None
        name = functions.get(fn, str(fn)) if fn is not None else f'op:{op}'
        counts[name] += 1
        if op == 0xfffffff0:
            started = True
        if op == 0xfffffff1:
            frames = struct.unpack_from('<I', data, 8)[0]
        if started and name in ('glDrawArrays', 'glDrawElements'):
            draws.append({'record': index, 'frame': frames, 'owner': owner, 'call': name})
        if args.extract and index == int(args.extract[0]):
            Path(args.extract[1]).write_bytes(data)
            return
        if args.record == index:
            words = struct.unpack('<%dI' % (len(data) // 4), data)
            out = dict(record=index, offset=offset, owner=owner, call=name, words=words)
            if fn is not None:
                out['context'] = words[2]
                if name.startswith('glUniform'):
                    out['argument_float_view'] = struct.unpack('<%df' % (len(words) - 4), data[16:])
                if name == 'glShaderSource' and len(words) >= 6:
                    out['source'] = data[24:24 + words[5]].decode('utf8', 'replace')
            print(json.dumps(out, indent=2))
            return
    if args.record is not None or args.extract:
        raise ValueError('Record not found')
    print(json.dumps(dict(records=total, complete=frames == 31, complete_frames=max(0, frames - 1),
                          calls=counts, draws=draws), indent=2))


if __name__ == '__main__':
    main()
