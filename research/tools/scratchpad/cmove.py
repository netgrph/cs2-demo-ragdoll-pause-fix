import sys, struct
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, EFMT, EV
ins = Insight(sys.argv[1]); mm = ins.mm
last = None; lastprint = -99
for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        v = EFMT.unpack_from(mm, p)
        if v[0] != 6: continue
        c = v[8:11]
        if not v[11]: continue
        if last is None or max(abs(a-b) for a, b in zip(c, last)) > 0.3:
            print(f"f{v[1]} {'P' if v[4] else 'p'} tick{v[5]} C=({c[0]:.1f},{c[1]:.1f},{c[2]:.1f}) steps{v[12]} lastAnim{v[13]}")
            last = c
    elif tag == 'M':
        r = struct.unpack_from('<dIH', mm, p)
        t = bytes(mm[p+14:p+14+r[2]]).decode('utf-8','replace')
        if t.startswith(('PAUSE','UNPAUSE','JUMP','TARGET','CATCHUP','SETTLE','USER','STOP','START')) or 'settle' in t.lower(): print(f"f{r[1]} MARK {t[:200]}")
