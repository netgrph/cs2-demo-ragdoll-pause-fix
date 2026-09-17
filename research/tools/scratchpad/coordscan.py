"""coordscan.py FILE lo hi cx cy cz [R] - every census object whose changed words hold a float triple within R of (cx,cy,cz)."""
import sys, struct, collections
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT
ins = Insight(sys.argv[1]); mm = ins.mm
lo, hi = int(sys.argv[2]), int(sys.argv[3])
C = [float(x) for x in sys.argv[4:7]]; R = float(sys.argv[7]) if len(sys.argv) > 7 else 200
objs = {}; frame = 0
hits = collections.defaultdict(lambda: [0, None, None, set()])
def near(mem, off):
    if off < 0 or off + 12 > len(mem): return None
    t = struct.unpack_from('<3f', mem, off)
    return t if all(abs(t[k] - C[k]) < R for k in range(3)) else None
for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        frame = EFMT.unpack_from(mm, p)[1]
        if frame > hi: break
    elif tag == 'O':
        r = Rd(mm, p); oid, kind, flags, addr, vt, size, cls, root, depth = r.u('<IBBQQIIBB')
        objs[oid] = dict(label=r.s(), kind=kind, mem=bytearray(size), addr=addr)
    elif tag == 'S':
        oid = struct.unpack_from('<I', mm, p)[0]
        if oid in objs: objs[oid]['mem'][:] = mm[p + 4:p + n]
    elif tag == 'D':
        oid, cnt = struct.unpack_from('<II', mm, p)
        o = objs.get(oid)
        if not o: continue
        pairs = struct.unpack_from(f'<{2 * cnt}I', mm, p + 8)
        before = bytes(o['mem'])
        for k in range(0, 2 * cnt, 2): struct.pack_into('<I', o['mem'], pairs[k], pairs[k + 1])
        if not (lo <= frame <= hi) or o['kind'] == 2: continue
        seen = set()
        for k in range(0, 2 * cnt, 2):
            off = pairs[k]
            for base in (off, off - 4, off - 8):
                if base in seen: continue
                t = near(o['mem'], base)
                if t:
                    seen.add(base)
                    h = hits[(oid, base)]
                    h[0] += 1; h[3].add(frame)
                    if h[1] is None: h[1] = near(before, base) or struct.unpack_from('<3f', before, base) ; 
                    h[2] = t
for (oid, off), (c, first, last, frames) in sorted(hits.items(), key=lambda kv: -kv[1][0])[:80]:
    o = objs[oid]; fr = sorted(frames)
    print(f"#{oid} +0x{off:x} n{c} f{fr[0]}..{fr[-1]} {tuple(round(x,1) for x in first)} -> {tuple(round(x,1) for x in last)}  {o['label'][:90]}")
