import re
root = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game"
files = {
 'client': root + r'\csgo\bin\win64\client.dll',
 'engine2': root + r'\bin\win64\engine2.dll',
 'vphysics2': root + r'\bin\win64\vphysics2.dll',
 'animationsystem': root + r'\bin\win64\animationsystem.dll',
}
pat = re.compile(rb'[\x20-\x7e]{5,}')
kw = re.compile(r'(?i)ragdoll|demo_?pause|IsDemoPaused|ispaused|cl_phys|phys_|physics_?time|sv_pause|demo_timescale|host_timescale|OnDemoPause|CPhysAggregate|PhysicsWorld|IPhys|PhysicsGameSystem|physicsbody|rnbody|m_ragPos|ragAngles|StepSimulation|Simulate\(|freezeframe')
for name, path in files.items():
    data = open(path,'rb').read()
    hits = sorted(set(m.group().decode() for m in pat.finditer(data) if kw.search(m.group().decode())))
    print(f'===== {name} ({len(hits)} hits)')
    for h in hits[:500]:
        print('  ', h[:170])
