import pefile
g = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game"
for p in [r"\csgo\bin\win64\client.dll", r"\bin\win64\engine2.dll", r"\bin\win64\vphysics2.dll", r"\bin\win64\schemasystem.dll"]:
    pe = pefile.PE(g + p, fast_load=True)
    print(p, hex(pe.FILE_HEADER.TimeDateStamp), hex(pe.OPTIONAL_HEADER.SizeOfImage))
