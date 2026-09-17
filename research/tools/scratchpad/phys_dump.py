import os, sys, struct, math
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT

path = r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\bin\logs\insight_20260917_005251_bodies2.bin'
ins = Insight(path); mm = ins.mm
CX,CY,CZ = -2146.1, 809.2, -110.3

# target physics objects (oid -> label)
TARGET = {638:'CPhysicsRagdoll pawn+10c0', 639:'CPhysAggregateInstance pawn>+30>+2a0',
          648:'CPhysAggregateInstance world>+50>+328>+260>+220', 649:'CRnCapsuleShape'}

# object meta (O) and first snapshot (S)
meta = {}
snap = {}
for tag, p, n in records(mm, ins.start):
    if tag == 'O':
        r = Rd(mm, p); oid, kind, flags, addr, vt, size = r.u('<IBBQQI')
        if oid in TARGET: meta[oid] = dict(addr=addr, size=size, vt=vt)
    elif tag == 'S':
        oid = struct.unpack_from('<I', mm, p)[0]
        if oid in TARGET and oid not in snap:
            snap[oid] = bytes(mm[p+4:p+n])

def looks_ptr(v):
    return 0x0000010000000000 <= v <= 0x0000480000000000 and (v & 0xf) == 0

for oid, name in TARGET.items():
    if oid not in snap:
        print(f'\n#{oid} {name}: NO snapshot'); continue
    b = snap[oid]; m = meta.get(oid, {})
    print(f'\n===== #{oid} {name}  addr {m.get("addr",0):012x} size 0x{len(b):x} vt {m.get("vt",0):012x} =====')
    # 1) centroid-near float triples at any 4-byte offset
    hits = []
    for off in range(0, len(b)-12, 4):
        x,y,z = struct.unpack_from('<fff', b, off)
        if all(math.isfinite(t) for t in (x,y,z)):
            d = math.sqrt((x-CX)**2+(y-CY)**2+(z-CZ)**2)
            if d < 200:
                hits.append((off, x, y, z, d))
    print(f'  centroid-near xyz triples (<200u): {len(hits)}')
    for off,x,y,z,d in hits[:40]:
        print(f'    +0x{off:03x}  ({x:9.2f},{y:9.2f},{z:9.2f})  d{d:6.1f}')
    # 2) pointer fields (8-byte aligned)
    ptrs = []
    for off in range(0, len(b)-8, 8):
        v = struct.unpack_from('<Q', b, off)[0]
        if looks_ptr(v): ptrs.append((off, v))
    print(f'  pointer-like qwords: {len(ptrs)}')
    for off,v in ptrs[:24]:
        print(f'    +0x{off:03x}  -> {v:012x}')
