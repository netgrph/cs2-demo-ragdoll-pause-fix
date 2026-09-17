import os, sys, struct, math
from collections import defaultdict
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT

path = r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\bin\logs\insight_20260917_180257_recovery.bin'
ins = Insight(path); mm = ins.mm
SEEK, UNPAUSE = 1304, 2714
CX, CY, CZ = -2141.9, 780.2, -85.5
R = 220.0

def near(x, y, z):
    return abs(x-CX) < R and abs(y-CY) < R and abs(z-CZ) < R and not (x == 0 and y == 0 and z == 0)

objs = {}
frame = 0
steps_by_frame = {}
# checkpoints: snapshot memory of all objects at these frames (state at START of frame i.e. before its D records)
CHECK = [SEEK, SEEK+2, SEEK+10, SEEK+60, SEEK+300, UNPAUSE, UNPAUSE+1, UNPAUSE+2, UNPAUSE+5, UNPAUSE+30, UNPAUSE+300]
snaps = {}
xyz_offs = defaultdict(set)   # oid -> set of offsets that ever changed and form near-corpse xyz
chg = defaultdict(lambda: defaultdict(int))
for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        v = EFMT.unpack_from(mm, p)
        nf = v[1]
        if nf != frame:
            for c in CHECK:
                if frame < c <= nf:
                    snaps[c] = {oid: bytes(o['mem']) for oid, o in objs.items() if o['alive']}
        frame = nf
        steps_by_frame[frame] = (v[12], v[4], v[5])
    elif tag == 'O':
        r = Rd(mm, p); oid, kind, flags, addr, vt, size, cls, root, depth = r.u('<IBBQQIIBB'); lab = r.s()
        objs[oid] = dict(kind=kind, addr=addr, size=size, cls=cls, label=lab, mem=bytearray(size), alive=True, born=frame)
    elif tag == 'S':
        oid = struct.unpack_from('<I', mm, p)[0]
        if oid in objs: objs[oid]['mem'][:] = mm[p+4:p+n]
    elif tag == 'X':
        oid = struct.unpack_from('<I', mm, p)[0]
        if oid in objs: objs[oid]['alive'] = False
    elif tag == 'D':
        oid, cnt = struct.unpack_from('<II', mm, p)
        o = objs.get(oid)
        if not o: continue
        pairs = struct.unpack_from(f'<{2*cnt}I', mm, p+8)
        m = o['mem']
        for k in range(0, 2*cnt, 2):
            off, new = pairs[k], pairs[k+1]
            if off+4 <= len(m): struct.pack_into('<I', m, off, new)
            chg[oid][off] += 1
        if SEEK <= frame < UNPAUSE + 400:
            for k in range(0, 2*cnt, 2):
                off = pairs[k]
                for base in (off, off-4, off-8):
                    if base >= 0 and base+12 <= len(m):
                        x, y, z = struct.unpack_from('<fff', m, base)
                        if all(map(math.isfinite, (x, y, z))) and near(x, y, z):
                            xyz_offs[oid].add(base)

print('== physics steps (E.steps) changes, f1290..2740 ==')
prev = None
for f in sorted(steps_by_frame):
    if 1290 <= f <= 2740:
        s = steps_by_frame[f]
        if prev is None or s[0] != prev[0] or s[1] != prev[1]:
            print(f'  f{f:<5} steps {s[0]:<5} paused {s[1]} tick {s[2]}')
        prev = s

# for each object with near-corpse xyz words: print value at checkpoints (first 6 xyz per object)
print('\n== near-corpse xyz words: value at checkpoints ==')
hdr = '  ' + ''.join(f'{("f"+str(c)):>26}' for c in CHECK)
rows = 0
for oid in sorted(xyz_offs):
    o = objs[oid]
    offs = sorted(xyz_offs[oid])
    # collapse overlapping (keep offsets 12 apart)
    keep = []
    for b in offs:
        if not keep or b - keep[-1] >= 12: keep.append(b)
    print(f'\n#{oid} kind{o["kind"]} {o["label"][:90]}  ({len(keep)} xyz)')
    for b in keep[:8]:
        line = f'   +0x{b:04x}'
        vals = []
        for c in CHECK:
            sm = snaps.get(c, {}).get(oid)
            if sm is None or b+12 > len(sm): vals.append('               -'); continue
            x, y, z = struct.unpack_from('<fff', sm, b)
            vals.append(f'{x:8.1f},{y:6.1f},{z:7.1f}')
        print(line + '  ' + ' | '.join(vals))
        rows += 1
print('\ncheckpoints:', CHECK)
