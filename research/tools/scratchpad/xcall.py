import sys, struct, pefile
# usage: xcall.py <dll> <rva>...  -> call/jmp rel32 sites targeting rva
pe = pefile.PE(sys.argv[1], fast_load=True)
img = bytes(pe.get_memory_mapped_image())
t = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
lo = t.VirtualAddress; hi = lo + t.Misc_VirtualSize
import numpy as np
a = np.frombuffer(img, dtype=np.uint8)
seg = a[lo:hi]
rel = np.frombuffer(img[lo+1:hi+1] + b'\0'*3, dtype=np.uint8)
for tgt in [int(x, 16) for x in sys.argv[2:]]:
    idx = np.nonzero((seg == 0xE8) | (seg == 0xE9))[0]
    idx = idx[idx + 5 <= len(seg)]
    r = seg[idx+1].astype(np.int64) | (seg[idx+2].astype(np.int64) << 8) | (seg[idx+3].astype(np.int64) << 16) | (seg[idx+4].astype(np.int64) << 24)
    r = np.where(r >= 2**31, r - 2**32, r)
    dst = lo + idx + 5 + r
    hits = idx[dst == tgt] + lo
    print(hex(tgt), [('%s %s' % (hex(h), 'call' if img[h] == 0xE8 else 'jmp')) for h in hits])
