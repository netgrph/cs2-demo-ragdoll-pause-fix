import os, sys, struct, math
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT

path = r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\bin\logs\insight_20260917_005251_bodies2.bin'
ins = Insight(path); mm = ins.mm
CX,CY,CZ = -2146.1, 809.2, -110.3
UP = 4043
def d3(a,b): return math.sqrt(sum((a[i]-b[i])**2 for i in range(3)))
def fin(t): return all(math.isfinite(v) for v in t)

# ---- collect pointer targets from the physics objects (from their first snapshot) ----
TARGET = {638:'CPhysicsRagdoll', 639:'Aggregate(pawn)', 648:'Aggregate(world)', 649:'CapsuleShape'}
snap = {}
for tag,p,n in records(mm, ins.start):
    if tag=='S':
        oid = struct.unpack_from('<I', mm, p)[0]
        if oid in TARGET and oid not in snap: snap[oid]=bytes(mm[p+4:p+n])
def looks_ptr(v): return 0x0000010000000000<=v<=0x0000480000000000 and (v&0xf)==0
targets=[]  # (owner_oid, off, ptr)
for oid,b in snap.items():
    for off in range(0,len(b)-8,8):
        v=struct.unpack_from('<Q',b,off)[0]
        if looks_ptr(v): targets.append((oid,off,v))
print(f'pointer targets from physics objects: {len(targets)}')

# ---- load finder candidates (addr) + timelines with paused flag ----
cur={'frame':0,'paused':0}; cand={}; tl={}
for tag,p,n in records(mm, ins.start):
    if tag=='E':
        v=EFMT.unpack_from(mm,p); cur={'frame':v[1],'paused':v[4],'playing':v[3]}
    elif tag=='Y':
        r=Rd(mm,p); rid,start,count=r.u('<III')
        for i in range(count):
            addr=r.u('<Q'); pat=r.u('<B'); xyz=r.u('<3f'); cand[start+i]={'addr':addr,'pat':pat}
    elif tag=='C':
        r=Rd(mm,p); rid=r.u('<I'); ev=r.u('<B'); frame=r.u('<I'); sc=r.u('<Q'); s=r.u('<I'); c=r.u('<I')
        for _ in range(c):
            idx=r.u('<I'); xyz=r.u('<3f'); tl.setdefault(idx,[]).append((frame,cur['paused'],xyz))
print(f'candidates={len(cand)} timelined={len(tl)}')

# ---- find candidates whose addr lies inside a pointed-to object [T, T+0x800) ----
addr2idx={}
for idx,c in cand.items(): addr2idx.setdefault(c['addr'],idx)
sorted_addrs=sorted(addr2idx)
import bisect
def cands_in(lo,hi):
    i=bisect.bisect_left(sorted_addrs,lo); out=[]
    while i<len(sorted_addrs) and sorted_addrs[i]<hi:
        out.append(addr2idx[sorted_addrs[i]]); i+=1
    return out

print('\n== finder candidates that live INSIDE memory pointed-to by the physics objects ==')
print('owner  off    target        cand_in_[T,T+0x400)  (idx: motion_paused / motion_post  first->last)')
found_any=False
seen=set()
for oid,off,T in sorted(targets, key=lambda t:t[2]):
    ins_c=cands_in(T, T+0x400)
    if not ins_c: continue
    for idx in ins_c:
        if idx in seen: continue
        seen.add(idx)
        seq=[(fr,pa,xyz) for (fr,pa,xyz) in tl.get(idx,[]) if fin(xyz)]
        if not seq: continue
        near=[s for s in seq if d3(s[2],(CX,CY,CZ))<160]
        if not near: continue
        base=near[0][2]
        mp=max((d3(s[2],base) for s in near if s[1]),default=0.0)          # while paused
        post=[s for s in seq if s[0]>UP and fin(s[2])]
        mq=max((d3(s[2],base) for s in post),default=0.0)                   # after unpause
        endnear = d3(near[-1][2],(CX,CY,CZ))<160
        if endnear:
            found_any=True
            f=base; l=near[-1][2]; a=cand[idx]['addr']
            print(f'  #{oid:3d} +0x{off:03x} {T:012x}  idx{idx:6d}@{a:012x} pat{cand[idx]["pat"]}  '
                  f'paused{mp:7.2f} post{mq:8.2f}  ({f[0]:.1f},{f[1]:.1f},{f[2]:.1f})->({l[0]:.1f},{l[1]:.1f},{l[2]:.1f})  n{len(seq)}')
if not found_any:
    print('  (none: no tracked candidate memory sits inside the physics objects\' pointer targets)')
