import sys, struct, re, pefile
# usage: rttivt.py <dll> <regex> <slot>  -> vtables whose RTTI name matches, with slot target
pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = bytes(pe.get_memory_mapped_image())
rx = re.compile(sys.argv[2].encode()); slot = int(sys.argv[3])
tds = {}
for m in re.finditer(rb'\.\?AV[^\0]{1,200}@@\0', img):
    if rx.search(m.group(0)):
        tds[m.start() - 0x10] = m.group(0)[:-1].decode()
rd = [s for s in pe.sections if s.Name.startswith(b'.rdata')][0]
lo = rd.VirtualAddress; hi = lo + rd.Misc_VirtualSize
out = []
for i in range(lo, hi - 0x18, 4):
    sig, off, cd, td, chd = struct.unpack_from('<IIIII', img, i)
    if sig == 1 and td in tds and struct.unpack_from('<I', img, i + 0x14)[0] == i:
        col = base + i
        needle = struct.pack('<Q', col)
        j = img.find(needle, lo, hi)
        while j != -1:
            vt = j + 8
            t = struct.unpack_from('<Q', img, vt + 8 * slot)[0] - base
            out.append((tds[td], off, hex(vt), hex(t)))
            j = img.find(needle, j + 8, hi)
for o in out: print(*o)
