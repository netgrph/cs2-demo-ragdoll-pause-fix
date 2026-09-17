"""multicent.py FILE lo hi bonesId label1,label2 cx cy cz [R] - per event: bone centroid and centroid of distinct near-center float triples (16-byte aligned) in objects whose label contains labelN."""
import sys, struct
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT
ins = Insight(sys.argv[1]); mm = ins.mm
lo, hi, bid = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
labs = sys.argv[5].split(',')
C = [float(x) for x in sys.argv[6:9]]; R = float(sys.argv[9]) if len(sys.argv) > 9 else 150
objs = {}
evname = {1:'anim>',2:'anim<',3:'step>',4:'step<',5:'frame>',6:'frame<'}
def cent(mem, stride, lim=None, near=True):
    pts = set()
    for off in range(0, min(len(mem), lim or len(mem)) - 11, stride):
        t = struct.unpack_from('<3f', mem, off)
        if near and not all(abs(t[k] - C[k]) < R for k in range(3)): continue
        if not near and (any(abs(v) > 1e5 for v in t) or t == (0, 0, 0)): continue
        pts.add(t)
    if not pts: return None
    return tuple(sum(p[k] for p in pts) / len(pts) for k in range(3)), len(pts)
for tag, p, n in records(mm, ins.start):
    if tag == 'O':
        r = Rd(mm, p); oid, kind, flags, addr, vt, size, cls, root, depth = r.u('<IBBQQIIBB'); lab = r.s()
        if oid == bid or any(l in lab for l in labs): objs[oid] = dict(mem=bytearray(size), lab=lab)
    elif tag == 'X':
        objs.pop(struct.unpack_from('<I', mm, p)[0], None)
    elif tag == 'S':
        oid = struct.unpack_from('<I', mm, p)[0]
        if oid in objs: objs[oid]['mem'][:] = mm[p + 4:p + n]
    elif tag == 'D':
        oid, cnt = struct.unpack_from('<II', mm, p)
        if oid not in objs: continue
        pairs = struct.unpack_from(f'<{2 * cnt}I', mm, p + 8)
        m = objs[oid]['mem']
        for k in range(0, 2 * cnt, 2): struct.pack_into('<I', m, pairs[k], pairs[k + 1])
    elif tag == 'E':
        v = EFMT.unpack_from(mm, p)
        if v[1] > hi: break
        if v[1] < lo: continue
        parts = []
        for oid, o in sorted(objs.items()):
            c = cent(o['mem'], 32, 60 * 32, near=False) if oid == bid else cent(o['mem'], 16)
            if c: parts.append(f"#{oid}({c[0][0]:.1f},{c[0][1]:.1f},{c[0][2]:.1f})n{c[1]}")
        print(f"f{v[1]} {evname.get(v[0], v[0]):6} {'P' if v[4] else 'p'} t{v[5]} la{v[13]} st{v[12]} " + ' '.join(parts))
    elif tag == 'M':
        r = Rd(mm, p); wall, frame = r.u('<dI'); t = r.s()
        if lo <= frame <= hi and t.startswith(('PAUSE', 'UNPAUSE', 'CATCHUP', 'JUMP', 'SETTLE', 'LOCK')): print(f'  == f{frame} {t[:140]}')
