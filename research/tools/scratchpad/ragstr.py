import pefile, re, struct, sys
G = r"C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game"
for path in (G + "/csgo/bin/win64/client.dll", G + "/bin/win64/animationsystem.dll"):
    p = pefile.PE(path, fast_load=True)
    img = bytes(p.get_memory_mapped_image())
    t = [s for s in p.sections if s.Name.startswith(b'.text')][0]
    lo, hi = t.VirtualAddress, t.VirtualAddress + t.Misc_VirtualSize
    strs = {}
    for m in re.finditer(rb'[\x20-\x7e]{0,80}[Rr]agdoll[\x20-\x7e]{0,80}\x00', img):
        if lo <= m.start() < hi: continue
        strs[m.start()] = m.group(0)[:-1].decode()
    # rip-relative refs from .text
    code = img[lo:hi]
    refs = {a: [] for a in strs}
    for mm in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', code):
        a = lo + mm.start(); tgt = a + 7 + struct.unpack_from('<i', code, mm.start() + 3)[0]
        if tgt in refs: refs[tgt].append(a)
    print('=====', path.split('/')[-1], len(strs))
    for a in sorted(strs):
        s = strs[a]
        if len(s) > 100: s = s[:100]
        print('0x%x refs %-40s %s' % (a, ','.join(hex(r) for r in refs[a][:4]), s))
