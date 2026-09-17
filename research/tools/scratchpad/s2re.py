"""Static RE helper for CS2 x64 DLLs.

Usage:
  re.py xref <dll> <string> [maxdis]   - find RIP-relative refs to an ASCII string, print owning function + disassembly
  re.py func <dll> <rva_hex> [count]   - disassemble function containing rva
  re.py exports <dll> [filter]         - list exports
  re.py vtable <dll> <.?AVClass@@>     - find vtable(s) via RTTI, print first N entries as RVAs
"""
import sys, struct, bisect, re
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

class Img:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=False)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = self.pe.get_memory_mapped_image()
        self.secs = {s.Name.rstrip(b'\0').decode(): s for s in self.pe.sections}
        # function table from .pdata
        self.funcs = []
        for e in getattr(self.pe, 'DIRECTORY_ENTRY_EXCEPTION', []):
            self.funcs.append((e.struct.BeginAddress, e.struct.EndAddress))
        self.funcs.sort()
        self.fstarts = [f[0] for f in self.funcs]
        self.md = Cs(CS_ARCH_X86, CS_MODE_64)

    def sec_range(self, name):
        s = self.secs[name]
        return s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize

    def func_of(self, rva):
        i = bisect.bisect_right(self.fstarts, rva) - 1
        if i >= 0 and self.funcs[i][0] <= rva < self.funcs[i][1]:
            # walk back over chained unwind fragments is non-trivial; return fragment
            return self.funcs[i]
        return None

    def find_all(self, needle, sec=None):
        lo, hi = (0, len(self.data)) if sec is None else self.sec_range(sec)
        out, i = [], lo
        while True:
            i = self.data.find(needle, i, hi)
            if i < 0: return out
            out.append(i); i += 1

    def riprel_refs(self, target_rva):
        """Scan .text for disp32 that resolves to target (any instruction length 5..10 where disp is at end-4 or earlier)."""
        lo, hi = self.sec_range('.text')
        d = self.data
        res = []
        # disp32 located at offset p; next instruction address = p+4+k where k=trailing imm bytes (0,1,4)
        for k in (0, 1, 4):
            # target = p + 4 + k + disp  => disp = target - p - 4 - k
            pass
        mv = memoryview(d)
        for p in range(lo, hi - 4):
            disp = struct.unpack_from('<i', d, p)[0]
            if p + 4 + disp == target_rva:
                res.append(p)
        return res

    def disasm(self, rva, size):
        code = bytes(self.data[rva:rva + size])
        return list(self.md.disasm(code, self.base + rva))

def strings_rva(img, s):
    return img.find_all(s.encode() + b'\0')

def cmd_xref(path, s, maxdis=0):
    img = Img(path)
    hits = strings_rva(img, s)
    print(f'string "{s}" at RVAs: {[hex(h) for h in hits]}')
    for h in hits:
        # exact start of string (ensure preceded by NUL)
        if h > 0 and img.data[h-1] != 0: continue
        refs = fast_refs(img, h)
        for r in refs:
            f = img.func_of(r)
            print(f'  ref @ {r:#x}  func {f[0]:#x}-{f[1]:#x}' if f else f'  ref @ {r:#x} (no func)')
            if maxdis and f:
                for ins in img.disasm(f[0], min(f[1]-f[0], maxdis)):
                    mark = '<==' if ins.address - img.base <= r < ins.address - img.base + ins.size else ''
                    print(f'    {ins.address - img.base:#010x}  {ins.mnemonic:8s} {ins.op_str} {mark}')

def fast_refs(img, target):
    import numpy as np
    lo, hi = img.sec_range('.text')
    buf = np.frombuffer(bytes(img.data[lo:hi]), dtype=np.uint8)
    n = len(buf) - 4
    disp = (buf[0:n].astype(np.int64) | (buf[1:n+1].astype(np.int64) << 8) | (buf[2:n+2].astype(np.int64) << 16) | (buf[3:n+3].astype(np.int64) << 24))
    disp = np.where(disp >= 2**31, disp - 2**32, disp)
    pos = np.arange(n, dtype=np.int64) + lo
    idx = np.nonzero(pos + 4 + disp == target)[0]
    return [int(lo + i) for i in idx]

def cmd_func(path, rva, count=200):
    img = Img(path)
    f = img.func_of(rva)
    start, end = (f if f else (rva, rva + 0x400))
    print(f'func {start:#x}-{end:#x}')
    for ins in img.disasm(start, min(end - start, 0x4000))[:count]:
        print(f'  {ins.address - img.base:#010x}  {ins.mnemonic:8s} {ins.op_str}')

def cmd_exports(path, filt=None):
    img = Img(path)
    for e in img.pe.DIRECTORY_ENTRY_EXPORT.symbols:
        n = e.name.decode() if e.name else f'#{e.ordinal}'
        if filt is None or re.search(filt, n, re.I):
            print(f'{e.address:#010x} {n}')

def cmd_vtable(path, tdname, count=60):
    img = Img(path)
    # RTTI type descriptor: vftable ptr (8) + spare (8) + name
    for h in img.find_all(tdname.encode() + b'\0'):
        td = h - 16
        # complete object locators reference td rva (signature=1 at +0, td rva at +12)
        for col in img.find_all(struct.pack('<I', td), '.rdata'):
            colrva = col - 12
            sig, off, cdoff, tdr = struct.unpack_from('<IIII', img.data, colrva)
            if sig != 1: continue
            # vtable meta pointer = absolute VA of COL
            colva = struct.pack('<Q', img.base + colrva)
            for m in img.find_all(colva, '.rdata'):
                vt = m + 8
                print(f'vtable {tdname} offset={off:#x} @ rva {vt:#x}')
                for i in range(count):
                    va = struct.unpack_from('<Q', img.data, vt + 8*i)[0]
                    rva = va - img.base
                    if not (0 < rva < len(img.data)): break
                    print(f'  [{i:3d}] {rva:#010x}')

if __name__ == '__main__':
    a = sys.argv[1:]
    if a[0] == 'xref': cmd_xref(a[1], a[2], int(a[3]) if len(a) > 3 else 0)
    elif a[0] == 'func': cmd_func(a[1], int(a[2], 16), int(a[3]) if len(a) > 3 else 200)
    elif a[0] == 'exports': cmd_exports(a[1], a[2] if len(a) > 2 else None)
    elif a[0] == 'vtable': cmd_vtable(a[1], a[2], int(a[3]) if len(a) > 3 else 60)
