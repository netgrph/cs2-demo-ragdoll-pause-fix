"""rectrack.py FILE lo hi oid base stride count bonesId - per event: record positions (vec3 at base+k*stride) of an object, vs bone centroid."""
import sys, struct
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT
ins = Insight(sys.argv[1]); mm = ins.mm
lo, hi, oid = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
base, stride, cnt, bid = int(sys.argv[5], 0), int(sys.argv[6], 0), int(sys.argv[7]), int(sys.argv[8])
mems = {}
evname = {1:'anim>',2:'anim<',3:'step>',4:'step<',5:'frame>',6:'frame<'}
prev = None
def bc(m):
    xs=[];
    for i in range(60):
        t = struct.unpack_from('<3f', m, i*32)
        if all(abs(v) < 1e5 for v in t) and t != (0,0,0): xs.append(t)
    return tuple(sum(p[k] for p in xs)/len(xs) for k in range(3))
for tag, p, n in records(mm, ins.start):
    if tag == 'O':
        r = Rd(mm, p); o, kind, flags, addr, vt, size, cls, root, depth = r.u('<IBBQQIIBB')
        if o in (oid, bid): mems[o] = bytearray(size)
    elif tag == 'S':
        o = struct.unpack_from('<I', mm, p)[0]
        if o in mems: mems[o][:] = mm[p + 4:p + n]
    elif tag == 'D':
        o, c = struct.unpack_from('<II', mm, p)
        if o not in mems: continue
        pairs = struct.unpack_from(f'<{2 * c}I', mm, p + 8)
        for k in range(0, 2 * c, 2): struct.pack_into('<I', mems[o], pairs[k], pairs[k + 1])
    elif tag == 'E':
        v = EFMT.unpack_from(mm, p)
        if v[1] > hi: break
        if v[1] < lo or oid not in mems or bid not in mems: continue
        recs = [struct.unpack_from('<3f', mems[oid], base + k * stride) for k in range(cnt)]
        b = bc(mems[bid])
        line = ' '.join(f"({x:.0f},{y:.0f},{z:.0f})" for x, y, z in recs)
        key = (line, round(b[1], 1))
        if key == prev: continue
        prev = key
        print(f"f{v[1]} {evname.get(v[0]):6} {'P' if v[4] else 'p'} st{v[12]} la{v[13]} bones({b[0]:.1f},{b[1]:.1f},{b[2]:.1f}) recs {line}")
    elif tag == 'M':
        r = Rd(mm, p); wall, frame = r.u('<dI'); t = r.s()
        if lo <= frame <= hi and t.startswith(('PAUSE', 'UNPAUSE', 'JUMP')): print(f'  == f{frame} {t[:100]}')
