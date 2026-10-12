#!/usr/bin/env python3
# profsum.py <sym.txt> [topN] [thread-substr...] : per-thread inclusive function shares from a macOS `sample` call tree.
# A function counts once per path (topmost occurrence). Prints the busiest threads (non-idle samples) with top functions.
import sys, re
f = sys.argv[1]; top = int(sys.argv[2]) if len(sys.argv) > 2 else 25; filt = sys.argv[3:]
thr = re.compile(r'^    (\d+) (Thread_\d+.*)$')
node = re.compile(r'^[ +!:|]*?(\d+) (.+?)(?:  \(in [^)]*\))?(?: \+ \d+)?(?:  \[0x[0-9a-f]+\])?\s*$')
IDLE = ('mach_msg', '__psynch_cvwait', '__semwait_signal', 'kevent', '__workq_kernreturn', '__select', 'nanosleep', '__ulock_wait', 'semaphore_wait', '__psynch_mutexwait', 'poll', 'mach_msg2_internal', 'mach_msg_overwrite', '_pthread_cond_wait', 'std::condition_variable::wait', 'std::this_thread::sleep_for', 'MVKSemaphoreImpl::wait', 'usleep')
threads = []; cur = None
lines = open(f, errors='replace').read().split('\n')
start = next((i for i, l in enumerate(lines) if l.startswith('Call graph:')), 0)
for l in lines[start + 1:]:
    if l.startswith('Total number in stack') or l.startswith('Binary Images'): break
    m = thr.match(l)
    if m: cur = {'name': m.group(2).strip(), 'total': int(m.group(1)), 'nodes': []}; threads.append(cur); continue
    if cur is None or not l.strip(): continue
    m = re.match(r'^([ +!:|]*)(\d+) (.*)$', l)
    if not m: continue
    depth = len(m.group(1)); cnt = int(m.group(2)); name = m.group(3)
    name = re.sub(r'  \[0x[0-9a-f]+\]$', '', name); name = re.sub(r'\+0x[0-9a-f]+$', '', name)
    name = re.sub(r'  \(in ([^)]*)\) \+ \d+$', r' (\1)', name)
    cur['nodes'].append((depth, cnt, name))
for t in threads:
    inc = {}; stack = []; idle = 0; leafidle = []
    for i, (d, c, n) in enumerate(t['nodes']):
        while stack and stack[-1][0] >= d: stack.pop()
        if not any(s[1] == n for s in stack): inc[n] = inc.get(n, 0) + c
        stack.append((d, n))
        nxt = t['nodes'][i + 1][0] if i + 1 < len(t['nodes']) else -1
        if nxt <= d and any(any(sn.startswith(x) for x in IDLE) for _, sn in stack[-4:]): idle += c
    t['inc'] = inc; t['idle'] = idle; t['busy'] = t['total'] - idle
sel = [t for t in threads if (not filt or any(x in t['name'] or any(x in k for k in list(t['inc'])[:12]) for x in filt))]
sel.sort(key=lambda t: -t['busy'])
for t in sel[:6 if not filt else 50]:
    print('== %s total=%d busy=%d (%.0f%%) idle=%d' % (t['name'], t['total'], t['busy'], 100. * t['busy'] / max(1, t['total']), t['idle']))
    for n, c in sorted(t['inc'].items(), key=lambda x: -x[1])[:top]:
        print('  %5.1f%%  %s' % (100. * c / t['total'], n[:150]))
