import sys, struct, os, math
from collections import defaultdict
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT, EV
# Every changed word (old -> new, as int and float) per event in a frame window, for objects whose label matches an optional
# filter. Objects with more than MAXW changed words in one event are summarized.
path = sys.argv[1]; f0 = int(sys.argv[2]); f1 = int(sys.argv[3])
filt = sys.argv[4].lower() if len(sys.argv) > 4 else ''
MAXW = int(sys.argv[5]) if len(sys.argv) > 5 else 40
ins = Insight(path); mm = ins.mm
objs = {}
lastE = None
def fl(u):
    f = struct.unpack('<f', struct.pack('<I', u))[0]
    return f'{f:.5g}' if math.isfinite(f) and (f == 0 or 1e-6 < abs(f) < 1e9) else '-'
for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        v = EFMT.unpack_from(mm, p)
        lastE = v
        if v[1] > f1: break
        if f0 <= v[1]:
            print(f"== f{v[1]} {EV.get(v[0], v[0])} paused {v[4]} tick {v[5]} curtime {v[7]:.4f} lastAnim {v[13]} T {v[17]:.3f} P {v[18]:.3f}")
    elif tag == 'O':
        r = Rd(mm, p); oid, kind, flags, addr, vt, size, cls, root, depth = r.u('<IBBQQIIBB'); lab = r.s()
        objs[oid] = dict(label=lab, kind=kind, addr=addr, mem=bytearray(size))
    elif tag == 'S':
        oid = struct.unpack_from('<I', mm, p)[0]
        if oid in objs: objs[oid]['mem'][:] = mm[p + 4:p + n]
    elif tag == 'M':
        r = Rd(mm, p); wall, fr = r.u('<dI'); txt = r.s()
        if f0 <= fr <= f1 and not txt.startswith('CENSUS'): print('   MARK', txt[:160])
    elif tag == 'D':
        oid, cnt = struct.unpack_from('<II', mm, p)
        o = objs.get(oid)
        if not o: continue
        pairs = struct.unpack_from(f'<{2 * cnt}I', mm, p + 8)
        m = o['mem']
        show = lastE is not None and f0 <= lastE[1] <= f1 and filt in o['label'].lower()
        lines = []
        for k in range(0, 2 * cnt, 2):
            off, new = pairs[k], pairs[k + 1]
            if off + 4 > len(m): continue
            old = struct.unpack_from('<I', m, off)[0]
            struct.pack_into('<I', m, off, new)
            if show: lines.append(f'+0x{off:04x} {old:#010x}->{new:#010x} i {struct.unpack("<i", struct.pack("<I", old))[0]}->{struct.unpack("<i", struct.pack("<I", new))[0]} f {fl(old)}->{fl(new)}')
        if show:
            if len(lines) <= MAXW:
                print(f"   #{oid} k{o['kind']} {o['label'][:70]} ({len(lines)} words)")
                for l in lines: print('      ' + l)
            else:
                print(f"   #{oid} k{o['kind']} {o['label'][:70]} ({len(lines)} words, summarized)")
