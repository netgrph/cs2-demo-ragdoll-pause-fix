import os, sys, struct
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT, EV

path = r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\bin\logs\insight_20260916_181726_F_forensic.bin'
ins = Insight(path)
mm = ins.mm
print(f'format={ins.format} version={ins.version!r} label={ins.label!r} size={os.path.getsize(path)/2**20:.0f}MB')

# 1) tick over frames: find pause hold and unpause
prev = None
ticks = []  # (frame, tick, paused, playing)
for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        v = EFMT.unpack_from(mm, p)
        frame, ev, playing, paused, tick = v[1], v[0], v[3], v[4], v[5]
        key = (tick, paused, playing)
        if key != prev:
            ticks.append((frame, ev, tick, paused, playing))
            prev = key
print('\n-- demo tick transitions (frame: tick paused/playing) --')
for frame, ev, tick, paused, playing in ticks[:60]:
    st = 'PAUSED' if paused else 'play' if playing else 'stop'
    print(f'  f{frame:<6} tick {tick:<7} {st}')

# 2) markers
print('\n-- markers of interest --')
for tag, p, n in records(mm, ins.start):
    if tag == 'M':
        r = Rd(mm, p); wall, frame = r.u('<dI'); text = r.s()
        if any(k in text for k in ('PAUSE','UNPAUSE','JUMP','FORENSIC','rec','START','STOP')):
            print(f'  f{frame:<6} {text[:90]}')

# 3) W-record access nature on body pages (format 2 only)
if ins.format >= 2:
    from collections import Counter
    natures = Counter()
    for tag, p, n in records(mm, ins.start):
        if tag == 'W':
            # minimal: skip to access byte. Reuse forensic layout up to mem+tail start.
            r = Rd(mm, p)
            seq = r.u('<Q'); when = r.u('<d'); fr = r.u('<I'); tid = r.u('<I')
            slot = r.u('<B'); kind = r.u('<B'); addr = r.u('<Q'); rip = r.u('<Q')
            r.u('<16Q')  # gpr
            ns = r.u('<B'); [r.u('<Q') for _ in range(ns)]
            nd = r.u('<B'); [r.u('<QQQ')[0] if False else r.u('<IQQ') for _ in range(nd)]
            r.p += 32  # mem
            access = r.u('<B')
            natures[access] += 1
    print('\n-- W access nature counts (0 read,1 write,3 hw r/w,8 exec; |0x80 diff-had-writes) --')
    for a, c in natures.most_common():
        print(f'  access 0x{a:02x}: {c}')
else:
    print('\n(format 1: no per-hit access-nature field)')
