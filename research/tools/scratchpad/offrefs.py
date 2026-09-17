"""Find instructions in a DLL whose memory operand displacement equals given offsets.
usage: offrefs.py <dll> <hexoff>...  (prints insn, function start)"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from s2re import Img

img = Img(sys.argv[1])
lo, hi = img.sec_range('.text')
d = bytes(img.data)
for a in sys.argv[2:]:
    off = int(a, 16)
    needle = off.to_bytes(4, 'little')
    out = []
    i = lo
    while True:
        i = d.find(needle, i, hi)
        if i < 0:
            break
        for back in (2, 3, 4, 5, 6):
            s = i - back
            ins = img.disasm(s, 16)
            if not ins:
                continue
            x = ins[0]
            if x.address - img.base + x.size >= i + 4 and hex(off) in x.op_str and '[' in x.op_str:
                f = img.func_of(s)
                out.append((s, x, f[0] if f else -1))
                break
        i += 1
    print(f"=== {a}: {len(out)} refs")
    for s, x, f in out:
        print(f"  {s:#x} {x.mnemonic} {x.op_str}  func {f:#x}")
