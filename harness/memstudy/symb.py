#!/usr/bin/env python3
# symb.py <sample.txt> <out.sym.txt> [syms] : replace "??? (in kyty_emulator) load address ... [0xADDR]" with symbol+off.
# syms = `nm -n kyty_emulator | c++filt` output (regenerated if missing/older than the binary).
import sys, re, bisect, os, subprocess
src, dst = sys.argv[1], sys.argv[2]
B = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '_Build', 'macos-x86_64', 'kyty_emulator')
syms = sys.argv[3] if len(sys.argv) > 3 else '/tmp/kyty.syms.dm'
if not os.path.exists(syms) or os.path.getmtime(syms) < os.path.getmtime(B):
    out = subprocess.run("nm -n '%s' | c++filt > '%s'" % (B, syms), shell=True)
A, N = [], []
for l in open(syms, errors='replace'):
    p = l.rstrip('\n').split(' ', 2)
    if len(p) == 3 and p[1] in 'tTsS':
        try: A.append(int(p[0], 16)); N.append(p[2])
        except ValueError: pass
pat = re.compile(r'\?\?\?  \(in kyty_emulator\)  load address 0x[0-9a-f]+ \+ 0x[0-9a-f]+  \[0x([0-9a-f]+)\]')
def rep(m):
    a = int(m.group(1), 16); i = bisect.bisect_right(A, a) - 1
    return '%s+0x%x' % (N[i], a - A[i]) if i >= 0 else m.group(0)
with open(dst, 'w') as o:
    for l in open(src, errors='replace'): o.write(pat.sub(rep, l))
