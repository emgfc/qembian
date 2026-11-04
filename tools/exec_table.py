#!/usr/bin/env python3
"""Number the executive calls in an EKA2 execs.txt the way genexec does (fast calls
0x00800000|n, slow calls n, in file order) and optionally compare against the SVC tables
in EKA2L1's src/emu/kernel/src/svc.cpp (matching on normalised names).

  tools/exec_table.py execs.txt                   # print our numbering
  tools/exec_table.py execs.txt EKA2L1/svc.cpp    # compare with each EKA2L1 table
"""
import re, sys

def ours(path):
    t = re.sub(r"/\*.*?\*/", "", open(path, errors="replace").read(), flags=re.S)
    t = re.sub(r"//[^\n]*", "", t)
    out, nf, ns = {}, 0, 0
    for kind, body in re.findall(r"\b(fast|slow)\s*\{(.*?)\}", t, re.S):
        m = re.search(r"\bname\s*=\s*(\w+)", body)
        name = m.group(1) if m else "?"
        if kind == "fast":
            out[0x00800000 | nf] = name; nf += 1
        else:
            out[ns] = name; ns += 1
    return out

def eka2l1(path):
    t = open(path, errors="replace").read()
    return {m.group(1): {int(a, 16): b for a, b in re.findall(r"BRIDGE_REGISTER\((0x[0-9A-Fa-f]+),\s*(\w+)\)", m.group(2))}
            for m in re.finditer(r"func_map svc_register_funcs_(\w+) = \{(.*?)\n    \};", t, re.S)}

norm = lambda s: re.sub(r"(ver\d+|v\d+)$", "", s.lower().replace("_", ""))

if __name__ == "__main__":
    o = ours(sys.argv[1])
    if len(sys.argv) == 2:
        for n, name in sorted(o.items()): print(hex(n), name)
        sys.exit()
    for ver, tab in eka2l1(sys.argv[2]).items():
        common = [n for n in tab if n in o]
        same = [n for n in common if norm(tab[n]) == norm(o[n])]
        print(f"EKA2L1 {ver:5s}: {len(tab):3d} entries; numbers also in ours {len(common):3d}; "
              f"name matches at same number {len(same):3d} ({100*len(same)//max(1,len(common))}%)")
