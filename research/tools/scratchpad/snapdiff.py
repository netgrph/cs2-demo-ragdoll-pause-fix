"""snapdiff.py FILE frameA evA frameB evB cx cy cz R - all near-center float triples (4-aligned) in every live object at two event points, with change."""
import sys, struct
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT
ins = Insight(sys.argv[1]); mm = ins.mm
pts = [(int(sys.argv[2]), int(sys.argv[3])), (int(sys.argv[4]), int(sys.argv[5]))]
C = [float(x) for x in sys.argv[6:9]]; R = float(sys.argv[9])
objs = {}; snaps = []
def grab():
    out = {}
    for oid, o in objs.items():
        m = o['mem']
        for off in range(0, len(m) - 11, 4):
            t = struct.unpack_from('<3f', m, off)
            if all(abs(t[k] - C[k]) < R for k in range(3)): out[(oid, off)] = t
    return out
for tag, p, n in records(mm, ins.start):
    if tag == 'O':
        r = Rd(mm, p); oid, kind, flags, addr, vt, size, cls, root, depth = r.u('<IBBQQIIBB'); lab = r.s()
        objs[oid] = dict(mem=bytearray(size), lab=lab, kind=kind, addr=addr, depth=depth)
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
        if (v[1], v[0]) == pts[len(snaps)]:
            snaps.append((grab(), {k: dict(lab=o['lab'], kind=o['kind'], addr=o['addr'], size=len(o['mem'])) for k, o in objs.items()}))
            if len(snaps) == 2: break
(a, la), (b, lb) = snaps
keys = sorted(set(a) | set(b))
byobj = {}
for k in keys:
    byobj.setdefault(k[0], []).append(k)
for oid, ks in sorted(byobj.items()):
    info = lb.get(oid) or la.get(oid)
    moved = [k for k in ks if k in a and k in b and max(abs(a[k][i] - b[k][i]) for i in range(3)) > 0.5]
    print(f"#{oid} kind{info['kind']} {info['addr']:x} size{info['size']} {info['lab'][:80]}  triples A{sum(1 for k in ks if k in a)} B{sum(1 for k in ks if k in b)} moved{len(moved)}")
    for k in ks[:6] if not moved else moved[:6]:
        fa = a.get(k); fb = b.get(k)
        f = lambda t: '-' if t is None else f"({t[0]:.1f},{t[1]:.1f},{t[2]:.1f})"
        print(f"     +0x{k[1]:x} {f(fa)} -> {f(fb)}")
