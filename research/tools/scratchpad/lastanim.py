import sys, struct
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, EFMT, EV
ins = Insight(sys.argv[1]); mm = ins.mm
prev = None; pp = None; ps = None
for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        v = EFMT.unpack_from(mm, p)
        la = v[13]; paused = v[4]
        if la != prev or paused != pp:
            print(f"f{v[1]} ev{v[0]} {'P' if paused else 'p'} tick{v[5]} lastAnim {prev}->{la} steps{v[12]} C=({v[8]:.1f},{v[9]:.1f},{v[10]:.1f})")
            prev = la; pp = paused
    elif tag == 'M':
        r = struct.unpack_from('<dIH', mm, p)
        t = bytes(mm[p+14:p+14+r[2]]).decode('utf-8','replace')
        if t.startswith(('PAUSE','UNPAUSE','JUMP','TARGET','CATCHUP','USER')): print(f"f{r[1]} MARK {t[:160]}")
