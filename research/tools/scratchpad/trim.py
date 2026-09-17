import re, sys
for fn in sys.argv[1:]:
    print('=====', fn.split('\\')[-1])
    for line in open(fn, encoding='utf-8', errors='replace'):
        if line.startswith('## f') or re.search(r'CPhysicsRagdoll|bone transforms|ragsys>|posectl>\+20>\+28>\+278', line):
            print(line.rstrip()[:900])
