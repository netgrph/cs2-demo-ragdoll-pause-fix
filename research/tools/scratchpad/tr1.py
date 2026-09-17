import csv,collections,re,sys
f=r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\bin\logs\democlock_20260915_183143.csv'
rows=list(csv.DictReader(open(f,encoding='utf-8',errors='replace')))
print(len(rows))
c=collections.Counter()
for r in rows:
    a=r['action']; k=re.sub(r'[-\d\.]+','#',a)[:60]; c[k]+=1
for k,v in c.most_common(60): print(v,k)
