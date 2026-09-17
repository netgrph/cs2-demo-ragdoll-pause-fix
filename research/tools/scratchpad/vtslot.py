"""Find vtables holding given function RVAs: prints slot index and RTTI class. usage: vtslot.py <dll> <rva_hex>..."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(__file__))
from s2re import Img
img = Img(sys.argv[1]); d = bytes(img.data); base = img.base
lo, hi = img.sec_range('.rdata')
def rtti(vt):
    try:
        col = struct.unpack_from('<Q', d, vt - 8)[0] - base
        if not (0 < col < len(d)) or struct.unpack_from('<I', d, col)[0] != 1: return None
        td = struct.unpack_from('<I', d, col + 12)[0]
        e = d.find(b'\0', td + 16, td + 400)
        return d[td + 16:e].decode('latin1')
    except Exception: return None
for a in sys.argv[2:]:
    f = int(a, 16); needle = struct.pack('<Q', base + f); i = lo; hits = 0
    while True:
        i = d.find(needle, i, hi)
        if i < 0: break
        # walk back to the vtable start (entry preceded by a COL pointer)
        j = i; name = None
        for k in range(0, 1200):
            name = rtti(j)
            if name: break
            j -= 8
        print(f"{a}: at .rdata {hex(i)} vt {hex(j)} slot {(i-j)//8} class {name}"); hits += 1; i += 8
        if hits > 12: break
    if not hits: print(f"{a}: no vtable refs")
