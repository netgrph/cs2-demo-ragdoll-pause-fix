import re, sys
for path in sys.argv[2:]:
    d = open(path,'rb').read()
    print('==', path.split('/')[-1])
    seen=set()
    for m in re.finditer(rb'[\x20-\x7e]{5,}', d):
        s = m.group().decode()
        if re.search(sys.argv[1], s, re.I) and s not in seen:
            seen.add(s); print(hex(m.start()), s[:150])
