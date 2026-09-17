import pefile, struct, re
g = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game"
c = pefile.PE(g + r"\csgo\bin\win64\client.dll", fast_load=True).get_memory_mapped_image()
for r in [0x198f3c8, 0x198f3d4, 0x198f3d8, 0x198f3fc, 0x198f410]:
    print(f"client {r:#x}: f32={struct.unpack_from('<f', c, r)[0]!r} f64={struct.unpack_from('<d', c, r)[0]!r}")
e = pefile.PE(g + r"\bin\win64\engine2.dll", fast_load=True).get_memory_mapped_image()
for r in [0x586780, 0x5865ac]:
    print(f"engine2 {r:#x}: f32={struct.unpack_from('<f', e, r)[0]!r}")
names = sorted(set(m.group().decode() for m in re.finditer(rb'\.\?AV[A-Za-z0-9_@]*(Demo|Host|Pause|Frame|Timescale)[A-Za-z0-9_@]*@@', bytes(e))))
print("engine2 RTTI:", names)
names = sorted(set(m.group().decode() for m in re.finditer(rb'\.\?AV[A-Za-z0-9_@]*(Ragdoll|Physics|PhysGame)[A-Za-z0-9_@]*@@', bytes(c))))
print("client RTTI:", names)
