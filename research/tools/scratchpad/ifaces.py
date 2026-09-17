import pefile, re
root = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game"
pe = pefile.PE(root + r"\bin\win64\tier0.dll")
names = [e.name.decode() for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name]
print("tier0 exports matching:", [n for n in names if re.search(r'^(Msg|ConMsg|ConColorMsg|Warning|DevMsg|Plat_FloatTime|LoggingSystem_Log|Plat_GetGameDirectory|CreateInterface|ConDMsg)$', n)])
print("tier0 export count:", len(names))
pat = re.compile(rb'[A-Za-z_]+0\d\d\x00')
for dll in [r"\bin\win64\engine2.dll", r"\csgo\bin\win64\client.dll", r"\bin\win64\vphysics2.dll", r"\bin\win64\schemasystem.dll", r"\bin\win64\tier0.dll"]:
    d = open(root + dll, 'rb').read()
    ifs = sorted(set(m.group()[:-1].decode() for m in pat.finditer(d)))
    ifs = [i for i in ifs if re.search(r'(?i)engine|client|phys|schema|cvar|demo|game|entity|world', i)]
    print(dll, ifs)
