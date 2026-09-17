import sys, struct, os
from collections import defaultdict
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT, EV
# Per frame: paused flag, target lastAnimTick before/after the anim event, and word changes by event for bones (kind 2)
# and physics body objects (label contains PHYSBODY or kind 3). Also every M marker. One row per frame with activity.
path = sys.argv[1]
ins = Insight(path); mm = ins.mm
objs = {}
rows = {}      # frame -> dict
order = []
lastE = None
marks = []
def row(f):
    if f not in rows:
        rows[f] = dict(paused=None, tick=None, la=[], ev=defaultdict(lambda: [0, 0]), steps=None, tgt=None)
        order.append(f)
    return rows[f]
for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        v = EFMT.unpack_from(mm, p)
        name = EV.get(v[0], v[0])
        lastE = (v[1], name)
        r = row(v[1])
        r['paused'] = v[4]; r['tick'] = v[5]; r['steps'] = v[12]; r['tgt'] = v[19]
        r['la'].append((name, v[13]))
    elif tag == 'O':
        rd = Rd(mm, p); oid, kind, flags, addr, vt, size, cls, root, depth = rd.u('<IBBQQIIBB'); lab = rd.s()
        objs[oid] = (kind, lab)
    elif tag == 'M':
        rd = Rd(mm, p); t, fr = rd.u('<dI'); txt = rd.s()
        marks.append((fr, txt))
    elif tag == 'D' and lastE:
        oid, cnt = struct.unpack_from('<II', mm, p)
        kind, lab = objs.get(oid, (0, ''))
        c = rows[lastE[0]]['ev'][lastE[1]]
        if kind == 2: c[0] += cnt
        elif 'PHYSBODY' in lab: c[1] += cnt
mk = defaultdict(list)
for fr, txt in marks: mk[fr].append(txt)
kinds = defaultdict(int)
for oid, (k, lab) in objs.items(): kinds[k] += 1
print(os.path.basename(path), 'objects by kind', dict(kinds))
print('bone objects:', [(o, l[:60]) for o, (k, l) in objs.items() if k == 2][:12])
print('physbody objects:', len([1 for o, (k, l) in objs.items() if 'PHYSBODY' in l]))
prevP = None; prevLa = None
for f in order:
    r = rows[f]
    la = [x[1] for x in r['la']]
    evs = ' '.join(f"{e}{b}/{pb}" for e, (b, pb) in r['ev'].items() if b or pb)
    laChg = prevLa is not None and la and la[-1] != prevLa
    interesting = evs or r['paused'] != prevP or laChg or f in mk
    if interesting:
        anim = [x for x in r['la'] if x[0] in ('anim>', 'anim<')]
        a = f"anim {anim[0][1]}->{anim[-1][1]}" if anim else 'noanim'
        print(f"f{f:<6} p{r['paused']} tick {r['tick']} tgt {r['tgt']} {a:<20} bones/bodies {evs}" + (' | ' + ' || '.join(mk[f]) if f in mk else ''))
    prevP = r['paused']
    if la: prevLa = la[-1]
