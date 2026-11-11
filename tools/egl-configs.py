#!/usr/bin/env python3
"""EGL configs of a real phone, from an eglprobe report (src/n95apps/eglprobe), as a device profile file the
host EGL serves (src/syvg/syvg_egl.cpp; docs/devices/nokia-n8-rm596/gles-host.md):

  egl-configs.py PROBE.txt > models/<model>/egl-configs.txt

One line per config, in the phone's order: hex attribute=value pairs (attributes the phone refused are left
out)."""
import re, sys

cfgs, cur = [], None
for ln in open(sys.argv[1], errors='replace'):
    if re.match(r'config \d+:', ln):
        cur = []
        cfgs.append(cur)
        continue
    m = re.match(r'\s+\w+ \((0x[0-9a-f]+)\) = (-?\d+)', ln)
    if m and cur is not None:
        cur.append('%s=%s' % (m.group(1), m.group(2)))
    elif cur is not None and not ln.startswith(' '):
        cur = None
print('# EGL configs of the real phone (eglprobe %s), one per line: attribute=value (hex attribute)' % sys.argv[1].split('/')[-1])
for c in cfgs:
    print(' '.join(c))
