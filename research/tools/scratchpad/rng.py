import pefile, capstone, struct, sys
p = pefile.PE(r"C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game/csgo/bin/win64/client.dll", fast_load=True)
img = p.get_memory_mapped_image(); base = p.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for arg in sys.argv[1:]:
    a, b = [int(x, 16) for x in arg.split('-')]
    print('#### 0x%x-0x%x' % (a, b))
    for ins in md.disasm(img[a:b], a):
        print('  0x%08x  %-8s %s' % (ins.address, ins.mnemonic, ins.op_str))
vt = 0x19d1b48
print('#### CPhysicsGameSystem vtable')
for i in range(20, 60):
    v = struct.unpack_from('<Q', img, vt + 8*i)[0] - base
    print(' slot %2d -> 0x%x %s' % (i, v, img[v:v+10].hex()))
