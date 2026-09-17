import csv, math
F=r"C:/.VSCODE/CS2_RagdollDemoFix/DemoClock/bin/logs/democlock_20260915_073455.csv"
rows=list(csv.reader(open(F,encoding='utf-8',errors='replace')))[1:]
MAIN=('gate','step','catchup','resync','start','passthrough','suspect_jump')
calls=[]
for i,r in enumerate(rows):
    a=r[14]
    if a in MAIN: calls.append(dict(i=i,p=int(r[3]),tick=int(r[4]),st=float(r[12]),steps=int(r[13]),a=a,settled=0,show=False))
    elif a=='settle': calls[-1]['settled']=int(r[13])
    elif a.startswith('show'): calls[-1]['show']=True
prev=-1; staleCall=-10**9; staleTick=-1; why=''; oldPause=False
for n,c in enumerate(calls):
    if c['a']=='suspect_jump': continue
    t=c['tick']; jumped= prev>=0 and (t<prev or t>prev+max(1,math.ceil(c['st']-1e-6)))
    w='settle' if c['settled'] else 'catchup' if c['steps']>1 else 'jump' if jumped and c['steps'] else None
    if w: staleCall=n; staleTick=t; why=w
    prev=t
    if c['p']:
        armed=c['settled'] or (n-staleCall<=16 and abs(t-staleTick)<=2)
        if armed:
            staleCall=-10**9
            tag='OLD-ALSO' if c['settled'] else 'NEW'
            print(f"row {c['i']:5d} tick {t} armed by {why if not c['settled'] else 'settle'} [{tag}]")
        if not oldPause: print(f"row {c['i']:5d} tick {t} pause edge{' (no arm)' if not armed else ''}")
    oldPause=bool(c['p'])
