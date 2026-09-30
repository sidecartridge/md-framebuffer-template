#!/usr/bin/env python3
"""Worst-case stack depth from GCC -fcallgraph-info=su output (.ci files).

Static call edges only. Indirect calls (the jobs core 1 runs through
fb_core1_dispatch(), the demo modules' function pointers, the devhooks
handler, IRQs) are not followed, so stitch them by hand:

  # largest application frames
  stackdepth.py BUILD_DIR top
  # deepest path below each named function: core 0 from main, core 1's
  # loop and the jobs it runs
  stackdepth.py BUILD_DIR roots main fb_core1_loop fb_c2p_bottom_job
  # heaviest path between two functions (segments are summed)
  stackdepth.py BUILD_DIR path 'main>fb_publish'

The build needs -fcallgraph-info=su: tools/dev/measure_builds.sh passes it.

Frames from precompiled libraries (newlib, libgcc) count as 0, and interrupt
stacking is not included, so real peaks are higher than reported.
"""
import re, sys, os, collections
bdir = sys.argv[1]
node_re = re.compile(r'node: \{ title: "([^"]+)" label: "([^"]*)"')
edge_re = re.compile(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"')
frames = {}        # title -> bytes
defs_by_name = collections.defaultdict(list)
edges = collections.defaultdict(set)
indirect = collections.defaultdict(int)
for root, _, files in os.walk(bdir):
    for fn in files:
        if not fn.endswith('.ci'):
            continue
        txt = open(os.path.join(root, fn), errors='replace').read()
        for t, label in node_re.findall(txt):
            m = re.search(r'\\n(\d+) bytes', label)
            if m:
                frames[t] = int(m.group(1))
                name = t.split(':')[-1]
                defs_by_name[name].append(t)
        for s, t in edge_re.findall(txt):
            edges[s].add(t)
            if t == '__indirect_call':
                indirect[s] += 1

def resolve(t):
    if t in frames:
        return [t]
    name = t.split(':')[-1]
    cands = defs_by_name.get(name, [])
    # prefer __wrap_ versions (pico SDK wraps printf/malloc family)
    w = defs_by_name.get('__wrap_' + name, [])
    return w or cands

memo = {}
onstack = set()
def depth(t):
    if t in memo:
        return memo[t]
    if t in onstack:
        return (0, [t + ' (recursion)'])
    onstack.add(t)
    best = (0, [])
    for c in edges.get(t, ()):
        for r in resolve(c):
            d = depth(r)
            if d[0] > best[0]:
                best = d
    onstack.discard(t)
    res = (frames.get(t, 0) + best[0], [t] + best[1])
    memo[t] = res
    return res

def short(t):
    parts = t.split(':')
    return parts[-1] + ' [' + os.path.basename(parts[0]) + ']' if len(parts) > 1 else t

def report(root_name, show=12):
    cands = defs_by_name.get(root_name, [])
    if not cands:
        print(f"{root_name:40s} (not found)")
        return None
    best = max((depth(c) for c in cands), key=lambda x: x[0])
    chain = ' > '.join(short(x) + f"({frames.get(x,0)})" for x in best[1][:show])
    print(f"{root_name:34s} {best[0]:6d}  {chain}{' > ...' if len(best[1])>show else ''}")
    return best[0]

mode = sys.argv[2] if len(sys.argv) > 2 else 'roots'
if mode == 'top':
    app = [(b, t) for t, b in frames.items() if '/rp/src/' in t]
    for b, t in sorted(app, reverse=True)[:25]:
        print(f"{b:6d}  {short(t)}")
else:
    for r in sys.argv[3:]:
        report(r)

# ---- path mode: longest-frame path from A to B (static edges) ----
def path_to(src_name, dst_name):
    dsts = set(defs_by_name.get(dst_name, []))
    memo2 = {}
    stack2 = set()
    def walk(t):
        if t in dsts:
            return (frames.get(t, 0), [t])
        if t in memo2:
            return memo2[t]
        if t in stack2:
            return None
        stack2.add(t)
        best = None
        for c in edges.get(t, ()):
            for r in resolve(c):
                w = walk(r)
                if w and (best is None or w[0] > best[0]):
                    best = w
        stack2.discard(t)
        res = (frames.get(t, 0) + best[0], [t] + best[1]) if best else None
        memo2[t] = res
        return res
    best = None
    for s in defs_by_name.get(src_name, []):
        w = walk(s)
        if w and (best is None or w[0] > best[0]):
            best = w
    return best

if mode == 'path':
    pairs = sys.argv[3:]
    total = 0
    for p in pairs:
        a, b = p.split('>')
        w = path_to(a, b)
        if not w:
            print(f"  {a} -> {b}: no static path"); continue
        # exclude the destination frame from the segment (it is counted by the next segment)
        seg = w[0] - frames.get(w[1][-1], 0)
        total += seg
        print(f"  {a} -> {b}: {seg:5d}  " + ' > '.join(short(x)+f"({frames.get(x,0)})" for x in w[1]))
    print(f"  prefix total (excluding final callee): {total}")
