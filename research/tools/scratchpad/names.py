import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from s2re import Img, fast_refs
from dumpf import annotate, build_imports
g = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game"
img = Img(g + r"\csgo\bin\win64\client.dll")
imps = build_imports(img)
targets = [int(x, 16) for x in sys.argv[1:]]
for t in targets:
    refs = fast_refs(img, t)
    print(f"\n##### target {t:#x}: {len(refs)} refs")
    for r in refs[:40]:
        f = img.func_of(r)
        if not f: print(f"  ref {r:#x} nofunc"); continue
        ins_all = img.disasm(f[0], min(f[1]-f[0], 0x4000))
        strs = []
        this = None
        for ins in ins_all:
            a = ins.address - img.base
            n = annotate(img, imps, ins)
            if a <= r < a + ins.size: this = f"{ins.mnemonic} {ins.op_str}{n}"
            if '"' in n and abs(a - r) < 0x60: strs.append(n.strip(' ;'))
        print(f"  ref {r:#x} in {f[0]:#x}-{f[1]:#x} (sz {f[1]-f[0]:#x}): {this}")
        for s_ in strs[:4]: print(f"       {s_}")
