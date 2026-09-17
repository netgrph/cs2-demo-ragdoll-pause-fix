from census import classes
import struct
G = "C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game/"
vp = classes(G + "bin/win64/vphysics2.dll")
print('vphysics2 classes:', sorted(set(c[0] for c in vp if 'lambda' not in c[0] and '_Func' not in c[0])))
cl = classes(G + "csgo/bin/win64/client.dll")
for c in cl:
    if c[0] == '.?AVC_CSPlayerPawn@@' and c[1] == 0: print('pawn bases', c[4])
data = open(G + "bin/win64/vphysics2.dll", 'rb').read()
for nm, v in (('0.0254', struct.pack('<f', 0.0254)), ('39.37', struct.pack('<f', 39.3701)), ('1/39.37', struct.pack('<f', 1/39.3701))):
    print(nm, data.count(v))
