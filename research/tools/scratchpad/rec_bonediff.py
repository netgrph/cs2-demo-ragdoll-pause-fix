import sys, struct, math
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT, EV
# Per event in a frame window: which bones of the bone object changed, max |delta| of position and of quaternion, bones touched,
# plus the lastAnim of the event. Also lists every other tracked object whose label matches argv[4] with its changed word count.
path = sys.argv[1]; f0 = int(sys.argv[2]); f1 = int(sys.argv[3])
extra = sys.argv[4].lower() if len(sys.argv) > 4 else None
ins = Insight(path); mm = ins.mm
objs = {}
bone_oid = None
lastE = None
acc = {}
def flush():
    if lastE is None or not (f0 <= lastE[1] <= f1): acc.clear(); return
    parts = []
    for oid, (bones, dpos, dq, words) in acc.items():
        o = objs[oid]
        if oid == bone_oid:
            parts.append(f"BONES {words}w {len(bones)} bones dpos {dpos:.3f} dq {dq:.4f} first {sorted(bones)[:6]}")
        else:
            parts.append(f"#{oid} {o['label'][:40]} {words}w")
    print(f"f{lastE[1]:<6} {EV.get(lastE[0], lastE[0]):<7} p{lastE[4]} tick {lastE[5]} last {lastE[13]:<6} " + ' | '.join(parts))
    acc.clear()
for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        flush()
        v = EFMT.unpack_from(mm, p); lastE = v
        if v[1] > f1: break
    elif tag == 'O':
        r = Rd(mm, p); oid, kind, flags, addr, vt, size, cls, root, depth = r.u('<IBBQQIIBB'); lab = r.s()
        objs[oid] = dict(label=lab, kind=kind, mem=bytearray(size))
        if kind == 2: bone_oid = oid
    elif tag == 'S':
        oid = struct.unpack_from('<I', mm, p)[0]
        if oid in objs: objs[oid]['mem'][:] = mm[p + 4:p + n]
    elif tag == 'M':
        r = Rd(mm, p); wall, fr = r.u('<dI'); txt = r.s()
        if f0 <= fr <= f1 and not txt.startswith('CENSUS'): print('   MARK', txt[:200])
    elif tag == 'D':
        oid, cnt = struct.unpack_from('<II', mm, p)
        o = objs.get(oid)
        if not o: continue
        pairs = struct.unpack_from(f'<{2 * cnt}I', mm, p + 8)
        m = o['mem']
        track = oid == bone_oid or (extra and extra in o['label'].lower())
        rec = acc.setdefault(oid, [set(), 0.0, 0.0, 0]) if track and lastE is not None and f0 <= lastE[1] <= f1 else None
        for k in range(0, 2 * cnt, 2):
            off, new = pairs[k], pairs[k + 1]
            if off + 4 > len(m): continue
            if rec is not None:
                old = struct.unpack_from('<f', m, off)[0]
                nf = struct.unpack('<f', struct.pack('<I', new))[0]
                rec[3] += 1
                if oid == bone_oid:
                    b, field = divmod(off, 32)
                    rec[0].add(b)
                    dd = abs(nf - old) if math.isfinite(nf) and math.isfinite(old) else 0.0
                    if field < 12: rec[1] = max(rec[1], dd)
                    elif field >= 16: rec[2] = max(rec[2], dd)
            struct.pack_into('<I', m, off, new)
