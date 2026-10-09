"""Collect the call tree of guest functions from generated code and count mnemonics.
calltree.py <generated_dir> <depth> <sub_ADDR>..."""
import glob, re, sys, collections
gen, depth, roots = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
bodies = {}
for f in glob.glob(gen + '/*_recomp.*.cpp'):
    name = None; buf = []
    for line in open(f, encoding='utf-8', errors='replace'):
        m = re.match(r'DEFINE_REX_FUNC\((sub_[0-9A-F]+)\)', line)
        if m: name = m.group(1); buf = []; continue
        if name:
            if line.startswith('}'): bodies[name] = ''.join(buf); name = None
            else: buf.append(line)
seen = set(); frontier = list(roots)
for _ in range(depth + 1):
    nxt = []
    for fn in frontier:
        if fn in seen or fn not in bodies: continue
        seen.add(fn)
        nxt += re.findall(r'(sub_[0-9A-F]{8})\(ctx, base\)', bodies[fn])
        # function pointers built with lis/addi show up as constants; include code addresses
        for v in re.findall(r'ctx\.r\d+\.s64 = (-?\d+);', bodies[fn]): pass
    frontier = nxt
cnt = collections.Counter()
for fn in seen: cnt.update(re.findall(r'^\s*// ([a-z][a-z0-9.]*)', bodies[fn], re.M))
print(len(seen), 'functions:', ' '.join(sorted(seen))[:2000])
print(' '.join(f'{k}:{v}' for k, v in cnt.most_common() if k.startswith(('v', 'lv', 'stv', 'mf', 'dcb'))))
