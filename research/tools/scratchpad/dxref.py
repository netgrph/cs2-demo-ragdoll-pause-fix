"""Data/code xrefs. usage: dxref.py <dll> <rva_hex>...
For each target: RIP-relative refs in .text (with instruction + func), absolute 64-bit pointers in .rdata/.data, and direct call/jmp rel32 refs."""
import sys, os, struct
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from s2re import Img, fast_refs
from dumpf import annotate, build_imports

def ins_at(img, ref_rva):
    # try instruction starts 2..8 bytes before disp
    for back in range(1, 10):
        st = ref_rva - back
        ins = list(img.md.disasm(bytes(img.data[st:st + 16]), img.base + st, 1))
        if ins and st + ins[0].size > ref_rva + 3 and 'rip' in ins[0].op_str or (ins and ins[0].mnemonic in ('call', 'jmp') and st + ins[0].size == ref_rva + 4):
            return ins[0]
    return None

def main():
    img = Img(sys.argv[1])
    imps = build_imports(img)
    for t in sys.argv[2:]:
        tr = int(t, 16)
        print(f'\n##### target {tr:#x}')
        refs = []
        for off in range(0, 1):
            refs += fast_refs(img, tr)
        # also refs into target+8 etc for struct members handled by caller
        for r in refs:
            ins = ins_at(img, r)
            f = img.func_of(r)
            fs = f'{f[0]:#x}' if f else '?'
            if ins:
                print(f'  code {ins.address-img.base:#010x} func {fs}: {ins.mnemonic} {ins.op_str}{annotate(img, imps, ins)}')
            else:
                print(f'  disp @ {r:#x} func {fs}')
        va = struct.pack('<Q', img.base + tr)
        for sec in ('.rdata', '.data'):
            if sec in img.secs:
                for p in img.find_all(va, sec):
                    print(f'  ptr in {sec} @ {p:#x}')

if __name__ == '__main__':
    main()
