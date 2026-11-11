#!/usr/bin/env bash
# Every relative markdown link in docs/, models/, personalities/, platform/ and CLAUDE.md resolves.
#   tests/docs-links.sh      (exit 1 and the broken links listed)
cd "$(dirname "$0")/.."
python3 - <<'PY'
import os, re, sys
bad = 0
files = ['CLAUDE.md'] + [os.path.join(d, f) for top in ('docs', 'models', 'personalities', 'platform')
                         for d, _, fs in os.walk(top) for f in fs if f.endswith('.md')]
for p in files:
    for m in re.finditer(r'\]\(([^)#\s]+)(#[^)]*)?\)', open(p, encoding='utf-8', errors='replace').read()):
        tgt = m.group(1)
        if re.match(r'^[a-z]+:', tgt) or tgt.startswith('/'):
            continue
        if not os.path.exists(os.path.normpath(os.path.join(os.path.dirname(p), tgt))):
            print('%s: broken link %s' % (p, tgt)); bad += 1
print('%d broken link(s) in %d files' % (bad, len(files)))
sys.exit(1 if bad else 0)
PY
