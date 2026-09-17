import csv
f=r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\bin\logs\democlock_20260915_183143.csv'
rows=list(csv.DictReader(open(f,encoding='utf-8',errors='replace')))
out=open('tr2.txt','w',encoding='utf-8')
prev=None
for i,r in enumerate(rows):
    a=r['action']
    if a.startswith('animtick') : continue
    if a=='gate': 
        continue
    out.write(f"{i} {r['wall']} p{r['playing']}{r['paused']} t{r['tick']} f{r['frac']} T{r['T']} Pb{r['P_before']} Pa{r['P_after']} st{r['steps']} dt{r['dt']} | {a}\n")
