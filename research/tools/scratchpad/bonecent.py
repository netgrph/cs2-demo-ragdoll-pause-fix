"""bonecent.py FILE lo hi bonesId [nbones] - bone centroid of a census bones object after every event."""
import sys, struct
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT, EV
ins = Insight(sys.argv[1]); mm = ins.mm
lo, hi, bid = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
nb = int(sys.argv[5]) if len(sys.argv) > 5 else 60
mem = None; last = None
def cent():
    xs = ys = zs = 0.0; k = 0
    for i in range(nb):
        x, y, z = struct.unpack_from('<3f', mem, i * 32)
        if abs(x) > 1e5 or abs(y) > 1e5 or abs(z) > 1e5 or (x == 0 and y == 0 and z == 0): continue
        xs += x; ys += y; zs += z; k += 1
    return (xs / k, ys / k, zs / k) if k else (0, 0, 0)
evname = {1:'anim>',2:'anim<',3:'step>',4:'step<',5:'frame>',6:'frame<'}
for tag, p, n in records(mm, ins.start):
    if tag == 'O':
        oid = struct.unpack_from('<I', mm, p)[0]
        if oid == bid:
            size = struct.unpack_from('<I', mm, p + 4 + 1 + 1 + 8 + 8)[0]; mem = bytearray(size)
    elif tag == 'S' and mem is not None and struct.unpack_from('<I', mm, p)[0] == bid:
        mem[:] = mm[p + 4:p + n]
    elif tag == 'D' and mem is not None:
        oid, cnt = struct.unpack_from('<II', mm, p)
        if oid != bid: continue
        pairs = struct.unpack_from(f'<{2 * cnt}I', mm, p + 8)
        for k in range(0, 2 * cnt, 2): struct.pack_into('<I', mem, pairs[k], pairs[k + 1])
    elif tag == 'E':
        v = EFMT.unpack_from(mm, p)
        if v[1] > hi: break
        if v[1] >= lo and mem is not None:
            c = cent()
            print(f"f{v[1]} {evname.get(v[0], v[0]):6} {'P' if v[4] else 'p'} tick{v[5]} lastAnim{v[13]} steps{v[12]} bones=({c[0]:.1f},{c[1]:.1f},{c[2]:.1f})")
    elif tag == 'M':
        r = Rd(mm, p); wall, frame = r.u('<dI'); t = r.s()
        if lo <= frame <= hi and t.startswith(('PAUSE', 'UNPAUSE', 'CATCHUP', 'JUMP')): print(f'  == f{frame} {t[:120]}')
