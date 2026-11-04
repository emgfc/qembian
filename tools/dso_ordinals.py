#!/usr/bin/env python3
"""Print "<ordinal> <symbol>" for a Symbian ARM import library (.dso) or frozen .def file.

Symbian DSOs (made by elf2e32) are ELF shared objects whose dynamic symbols each point at a
32-bit word holding that export's ordinal. .def files list "symbol @ N NONAME [...]".

  tools/dso_ordinals.py euser.dso            # or euseru.def
  tools/dso_ordinals.py --diff OLD NEW       # ABI diff by ordinal (OLD = baseline)
"""
import struct, sys, re


def from_def(path):
    out = {}
    for line in open(path, errors="replace"):
        m = re.match(r"\s*(\S+)\s+@\s*(\d+)\s+NONAME(.*)", line)
        if m:
            out[int(m.group(2))] = (m.group(1), "ABSENT" in m.group(3))
    return out


def from_dso(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
        raise SystemExit(f"{path}: not a 32-bit little-endian ELF")
    e_shoff, = struct.unpack_from("<I", d, 0x20)
    e_shentsize, e_shnum = struct.unpack_from("<HH", d, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", d, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    out = {}
    for s in secs:
        if s[1] != 11:  # SHT_DYNSYM
            continue
        strtab = secs[s[6]]
        for off in range(s[4], s[4] + s[5], 16):
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from("<IIIBBH", d, off)
            if st_shndx == 0 or st_shndx >= len(secs):
                continue
            name = d[strtab[4] + st_name:d.index(b"\0", strtab[4] + st_name)].decode()
            tgt = secs[st_shndx]
            ordinal, = struct.unpack_from("<I", d, tgt[4] + (st_value - tgt[3]))
            out[ordinal] = (name, False)
    return out


def load(path):
    return from_def(path) if path.lower().endswith(".def") else from_dso(path)


def main(a):
    if a[:1] == ["--diff"]:
        old, new = load(a[1]), load(a[2])
        same = moved = gone = 0
        for o, (name, _) in sorted(old.items()):
            n = new.get(o)
            if n and n[0] == name and not n[1]:
                same += 1
            elif n:
                moved += 1
                print(f"CHANGED  @{o}: {name}  ->  {n[0]}{' (ABSENT)' if n[1] else ''}")
            else:
                gone += 1
                print(f"MISSING  @{o}: {name}")
        extra = len([o for o in new if o not in old])
        print(f"# baseline exports {len(old)}: identical {same}, changed {moved}, missing {gone}; "
              f"new-only ordinals {extra} (max old {max(old)}, max new {max(new)})")
        return
    for o, (name, absent) in sorted(load(a[0]).items()):
        print(o, name, "ABSENT" if absent else "")


if __name__ == "__main__":
    main(sys.argv[1:])
