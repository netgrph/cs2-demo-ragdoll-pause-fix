import os, sys, struct, math
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT

path = r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\bin\logs\insight_20260917_013655_pinbodies.bin'
ins = Insight(path); mm = ins.mm
UNPAUSE = 2407
CX,CY,CZ = -214.0, None, None  # target centroid printed truncated; recompute from bones later if needed

# ---- gather pinned physics objects (O + first S) ----
phys = {}   # oid -> dict(mem, size, cls, label, kind)
for tag,p,n in records(mm, ins.start):
    if tag=='O':
        r=Rd(mm,p); oid,kind,flags,addr,vt,size,cls,root,depth=r.u('<IBBQQIIBB'); lab=r.s()
        if lab.startswith('PHYSBODY') or lab.startswith('PHYSXFORM'):
            phys[oid]=dict(size=size,cls=cls,label=lab,kind=kind,addr=addr,mem=None)
    elif tag=='S':
        oid=struct.unpack_from('<I',mm,p)[0]
        if oid in phys and phys[oid]['mem'] is None:
            phys[oid]['mem']=bytearray(mm[p+4:p+n])

for oid in list(phys):
    if phys[oid]['mem'] is None: del phys[oid]
print(f'pinned phys objects with snapshot: {len(phys)}')

# ---- replay D records in order, tracking phase; capture end-of-pause and during-play snapshots ----
cur=dict(frame=0,paused=0,playing=0)
snap_pause={}; snap_play={}
paused_words=dict((oid,0) for oid in phys)   # count of word-changes while paused
play_words=dict((oid,0) for oid in phys)
paused_offs=dict((oid,set()) for oid in phys)
play_offs=dict((oid,set()) for oid in phys)

for tag,p,n in records(mm, ins.start):
    if tag=='E':
        v=EFMT.unpack_from(mm,p); cur=dict(frame=v[1],paused=v[4],playing=v[3])
        f=cur['frame']
        if f>=UNPAUSE and not snap_pause:
            for oid in phys: snap_pause[oid]=bytes(phys[oid]['mem'])
        if f>=UNPAUSE+90 and not snap_play:
            for oid in phys: snap_play[oid]=bytes(phys[oid]['mem'])
    elif tag=='D':
        oid,cnt=struct.unpack_from('<II',mm,p)
        if oid not in phys: continue
        pairs=struct.unpack_from(f'<{2*cnt}I',mm,p+8)
        mem=phys[oid]['mem']
        paused = cur['frame'] < UNPAUSE
        for k in range(0,2*cnt,2):
            off,new=pairs[k],pairs[k+1]
            if off+4<=len(mem): struct.pack_into('<I',mem,off,new)
            if paused: paused_words[oid]+=1; paused_offs[oid].add(off)
            else:      play_words[oid]+=1;  play_offs[oid].add(off)
if not snap_play:
    for oid in phys: snap_play[oid]=bytes(phys[oid]['mem'])

# ---- summary: did bodies change during pause? ----
print('\n== per-object word-change counts: PAUSED window (f0..%d) vs PLAY window ==' % (UNPAUSE-1))
print('  oid  cls  paused_chg  paused_offs  play_chg  play_offs   label')
tot_p=tot_q=0
for oid in sorted(phys):
    o=phys[oid]
    print(f'  #{oid:3d} {o["cls"]:>4} {paused_words[oid]:>10} {len(paused_offs[oid]):>12} '
          f'{play_words[oid]:>9} {len(play_offs[oid]):>10}   {o["label"][:60]}')
    tot_p+=paused_words[oid]; tot_q+=play_words[oid]
print(f'  TOTAL paused word-changes={tot_p}   play word-changes={tot_q}')

# ---- for objects that DID move at unpause: decode changed offsets as floats (pause-end vs play) ----
def f32(b,o): return struct.unpack_from('<f',b,o)[0]
print('\n== changed transform words across the unpause (pause-end -> play), floats only ==')
for oid in sorted(phys):
    o=phys[oid]; a=snap_pause.get(oid); b=snap_play.get(oid)
    if not a or not b: continue
    rows=[]
    for off in range(0,min(len(a),len(b))-3,4):
        wa=a[off:off+4]; wb=b[off:off+4]
        if wa==wb: continue
        va=f32(a,off); vb=f32(b,off)
        # only show plausible float coords (finite, not huge, not tiny-int-noise)
        if all(math.isfinite(x) for x in (va,vb)) and (abs(va)<1e6 and abs(vb)<1e6) and abs(vb-va)>1e-4:
            rows.append((off,va,vb))
    if not rows: continue
    print(f'\n  #{oid} {o["label"][:70]}  ({len(rows)} float words changed pause->play)')
    for off,va,vb in rows[:24]:
        print(f'    +0x{off:03x}  {va:12.4f} -> {vb:12.4f}   d{vb-va:+.4f}')
