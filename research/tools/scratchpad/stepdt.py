import sys, struct, collections
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, EFMT, EV
ins = Insight(sys.argv[1]); mm = ins.mm
lo, hi = int(sys.argv[2]), int(sys.argv[3])
cnt = collections.Counter()
for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        v = EFMT.unpack_from(mm, p)
        if lo <= v[1] <= hi and v[0] in (3, 4):
            cnt[(v[0], v[4], v[14], round(v[15], 5), v[16], round(v[17],3), round(v[18],3))] += 1
for k, c in sorted(cnt.items()): print(c, 'ev', k[0], 'paused', k[1], 'mode', k[2], 'dt', k[3], 'sub', k[4], 'T', k[5], 'P', k[6]) if c > 0 else None
