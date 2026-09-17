import sys, struct
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, EFMT, EV
ins = Insight(sys.argv[1]); mm = ins.mm
lo, hi = int(sys.argv[2]), int(sys.argv[3])
last = None
for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        v = EFMT.unpack_from(mm, p)
        if lo <= v[1] <= hi:
            print(f"f{v[1]} {EV.get(v[0])} play{v[3]} pause{v[4]} tick{v[5]} frac{v[6]:.3f} cur{v[7]:.4f} C=({v[8]:.1f},{v[9]:.1f},{v[10]:.1f}) haveC{v[11]} steps{v[12]} lastAnim{v[13]} mode{v[14]} dt{v[15]:.5f} sub{v[16]} T{v[17]:.4f} P{v[18]:.4f} idx{v[19]}")
    elif tag == 'M':
        r = struct.unpack_from('<dIH', mm, p)
        if lo <= r[1] <= hi:
            t = bytes(mm[p+14:p+14+r[2]]).decode('utf-8','replace')
            if not t.startswith(('CENSUS','WATCH page')): print(f"f{r[1]} MARK {t}")
