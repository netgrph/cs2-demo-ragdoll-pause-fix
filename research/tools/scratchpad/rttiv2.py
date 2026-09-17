import pefile, re, struct
p = pefile.PE(r"C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game/csgo/bin/win64/client.dll", fast_load=True)
img = bytes(p.get_memory_mapped_image()); base = p.OPTIONAL_HEADER.ImageBase
def vtables(name):
    td = img.find(name.encode() + b'\x00') - 0x10
    out = []
    for m in re.finditer(re.escape(struct.pack('<I', td)), img):
        col = m.start() - 0xc
        sig, off = struct.unpack_from('<II', img, col)
        if sig != 1: continue
        for mm in re.finditer(re.escape(struct.pack('<Q', base + col)), img):
            out.append((off, mm.start() + 8))
    return out
for nm in ('.?AVCRagdollPoseControlSystem@@', '.?AVCRagdollGameSystem@@', '.?AVCRagdollManager@@', '.?AVC_ClientRagdoll@@'):
    for off, vt in vtables(nm):
        print('== %s col-off %d vtable 0x%x' % (nm, off, vt))
        if 'System' not in nm: continue
        for i in range(0, 64):
            v = struct.unpack_from('<Q', img, vt + 8*i)[0] - base
            if not (0x1000 < v < 0x1900000): break
            b = img[v:v+6]
            if b[:3] == b'\xc2\x00\x00' or b[:1] == b'\xc3': continue
            print('  slot %2d -> 0x%x %s' % (i, v, img[v:v+16].hex()))
