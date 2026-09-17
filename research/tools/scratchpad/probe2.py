import os, sys
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT
from collections import Counter

path = r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\bin\logs\insight_20260917_004521_bodies.bin'
ins = Insight(path)
mm = ins.mm
print(f'format={ins.format} version={ins.version!r} label={ins.label!r} size={os.path.getsize(path)/2**20:.1f}MB')

kinds = Counter()
for tag, p, n in records(mm, ins.start):
    kinds[tag] += 1
print('\n-- record type counts --')
for t, c in sorted(kinds.items(), key=lambda x: -x[1]):
    print(f'  {t}: {c}')

# tick transitions + markers
prev = None; ticks = []
for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        v = EFMT.unpack_from(mm, p)
        key = (v[5], v[4], v[3])
        if key != prev:
            ticks.append((v[1], v[5], v[4], v[3])); prev = key
print('\n-- demo tick transitions --')
for frame, tick, paused, playing in ticks[:40]:
    st = 'PAUSED' if paused else 'play' if playing else 'stop'
    print(f'  f{frame:<6} tick {tick:<7} {st}')

print('\n-- markers --')
for tag, p, n in records(mm, ins.start):
    if tag == 'M':
        r = Rd(mm, p); wall, frame = r.u('<dI'); text = r.s()
        print(f'  f{frame:<6} {text[:100]}')
