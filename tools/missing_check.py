"""Check which files the game failed to open exist elsewhere on disc or inside .big archives.
missing_check.py <missing.txt> <game_dir>"""
import os, sys
miss = [l.strip() for l in open(sys.argv[1]) if l.strip()]
game = sys.argv[2]
names = {}
for root, ds, fs in os.walk(game):
    for f in fs: names.setdefault(f.lower(), os.path.join(root, f))
bigs = [names[n] for n in names if n.endswith(('.big', '.bh'))]
blob = {b: open(b, 'rb').read(4 << 20).lower() for b in bigs}  # archive tables live at the start
loose = inbig = nowhere = 0; nowhere_list = []
for m in miss:
    base = m.replace(chr(92), '/').rstrip('/').split('/')[-1].lower()
    if '.' not in base: continue
    if base in names: loose += 1; print('ON DISC ELSEWHERE', m, '->', names[base]); continue
    hit = [b for b, d in blob.items() if base.encode() in d]
    if hit: inbig += 1
    else: nowhere += 1; nowhere_list.append(m)
print(f'loose elsewhere={loose} inside .big={inbig} nowhere={nowhere}')
for m in nowhere_list[:60]: print('  NOWHERE', m)
