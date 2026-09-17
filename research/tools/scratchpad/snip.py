"""usage: snip.py <file> <regex> [ctx] [max]  - print context around regex hits (text decoded)"""
import re, sys
p, rx = sys.argv[1], sys.argv[2]
ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 600
mx = int(sys.argv[4]) if len(sys.argv) > 4 else 8
t = open(p, encoding='utf-8', errors='replace').read()
t = t.replace('\\n', '\n').replace('\\"', '"').replace('\\t', '\t')
n = 0
last = -10**9
for m in re.finditer(rx, t):
    if m.start() - last < ctx:
        continue
    last = m.start()
    print('=' * 20, m.start())
    print(t[max(0, m.start() - ctx): m.end() + ctx])
    n += 1
    if n >= mx:
        break
