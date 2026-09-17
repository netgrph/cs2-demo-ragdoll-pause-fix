"""Disassemble functions (by any address inside them) and list refs to their starts.
usage: dis.py <dll> <addr>[:maxinsns] ...   (hex RVAs)"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from s2re import Img, fast_refs

img = Img(sys.argv[1])
for spec in sys.argv[2:]:
    a, _, n = spec.partition(':')
    a = int(a, 16)
    n = int(n) if n else 400
    f = img.func_of(a)
    start, end = (f[0], f[1]) if f else (a, a + 0x400)
    print(f"##### {a:#x} func {start:#x}-{end:#x}")
    refs = fast_refs(img, start)
    rs = []
    for r in refs[:12]:
        rf = img.func_of(r)
        rs.append(f"{r:#x}@{rf[0]:#x}" if rf else f"{r:#x}")
    print(f"  refs to start: {rs}")
    for x in img.disasm(start, min(end - start, 0x4000))[:n]:
        print(f"  {x.address - img.base:#010x}  {x.mnemonic:8} {x.op_str}")
