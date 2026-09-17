import sys, struct
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, EFMT, EV
ins = Insight(r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\bin\logs\insight_20260917_180257_recovery.bin'); mm = ins.mm
from collections import Counter
per = Counter(); prev=None
for tag,p,n in records(mm, ins.start):
    if tag!='E': continue
    v=EFMT.unpack_from(mm,p); f=v[1]
    if 1300<=f<=1330 or 2710<=f<=2720:
        print(f'f{f:<5} {EV.get(v[0],v[0]):8} tick {v[5]} paused {v[4]} steps {v[12]} lastAnim {v[13]} mode {v[14]} T {v[17]:.2f} P {v[18]:.2f} idx {v[19]} curtime {v[7]:.3f}')
    if 1310<f<2714: per[(EV.get(v[0],v[0]), v[13])]+=1
print('\nevent types / lastAnim during pause f1311..2713:', dict(per))
