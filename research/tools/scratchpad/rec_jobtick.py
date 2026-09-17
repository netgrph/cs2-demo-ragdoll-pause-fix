import sys, struct
from collections import defaultdict
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT, EV
path = r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\bin\logs\insight_20260917_180257_recovery.bin'
ins = Insight(path); mm = ins.mm
# Checkpoints (state at the given event): look for int32 words that track the anim job tick:
#   pre-seek paused (f1300): job tick should be 16696? (15200 + 1496)
#   post-seek paused (f2713 anim<): 16696 expected (controller last)
#   first unpaused anim< (f2715): 16746
#   f2718 anim<: 16750
CHK = {('f1300', 1300, 'frame<'), ('f2713a', 2713, 'anim>'), ('f2715b', 2715, 'anim>'), ('f2715', 2715, 'anim<'), ('f2717', 2717, 'anim<'), ('f2718', 2718, 'anim<')}
want = {c[0]: None for c in CHK}
objs = {}
snaps = {}
for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        v = EFMT.unpack_from(mm, p)
        name = EV.get(v[0], v[0])
        for key, f, ev in CHK:
            if v[1] == f and name == ev and key not in snaps:
                # state BEFORE this event's D records = state after the previous event's D; approximate by snapshot at the E record
                snaps[key] = {oid: (o['label'], o['addr'], bytes(o['mem'])) for oid, o in objs.items() if o['alive']}
        if v[1] > 2720: break
    elif tag == 'O':
        r = Rd(mm, p); oid, kind, flags, addr, vt, size, cls, root, depth = r.u('<IBBQQIIBB'); lab = r.s()
        objs[oid] = dict(label=lab, addr=addr, mem=bytearray(size), alive=True)
    elif tag == 'S':
        oid = struct.unpack_from('<I', mm, p)[0]
        if oid in objs: objs[oid]['mem'][:] = mm[p + 4:p + n]
    elif tag == 'X':
        oid = struct.unpack_from('<I', mm, p)[0]
        if oid in objs: objs[oid]['alive'] = False
    elif tag == 'D':
        oid, cnt = struct.unpack_from('<II', mm, p)
        o = objs.get(oid)
        if not o: continue
        pairs = struct.unpack_from(f'<{2 * cnt}I', mm, p + 8)
        m = o['mem']
        for k in range(0, 2 * cnt, 2):
            off, new = pairs[k], pairs[k + 1]
            if off + 4 <= len(m): struct.pack_into('<I', m, off, new)

print('snapshots:', sorted(snaps))
order = ['f1300', 'f2713a', 'f2715b', 'f2715', 'f2717', 'f2718']
# candidate words: value in 16600..16800 at f2713a, and changed by f2718
common = set(snaps['f2713a']) & set(snaps['f2718'])
hits = []
for oid in common:
    lab, addr, a = snaps['f2713a'][oid]
    b = snaps['f2718'][oid][2]
    for off in range(0, min(len(a), len(b)) - 3, 4):
        va = struct.unpack_from('<i', a, off)[0]
        vb = struct.unpack_from('<i', b, off)[0]
        if 16600 <= va <= 16800 and 16600 <= vb <= 16800 and va != vb:
            row = []
            for k in order:
                s = snaps.get(k, {}).get(oid)
                row.append(struct.unpack_from('<i', s[2], off)[0] if s and off + 4 <= len(s[2]) else None)
            hits.append((oid, off, lab, addr, row))
print(f'{"oid":>5} {"off":>6}  ' + ' '.join(f'{k:>7}' for k in order) + '  label')
for oid, off, lab, addr, row in sorted(hits):
    print(f'#{oid:<4} +0x{off:04x}  ' + ' '.join(f'{str(x):>7}' for x in row) + f'  @{addr + off:x} {lab[:90]}')
