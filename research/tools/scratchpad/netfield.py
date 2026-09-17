"""Print the static schema metadata (MNetworkEnable etc.) of client.dll fields, to see which ones a demo networks.

usage: netfield.py field [field ...]
Field records are {name*, type*, int32 offset, int32 metadata count, metadata*}; metadata entries are {name*, data*}.
"""
import struct, sys
import numpy as np
import pefile

g = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game"
pe = pefile.PE(g + r"\csgo\bin\win64\client.dll", fast_load=True)
img = pe.get_memory_mapped_image()
base = pe.OPTIONAL_HEADER.ImageBase
q = np.frombuffer(img[: len(img) // 8 * 8], dtype="<u8")


def cstr(va, cap=96):
    rva = va - base
    if not 0 <= rva < len(img):
        return None
    end = img.find(b"\0", rva, rva + cap)
    return img[rva:end].decode(errors="replace") if end > rva else None


for field in sys.argv[1:]:
    start = 0
    while (rva := img.find(field.encode() + b"\0", start)) >= 0:
        start = rva + 1
        if rva and img[rva - 1] != 0:
            continue
        for idx in np.nonzero(q == base + rva)[0]:
            rec = int(idx) * 8
            off, n = struct.unpack_from("<iI", img, rec + 0x10)
            meta = struct.unpack_from("<Q", img, rec + 0x18)[0]
            if not (0 <= off < 0x10000 and n < 64):
                continue
            names = []
            for k in range(n):
                mrva = meta - base + k * 0x10
                if 0 <= mrva < len(img) - 16:
                    names.append(cstr(struct.unpack_from("<Q", img, mrva)[0]) or "?")
            tname = cstr(struct.unpack_from("<Q", img, struct.unpack_from("<Q", img, rec + 8)[0] - base + 8)[0]) \
                if 0 <= struct.unpack_from("<Q", img, rec + 8)[0] - base < len(img) - 16 else "?"
            print(f"{field} @rec {rec:#x}: offset {off:#x} type {tname} metadata {names}")
