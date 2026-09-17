import sys, struct, pefile, numpy as np
# usage: learef.py <dll> <rva>...  -> rip-relative lea/mov sites (48/4C 8D|8B modrm disp32) referencing rva
pe = pefile.PE(sys.argv[1], fast_load=True)
img = bytes(pe.get_memory_mapped_image())
t = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
lo = t.VirtualAddress; hi = lo + t.Misc_VirtualSize
seg = np.frombuffer(img[lo:hi + 8], dtype=np.uint8)
n = hi - lo
rex = (seg[:n] == 0x48) | (seg[:n] == 0x4C)
op = (seg[1:n+1] == 0x8D) | (seg[1:n+1] == 0x8B)
mr = (seg[2:n+2] & 0xC7) == 0x05
idx = np.nonzero(rex & op & mr)[0]
d = seg[idx+3].astype(np.int64) | (seg[idx+4].astype(np.int64) << 8) | (seg[idx+5].astype(np.int64) << 16) | (seg[idx+6].astype(np.int64) << 24)
d = np.where(d >= 2**31, d - 2**32, d)
dst = lo + idx + 7 + d
for a in sys.argv[2:]:
    tg = int(a, 16)
    print(a, [hex(x) for x in (idx[dst == tg] + lo)])
