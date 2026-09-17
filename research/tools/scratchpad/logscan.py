import sys
from collections import Counter
path = sys.argv[1]
types = Counter()
ent_classes = Counter()
w_first = []
with open(path, "rb") as fh:
    for raw in fh:
        t = raw[:1]
        types[t] += 1
        if t == b"E" and types[t] % 50 == 1:
            p = raw.split(b",", 7)
            if len(p) > 6:
                ent_classes[(p[5], p[6])] += 1
        elif t == b"W" and len(w_first) < 5:
            w_first.append(raw.decode().rstrip())
print(types)
print("W sample:", *w_first, sep="\n  ")
print("entity classes (sampled 1/50):")
for k, v in ent_classes.most_common(40):
    print("  ", v, k)
