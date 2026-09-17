"""ddump.py FILE lo hi id[,id...] [maxlines]  - every changed word of the chosen census objects, per event, with field names."""
import sys, struct
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT, EV, KIND, fmt_val, DROP
ins = Insight(sys.argv[1]); mm = ins.mm
lo, hi = int(sys.argv[2]), int(sys.argv[3])
want = set(int(x) for x in sys.argv[4].split(','))
maxl = int(sys.argv[5]) if len(sys.argv) > 5 else 400
ev = None
for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        v = EFMT.unpack_from(mm, p)
        ev = dict(ev=v[0], frame=v[1], tick=v[5], paused=v[4], lastAnim=v[13], steps=v[12])
        if ev['frame'] > hi: break
    elif tag == 'M':
        r = Rd(mm, p); wall, frame = r.u('<dI'); t = r.s()
        if lo <= frame <= hi and not t.startswith(('CENSUS', 'WATCH page')): print(f'  == f{frame} {t}')
    elif tag == 'O':
        r = Rd(mm, p)
        oid, kind, flags, addr, vt, size, cls, root, depth = r.u('<IBBQQIIBB')
        cls = struct.unpack('<i', struct.pack('<I', cls))[0]
        ins.objs[oid] = dict(id=oid, kind=kind, addr=addr, size=size, cls=cls, label=r.s(), mem=bytearray(size))
        if oid in want and ev and lo <= ev['frame'] <= hi: print(f"  ++ f{ev['frame']} NEW #{oid} {addr:x} {ins.objs[oid]['label']}")
    elif tag == 'S':
        oid = struct.unpack_from('<I', mm, p)[0]
        if oid in ins.objs: ins.objs[oid]['mem'][:] = mm[p + 4:p + n]
    elif tag == 'X':
        oid, why = struct.unpack_from('<IB', mm, p)
        if oid in want and ev and lo <= ev['frame'] <= hi: print(f"  -- f{ev['frame']} DROP #{oid} {DROP.get(why, why)}")
    elif tag == 'K':
        r = Rd(mm, p); cls = r.u('<I'); raw, pretty, module = r.s(), r.s(), r.s(); ins.classes[cls] = pretty
    elif tag == 'F':
        r = Rd(mm, p); cls, cnt = r.u('<II'); ent = []
        for _ in range(cnt):
            off = r.u('<I'); ent.append((off, r.s(), r.s(), r.s()))
        ent.sort(); ins.fields[cls] = ([e[0] for e in ent], [(e[1], e[2]) for e in ent])
    elif tag == 'D':
        oid, cnt = struct.unpack_from('<II', mm, p)
        o = ins.objs.get(oid)
        if not o or ev is None: continue
        pairs = struct.unpack_from(f'<{2 * cnt}I', mm, p + 8)
        show = oid in want and lo <= ev['frame'] <= hi
        lines = []
        for k in range(0, 2 * cnt, 2):
            off, new = pairs[k], pairs[k + 1]
            old = struct.unpack_from('<I', o['mem'], off)[0]
            struct.pack_into('<I', o['mem'], off, new)
            if show and len(lines) < maxl:
                fo, fn = struct.unpack('<ff', struct.pack('<II', old, new))
                lines.append(f'{ins.field(o, off):40} {old:08x} -> {new:08x}   ({fmt_val(old)} -> {fmt_val(new)})')
        if show:
            print(f"f{ev['frame']} {EV.get(ev['ev'])} tick{ev['tick']} {'P' if ev['paused'] else 'p'} lastAnim{ev['lastAnim']} steps{ev['steps']}  #{oid} {o['label'][:50]}: {cnt} words")
            for l in lines: print('     ' + l)
