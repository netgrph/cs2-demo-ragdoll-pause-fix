import os, sys, math, struct
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT

path = r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\bin\logs\insight_20260917_005251_bodies2.bin'
ins = Insight(path); mm = ins.mm
print(f'format={ins.format} version={ins.version!r} label={ins.label!r} size={os.path.getsize(path)/2**20:.1f}MB')

# ---- pass over records; maintain demo state from E; collect Y/C/B ----
cur = {'frame':0,'ev':0,'paused':0,'playing':0,'tick':0}
cand = {}     # idx -> {'addr','pat','first':(x,y,z)}
tl = {}       # idx -> list of (frame, ev, paused, x,y,z)
finalB = {}   # idx -> dict
unpause_frame = None
markers = []

def finite3(t):
    return all(math.isfinite(v) for v in t)

for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        v = EFMT.unpack_from(mm, p)
        cur = {'frame':v[1],'ev':v[0],'paused':v[4],'playing':v[3],'tick':v[5]}
    elif tag == 'M':
        r = Rd(mm, p); wall, fr = r.u('<dI'); txt = r.s(); markers.append((fr, txt))
        if 'unpause frame' in txt:
            try: unpause_frame = int(txt.split('unpause frame')[1].split()[0])
            except: pass
    elif tag == 'Y':
        r = Rd(mm, p); rid, start, count = r.u('<III')
        for i in range(count):
            addr = r.u('<Q'); pat = r.u('<B'); xyz = r.u('<3f')
            cand[start+i] = {'addr':addr,'pat':pat,'first':xyz}
    elif tag == 'C':
        r = Rd(mm, p); rid = r.u('<I'); ev = r.u('<B'); frame = r.u('<I')
        sc = r.u('<Q'); start = r.u('<I'); count = r.u('<I')
        for _ in range(count):
            idx = r.u('<I'); xyz = r.u('<3f')
            tl.setdefault(idx, []).append((frame, ev, cur['paused'], xyz))
    elif tag == 'B':
        r = Rd(mm, p); rid, start, count = r.u('<III')
        for i in range(count):
            addr = r.u('<Q'); pat = r.u('<B'); first = r.u('<3f'); last = r.u('<3f')
            mask = r.u('<H'); chg = r.u('<I'); noisy = r.u('<B')
            finalB[start+i] = {'addr':addr,'pat':pat,'first':first,'last':last,
                               'mask':mask,'chg':chg,'noisy':noisy}

print(f'unpause_frame={unpause_frame}  candidates={len(cand)}  with-timeline={len(tl)}  B={len(finalB)}')

# centroid from the scan-start marker
cx,cy,cz = -2146.1, 809.2, -110.3
for fr,txt in markers:
    if 'scan start' in txt and 'centroid' in txt:
        try:
            seg = txt.split('centroid')[1]
            nums = [float(x) for x in seg.replace(',',' ').split()[:3]]
            cx,cy,cz = nums
        except: pass
print(f'centroid=({cx:.1f},{cy:.1f},{cz:.1f})  unpause_frame={unpause_frame}')

def dist(a,b): return math.sqrt(sum((a[i]-b[i])**2 for i in range(3)))

# ---- classify every candidate that has a timeline ----
real=[]; artifact=[]
UP = unpause_frame or 4043
for idx, seq in tl.items():
    c = cand.get(idx, {})
    # split into finite, near-centroid samples only
    good = [(fr,ev,pa,xyz) for (fr,ev,pa,xyz) in seq if finite3(xyz)]
    if not good:
        artifact.append((idx,'all-nan',0,0,0)); continue
    # position just before unpause and last position after
    pre = [g for g in good if g[0] <= UP]
    post = [g for g in good if g[0] > UP]
    # last finite sample near centroid?
    finite_near = [g for g in good if dist(g[3],(cx,cy,cz)) < 120.0]
    if not finite_near:
        artifact.append((idx,'never-near',len(good),0,0)); continue
    # motion during pause window (samples with paused flag set)
    paused_samps = [g for g in finite_near if g[2]]
    move_paused = 0.0
    if len(paused_samps) >= 2:
        move_paused = max(dist(paused_samps[k][3],paused_samps[0][3]) for k in range(len(paused_samps)))
    # motion after unpause
    move_post = 0.0
    if post:
        base = finite_near[0][3]
        pf = [g for g in post if finite3(g[3])]
        if pf:
            move_post = max(dist(g[3],base) for g in pf)
    lastxyz = finite_near[-1][3]
    end_ok = dist(lastxyz,(cx,cy,cz)) < 120.0
    if end_ok and finite3(lastxyz):
        real.append((idx, c.get('addr',0), move_paused, move_post,
                     finite_near[0][3], lastxyz, len(finite_near)))
    else:
        artifact.append((idx,'drift-away',len(good),move_paused,move_post))

print(f'\nREAL (finite, near-centroid at start AND end): {len(real)}')
print(f'ARTIFACT (nan / freed / drifted to 0 or far): {len(artifact)}')

real.sort(key=lambda r:r[0])
print('\n-- REAL candidates: idx  addr  move_while_paused  move_after_unpause  n  first->last --')
for idx,addr,mp,mq,f,l,n in real[:60]:
    print(f'  {idx:6d} {addr:012x}  paused{mp:8.2f}  post{mq:8.2f}  n{n:4d}  '
          f'({f[0]:.1f},{f[1]:.1f},{f[2]:.1f})->({l[0]:.1f},{l[1]:.1f},{l[2]:.1f})')
