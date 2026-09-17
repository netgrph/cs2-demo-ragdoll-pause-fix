import pefile, struct, sys, collections
def load(path):
    p = pefile.PE(path, fast_load=True)
    img = bytearray(p.get_memory_mapped_image()); base = p.OPTIONAL_HEADER.ImageBase
    secs = {s.Name.rstrip(b'\0').decode(): (s.VirtualAddress, s.Misc_VirtualSize) for s in p.sections}
    return img, base, secs
def classes(path):
    img, base, secs = load(path)
    rd = secs['.rdata']; tx = secs['.text']
    u32 = lambda o: struct.unpack_from('<I', img, o)[0]
    u64 = lambda o: struct.unpack_from('<Q', img, o)[0]
    cols = {}
    for c in range(rd[0], rd[0]+rd[1]-24, 4):
        if u32(c) == 1 and u32(c+20) == c:
            cols[c] = (u32(c+4), u32(c+12), u32(c+16))
    def name(td):
        e = img.find(b'\0', td+16); return img[td+16:e].decode('latin1')
    def bases(chd):
        n = u32(chd+8); arr = u32(chd+12)
        return [name(u32(u32(arr+4*i))) for i in range(min(n, 200))]
    out = []
    for v in range(rd[0], rd[0]+rd[1]-8, 8):
        q = u64(v) - base
        if q in cols:
            off, td, chd = cols[q]
            vt = v + 8; n = 0
            while n < 4096:
                f = u64(vt + 8*n) - base
                if not (tx[0] <= f < tx[0]+tx[1]): break
                n += 1
            out.append((name(td), off, vt, n, bases(chd)))
    return out
if __name__ == '__main__':
    G = "C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game/"
    cl = classes(G + "csgo/bin/win64/client.dll")
    print('client vtables', len(cl), 'slots', sum(c[3] for c in cl))
    gs = [c for c in cl if '.?AVIGameSystem@@' in c[4]]
    print('IGameSystem-derived vtables', len(gs), 'slots', sum(c[3] for c in gs))
    for c in cl:
        if any(k in c[0] for k in ('C_CSPlayerPawn@', 'RagdollPoseControl', 'CAnimGraphGameSystem', 'CPhysicsGameSystem', 'Ragdoll')):
            print(' ', c[0], 'off', hex(c[1]), 'vt', hex(c[2]), 'slots', c[3], 'gs' if '.?AVIGameSystem@@' in c[4] else '')
    vp = classes(G + "bin/win64/vphysics2.dll")
    print('vphysics2 vtables', len(vp))
    for c in vp:
        if any(k in c[0] for k in ('CPhysicsBody@', 'CPhysAggregateInstance', 'CPhysicsRagdoll', 'AbsoluteRagdollControl@@', 'CPhysicsJoint@', 'ShadowController@', 'MotionController@', 'CVPhys2World@', 'CVPhysics2Interface@')):
            print(' ', c[0], 'off', hex(c[1]), 'vt', hex(c[2]), 'slots', c[3])
