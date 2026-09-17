import sys, struct
from collections import defaultdict
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT, EV
path = r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\bin\logs\insight_20260917_180257_recovery.bin'
ins = Insight(path); mm = ins.mm
objs = {}
order = []
chg = defaultdict(lambda: defaultdict(int))
lastE = None
seq = 0
def win(f): return 1303 <= f <= 1310 or 2713 <= f <= 2718
for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        v = EFMT.unpack_from(mm, p)
        seq += 1
        lastE = (seq, v[1], EV.get(v[0], v[0]), v[13])
        if win(v[1]): order.append(lastE)
    elif tag == 'O':
        r = Rd(mm, p); oid, kind, flags, addr, vt, size, cls, root, depth = r.u('<IBBQQIIBB'); lab = r.s()
        objs[oid] = (kind, lab)
    elif tag == 'D' and lastE and win(lastE[1]):
        oid, cnt = struct.unpack_from('<II', mm, p)
        chg[lastE][oid] += cnt
keys = [86, 88, 126, 160, 165, 197]
for e in order:
    c = chg.get(e, {})
    bones = {o: k for o, k in c.items() if objs.get(o, (0, ''))[0] == 2}
    tot = sum(c.values())
    print(f'f{e[1]:<5} {e[2]:<7} lastAnim {e[3]:<6} total {tot:<6} bones {bones}  key ' + ' '.join(f'#{k}:{c.get(k, 0)}' for k in keys))
print()
for k in keys: print(k, objs.get(k))
