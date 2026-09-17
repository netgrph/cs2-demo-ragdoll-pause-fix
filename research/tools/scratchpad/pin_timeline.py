import os, sys, struct, math
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT

path = r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\bin\logs\insight_20260917_013655_pinbodies.bin'
ins = Insight(path); mm = ins.mm
UNPAUSE = 2407

# watch these (oid, offset) position words to see per-frame trajectory across the unpause
WATCH = {
  7:  [0xac4,0xac8,0xacc],   # aggregate pawn>+30>+2a0 position
  27: [0x10c,0x110,0x114],   # raw transform pawn>+218>+340>+2c8 (fell 48u in phase view)
  29: None,                  # CPhysicsBody pawn>+260>+330>+1b8 - auto-pick its changed offsets
  8:  None,                  # CPhysicsBody pawn>+218>+340 - had 0 play changes (should be flat)
  15: [0x1c0,0x1c4,0x1c8],   # CRnHullShape that moved 38u
}

# object memory (from S) so we can seed values; then log every change of watched offsets with frame
mem={}; size={}; lab={}
for tag,p,n in records(mm, ins.start):
    if tag=='O':
        r=Rd(mm,p); oid,kind,flags,addr,vt,sz,cls,root,depth=r.u('<IBBQQIIBB'); l=r.s()
        if oid in WATCH: size[oid]=sz; lab[oid]=l
    elif tag=='S':
        oid=struct.unpack_from('<I',mm,p)[0]
        if oid in WATCH and oid not in mem: mem[oid]=bytearray(mm[p+4:p+n])

# auto-pick changed offsets for oids with None: first scan all D to find which offsets move during play
changed=dict((oid,set()) for oid in WATCH)
cur=0
for tag,p,n in records(mm, ins.start):
    if tag=='E': cur=EFMT.unpack_from(mm,p)[1]
    elif tag=='D':
        oid,cnt=struct.unpack_from('<II',mm,p)
        if oid not in WATCH: continue
        pairs=struct.unpack_from(f'<{2*cnt}I',mm,p+8)
        for k in range(0,2*cnt,2): changed[oid].add(pairs[k])
for oid in WATCH:
    if WATCH[oid] is None:
        offs=sorted(changed[oid])[:6]
        WATCH[oid]=offs

def f32(b,o):
    if o+4>len(b): return None
    return struct.unpack_from('<f',b,o)[0]

# now replay and log per-frame last value of each watched offset in window
frame=0; playing=0; paused=0
log=dict((oid,[]) for oid in WATCH)   # (frame, tuple of vals)
last=dict((oid,None) for oid in WATCH)
def snapshot(oid):
    return tuple(f32(mem[oid],o) for o in WATCH[oid])
for tag,p,n in records(mm, ins.start):
    if tag=='E':
        v=EFMT.unpack_from(mm,p)
        # before moving to new frame, record prev frame's final values if in window
        if 2380<=frame<=2520:
            for oid in WATCH:
                s=snapshot(oid)
                if s!=last[oid]:
                    log[oid].append((frame,paused,s)); last[oid]=s
        frame,paused,playing=v[1],v[4],v[3]
    elif tag=='D':
        oid,cnt=struct.unpack_from('<II',mm,p)
        if oid not in WATCH: continue
        pairs=struct.unpack_from(f'<{2*cnt}I',mm,p+8)
        m=mem[oid]
        for k in range(0,2*cnt,2):
            off,new=pairs[k],pairs[k+1]
            if off+4<=len(m): struct.pack_into('<I',m,off,new)

for oid in WATCH:
    print(f'\n== #{oid} {lab.get(oid,"?")[:60]}  offsets {[hex(o) for o in WATCH[oid]]} ==')
    print('  frame   st     ' + '   '.join(f'+0x{o:03x}' for o in WATCH[oid]))
    for fr,pa,vals in log[oid]:
        st='PAUSE' if pa else 'play'
        vs='  '.join(f'{v:11.3f}' if v is not None else '     None' for v in vals)
        mark=' <== UNPAUSE' if fr==UNPAUSE else ''
        print(f'  f{fr:<6} {st:<5} {vs}{mark}')
