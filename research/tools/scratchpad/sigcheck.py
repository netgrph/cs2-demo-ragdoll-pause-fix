import sys, pefile, re
pe = pefile.PE(sys.argv[1], fast_load=True)
img = bytes(pe.get_memory_mapped_image())
t = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
lo = t.VirtualAddress; hi = lo + t.Misc_VirtualSize
for a in sys.argv[2:]:
    rva, n = a.split(':'); rva = int(rva, 16); n = int(n, 16)
    b = img[rva:rva+n]
    print(hex(rva), ' '.join('%02X' % x for x in b))
    for m in (16, 24, 32, 40, 48):
        pat = img[rva:rva+m]
        cnt = 0; i = img.find(pat, lo, hi)
        while i != -1 and cnt < 5:
            cnt += 1; i = img.find(pat, i+1, hi)
        print('  prefix', m, 'matches', cnt)
