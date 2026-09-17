"""List strings in a DLL matching a regex with their code-ref functions.

usage: strrefs.py <dll> <regex>
"""
import re, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from s2re import Img, fast_refs

img = Img(sys.argv[1])
rx = re.compile(sys.argv[2].encode())
data = bytes(img.data)
lo, hi = img.sec_range(".rdata")
seen = set()
for m in re.finditer(rb"[\x20-\x7e]{4,200}\x00", data[lo:hi]):
    s = m.group()[:-1]
    if not rx.search(s) or s in seen:
        continue
    seen.add(s)
    h = lo + m.start()
    refs = fast_refs(img, h)
    fs = []
    for r in refs[:6]:
        f = img.func_of(r)
        fs.append(f"{r:#x}@{f[0]:#x}" if f else f"{r:#x}")
    print(f"{h:#x} '{s.decode()}' refs {fs}")
