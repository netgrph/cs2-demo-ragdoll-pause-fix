"""Batch string-xref with disassembly windows. usage: xrefs.py <dll> <before> <after> <str1> <str2> ..."""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from s2re import Img, fast_refs

def main():
    path, before, after = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
    img = Img(path)
    for s in sys.argv[4:]:
        hits = [h for h in img.find_all(s.encode() + b'\0') if h == 0 or img.data[h-1] == 0]
        print(f'\n##### "{s}" string RVAs {[hex(h) for h in hits]}')
        for h in hits:
            for r in fast_refs(img, h):
                f = img.func_of(r)
                if not f:
                    print(f'  ref @ {r:#x} (no pdata func)'); continue
                print(f'  --- ref @ {r:#x} in func {f[0]:#x}-{f[1]:#x} (size {f[1]-f[0]:#x})')
                for ins in img.disasm(f[0], f[1]-f[0]):
                    a = ins.address - img.base
                    if r - before <= a <= r + after:
                        mark = '   <==' if a <= r < a + ins.size else ''
                        print(f'    {a:#010x}  {ins.mnemonic:7s} {ins.op_str}{mark}')

main()
