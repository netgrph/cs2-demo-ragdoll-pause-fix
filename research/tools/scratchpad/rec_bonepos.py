import sys, struct, os, math
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT, EV
# Bone positions over time: per event, the centroid of the (last created) bone object and the root bone, printed whenever
# the centroid moved more than 0.05 units, or at pause changes / markers. Shows exactly when the displayed pose moves.
path = sys.argv[1]
f0 = int(sys.argv[2]) if len(sys.argv) > 2 else 0
f1 = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 30
ins = Insight(path); mm = ins.mm
objs = {}
bone_oid = None
prev = None
prevP = None
lastE = None
pend_marks = []
def centroid(m):
    n = len(m) // 32
    sx = sy = sz = 0.0; k = 0
    for i in range(n):
        x, y, z = struct.unpack_from('<3f', m, i * 32)
        if all(math.isfinite(t) and abs(t) < 20000 for t in (x, y, z)) and (x or y or z):
            sx += x; sy += y; sz += z; k += 1
    return (sx / k, sy / k, sz / k, k) if k else None
def emit(v):
    global prev, prevP
    if bone_oid is None: return
    c = centroid(objs[bone_oid]['mem'])
    if not c: return
    moved = prev is None or max(abs(c[i] - prev[i]) for i in range(3)) > 0.05
    if moved or v[4] != prevP or pend_marks:
        d = '' if prev is None else ' d %+.2f %+.2f %+.2f' % tuple(c[i] - prev[i] for i in range(3))
        root = struct.unpack_from('<3f', objs[bone_oid]['mem'], 0)
        print(f"f{v[1]:<6} {EV.get(v[0], v[0]):<7} p{v[4]} tick {v[5]} lastAnim {v[13]:<6} cent {c[0]:9.2f} {c[1]:8.2f} {c[2]:8.2f}{d:<26} root {root[0]:.1f} {root[1]:.1f} {root[2]:.1f}"
              + (' | ' + ' || '.join(pend_marks) if pend_marks else ''))
        pend_marks.clear()
    prev = c; prevP = v[4]
for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        # apply: the previous E's D records are in; print the state after the previous event
        if lastE is not None and f0 <= lastE[1] <= f1: emit(lastE)
        v = EFMT.unpack_from(mm, p)
        lastE = v
        if v[1] > f1: break
    elif tag == 'O':
        r = Rd(mm, p); oid, kind, flags, addr, vt, size, cls, root, depth = r.u('<IBBQQIIBB'); lab = r.s()
        objs[oid] = dict(label=lab, addr=addr, mem=bytearray(size))
        if kind == 2: bone_oid = oid; prev = None
    elif tag == 'S':
        oid = struct.unpack_from('<I', mm, p)[0]
        if oid in objs: objs[oid]['mem'][:] = mm[p + 4:p + n]
    elif tag == 'M':
        r = Rd(mm, p); wall, fr = r.u('<dI'); txt = r.s()
        if f0 <= fr <= f1 and not txt.startswith('CENSUS'): pend_marks.append(txt[:90])
    elif tag == 'D':
        oid, cnt = struct.unpack_from('<II', mm, p)
        o = objs.get(oid)
        if not o: continue
        pairs = struct.unpack_from(f'<{2 * cnt}I', mm, p + 8)
        m = o['mem']
        for k in range(0, 2 * cnt, 2):
            off, new = pairs[k], pairs[k + 1]
            if off + 4 <= len(m): struct.pack_into('<I', m, off, new)
