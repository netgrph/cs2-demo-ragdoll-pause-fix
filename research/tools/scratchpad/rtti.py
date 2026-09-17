"""List RTTI type names in a DLL matching a regex, with their vtable RVAs.

usage: rtti.py <dll> <regex> [slots]
"""
import re, struct, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from s2re import Img

img = Img(sys.argv[1])
rx = re.compile(sys.argv[2].encode())
slots = int(sys.argv[3]) if len(sys.argv) > 3 else 0
data = bytes(img.data)
for m in re.finditer(rb"\.\?AV[A-Za-z0-9_@?$]+@@\x00", data):
    name = m.group()[:-1]
    if not rx.search(name):
        continue
    td = m.start() - 16
    vts = []
    for col in img.find_all(struct.pack("<I", td), ".rdata"):
        colrva = col - 12
        sig, off = struct.unpack_from("<II", data, colrva)
        if sig != 1:
            continue
        for p in img.find_all(struct.pack("<Q", img.base + colrva), ".rdata"):
            vts.append((p + 8, off))
    print(name.decode(), [f"{v:#x}(off {o:#x})" for v, o in vts])
    for v, o in vts[:1]:
        for i in range(slots):
            va = struct.unpack_from("<Q", data, v + 8 * i)[0]
            if not 0 < va - img.base < len(data):
                break
            print(f"   [{i:3d}] {va - img.base:#x}")
