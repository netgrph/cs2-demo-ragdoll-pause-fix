import os, sys, struct
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT
from collections import Counter

path = r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\bin\logs\insight_20260917_013655_pinbodies.bin'
ins = Insight(path); mm = ins.mm
print(f'format={ins.format} version={ins.version!r} label={ins.label!r} size={os.path.getsize(path)/2**20:.1f}MB')

kinds = Counter()
for tag, p, n in records(mm, ins.start):
    kinds[tag] += 1
print('record counts:', dict(sorted(kinds.items(), key=lambda x:-x[1])))

# demo tick transitions
prev=None; trans=[]
for tag,p,n in records(mm, ins.start):
    if tag=='E':
        v=EFMT.unpack_from(mm,p)
        key=(v[5],v[4],v[3])
        if key!=prev: trans.append((v[1],v[5],v[4],v[3])); prev=key
print('\n-- demo tick transitions (frame, tick, paused, playing) --')
for fr,tick,pa,pl in trans[:60]:
    st='PAUSED' if pa else 'play' if pl else 'stop'
    print(f'  f{fr:<6} tick {tick:<7} {st}')

# markers (esp PIN / TARGET / CENSUS)
print('\n-- markers (PIN / TARGET / first CENSUS) --')
shown=0
for tag,p,n in records(mm, ins.start):
    if tag=='M':
        r=Rd(mm,p); wall,fr=r.u('<dI'); txt=r.s()
        if txt.startswith('PIN') or txt.startswith('TARGET') or ('CENSUS' in txt and shown<3):
            print(f'  f{fr:<6} {txt[:160]}')
            if 'CENSUS' in txt: shown+=1

# tracked objects: list PHYSBODY / PHYSXFORM O records
print('\n-- pinned physics objects (O records with PHYSBODY/PHYSXFORM label) --')
labels={}
for tag,p,n in records(mm, ins.start):
    if tag=='O':
        r=Rd(mm,p); oid,kind,flags,addr,vt,size,cls,root,depth=r.u('<IBBQQIIBB'); lab=r.s()
        if lab.startswith('PHYSBODY') or lab.startswith('PHYSXFORM'):
            labels[oid]=(addr,size,cls,lab)
for oid in sorted(labels):
    addr,size,cls,lab=labels[oid]
    print(f'  #{oid:4d} @{addr:012x} size 0x{size:<4x} cls{cls:<4d} {lab[:110]}')
print(f'\ntotal pinned physics objects: {len(labels)}')
