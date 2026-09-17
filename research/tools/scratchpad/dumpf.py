"""Annotated function dump. usage: dumpf.py <dll> <rva_hex>[:maxbytes] ...
Annotates RIP-relative operands (imports, strings, data RVAs) and direct call targets."""
import sys, os, re, struct
sys.path.insert(0, os.path.dirname(__file__))
from s2re import Img

RIP = re.compile(r'rip ([+-]) (0x[0-9a-f]+)')

def build_imports(img):
    m = {}
    for d in getattr(img.pe, 'DIRECTORY_ENTRY_IMPORT', []):
        for i in d.imports:
            m[i.address - img.base] = f"{d.dll.decode()}!{i.name.decode() if i.name else i.ordinal}"
    return m

def cstr(img, rva):
    d = img.data
    if not (0 <= rva < len(d)): return None
    e = d.find(b'\0', rva, rva + 200)
    if e <= rva: return None
    s = d[rva:e]
    if len(s) >= 3 and all(32 <= c < 127 for c in s):
        return s.decode()
    return None

def annotate(img, imps, ins):
    rva = ins.address - img.base
    notes = []
    m = RIP.search(ins.op_str)
    if m:
        disp = int(m.group(2), 16) * (1 if m.group(1) == '+' else -1)
        t = rva + ins.size + disp
        if t in imps: notes.append(imps[t])
        else:
            s = cstr(img, t)
            if s: notes.append(f'"{s}"')
            else:
                # pointer to string?
                if 0 <= t < len(img.data) - 8:
                    p = struct.unpack_from('<Q', img.data, t)[0] - img.base
                    s2 = cstr(img, p) if 0 < p < len(img.data) else None
                    notes.append(f'data {t:#x}' + (f' -> "{s2}"' if s2 else ''))
    if ins.mnemonic in ('call', 'jmp') and ins.op_str.startswith('0x'):
        t = int(ins.op_str, 16) - img.base
        notes.append(f'sub_{t:x}')
    return ('   ; ' + ' | '.join(notes)) if notes else ''

def main():
    img = Img(sys.argv[1])
    imps = build_imports(img)
    for spec in sys.argv[2:]:
        linear = spec.startswith('@')
        parts = spec.lstrip('@').split(':')
        rva = int(parts[0], 16)
        mx = int(parts[1], 16) if len(parts) > 1 else 0x3000
        f = None if linear else img.func_of(rva)
        start, end = f if f else (rva, rva + (mx if linear else 0x200))
        if f is None: print(f'(no pdata for {rva:#x})')
        print(f'\n======== func {start:#x}-{end:#x} (size {end-start:#x})')
        for ins in img.disasm(start, min(end - start, mx)):
            a = ins.address - img.base
            print(f'  {a:#010x}  {ins.mnemonic:7s} {ins.op_str}{annotate(img, imps, ins)}')

if __name__ == '__main__':
    main()
