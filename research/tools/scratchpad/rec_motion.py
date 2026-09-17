import os, sys, struct, math
from collections import defaultdict
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT

path = r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\bin\logs\insight_20260917_180257_recovery.bin'
ins = Insight(path); mm = ins.mm
SEEK, UNPAUSE = 1304, 2714

# full markers
print('== all markers ==')
for tag,p,n in records(mm, ins.start):
    if tag=='M':
        r=Rd(mm,p); wall,fr=r.u('<dI'); print(f'  f{fr:<6} {r.s()[:260]}')

objs={}
frame=0; ev=None
# per frame: last event summary
fr_ev={}
# bones: per frame snapshot of all bone xyz (only when changed)
bone_hist=[]   # (frame, paused, tick, list of (x,y,z))
def bones_of(o):
    m=o['mem']; out=[]
    for i in range(len(m)//32):
        x,y,z,s=struct.unpack_from('<ffff',m,i*32); out.append((x,y,z))
    return out
chg_frames=defaultdict(lambda: defaultdict(int))  # oid -> frame -> words
last_bone_frame=None
for tag,p,n in records(mm, ins.start):
    if tag=='E':
        v=EFMT.unpack_from(mm,p)
        nf=v[1]
        if nf!=frame:
            # frame boundary: snapshot bones objects
            for oid,o in objs.items():
                if o['kind']==2 and o.get('dirty'):
                    bone_hist.append((frame,oid,ev[4] if ev else 0,ev[5] if ev else 0,bones_of(o))); o['dirty']=False
        frame=nf; ev=v
        fr_ev[frame]=dict(tick=v[5],paused=v[4],playing=v[3],c=v[8:11],haveC=v[11],steps=v[12],lastAnim=v[13],mode=v[14],dt=v[15],sub=v[16],T=v[17],P=v[18],idx=v[19],curtime=v[7])
    elif tag=='O':
        r=Rd(mm,p); oid,kind,flags,addr,vt,size,cls,root,depth=r.u('<IBBQQIIBB'); lab=r.s()
        objs[oid]=dict(kind=kind,addr=addr,size=size,label=lab,mem=bytearray(size),dirty=True)
    elif tag=='S':
        oid=struct.unpack_from('<I',mm,p)[0]
        if oid in objs: objs[oid]['mem'][:]=mm[p+4:p+n]; objs[oid]['dirty']=True
    elif tag=='D':
        oid,cnt=struct.unpack_from('<II',mm,p)
        o=objs.get(oid)
        if not o: continue
        pairs=struct.unpack_from(f'<{2*cnt}I',mm,p+8)
        for k in range(0,2*cnt,2):
            off,new=pairs[k],pairs[k+1]
            if off+4<=len(o['mem']): struct.pack_into('<I',o['mem'],off,new)
        o['dirty']=True
        chg_frames[oid][frame]+=cnt

print('\n== bones objects ==')
for oid,o in objs.items():
    if o['kind']==2: print(f'  #{oid} @{o["addr"]:x} size 0x{o["size"]:x} ({o["size"]//32} bones) {o["label"][:80]}')

print('\n== centroid / steps / mode per frame (printed on change), f1290..f3000 ==')
prev=None
for f in sorted(fr_ev):
    if not (1290<=f<=3000): continue
    e=fr_ev[f]
    key=(tuple(round(x,2) for x in e['c']) if e['haveC'] else None, e['paused'], e['mode'], e['idx'])
    st=('PAUSE' if e['paused'] else 'play ')
    if key!=prev or f in (SEEK,UNPAUSE) or (f-UNPAUSE) in range(0,12):
        c=' '.join(f'{x:9.2f}' for x in e['c']) if e['haveC'] else '-'
        print(f'  f{f:<5} {st} tick {e["tick"]:<6} steps {e["steps"]:<7} dt {e["dt"]:.4f} sub {e["sub"]} T {e["T"]:.2f} P {e["P"]:.2f} mode {e["mode"]} tgt {e["idx"]} C {c}')
        prev=key

# bone trajectory: pick the bones object for the target (largest history)
cnts=defaultdict(int)
for f,oid,pa,tick,b in bone_hist: cnts[oid]+=1
print('\n== bone snapshots per bones-object:', dict(cnts))
for boid in cnts:
    hist=[h for h in bone_hist if h[1]==boid]
    print(f'\n== bones #{boid}: per-change summary (mean z, min z, max z, max per-bone move vs prev snapshot, bone0) ==')
    prevb=None
    for f,oid,pa,tick,b in hist:
        pts=[q for q in b if all(math.isfinite(x) for x in q) and abs(q[0])<1e5 and abs(q[2])<1e5 and (q[0]!=0 or q[1]!=0 or q[2]!=0)]
        if not pts: continue
        mz=sum(q[2] for q in pts)/len(pts)
        mv=0.0; mvi=-1
        if prevb:
            for i,(q,r) in enumerate(zip(b,prevb)):
                d=math.dist(q,r) if all(map(math.isfinite,q+r)) else 0
                if d>mv and d<1e4: mv=d; mvi=i
        st='PAUSE' if pa else 'play '
        if not (1280<=f<=3100): prevb=b; continue
        print(f'  f{f:<5} {st} tick {tick:<6} n{len(pts):<3} meanZ {mz:9.2f} minZ {min(q[2] for q in pts):9.2f} maxZ {max(q[2] for q in pts):9.2f}  maxMove {mv:8.2f} (bone {mvi})  b0 ({b[0][0]:.1f},{b[0][1]:.1f},{b[0][2]:.1f})'
              + ('  <== SEEK' if f==SEEK else '  <== UNPAUSE' if f==UNPAUSE else ''))
        prevb=b

# pinned physics objects: words changed per window
W=[('pre-seek',0,SEEK),('seek+60',SEEK,SEEK+60),('pause-rest',SEEK+60,UNPAUSE),('unpause+60',UNPAUSE,UNPAUSE+60),('play-rest',UNPAUSE+60,10**9)]
print('\n== pinned physics objects: words changed per window ==')
print('  oid   ' + ''.join(f'{w[0]:>12}' for w in W) + '   label')
for oid,o in objs.items():
    if not (o['label'].startswith('PHYS') and 'pawn' in o['label']): continue
    row=[sum(c for f,c in chg_frames[oid].items() if lo<=f<hi) for _,lo,hi in W]
    print(f'  #{oid:<4}' + ''.join(f'{x:>12}' for x in row) + f'   {o["label"][:70]}')
