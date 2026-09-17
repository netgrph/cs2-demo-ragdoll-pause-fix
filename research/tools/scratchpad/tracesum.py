"""Summarize a RagdollDemoClock trace CSV: pause transitions, tick jumps, resyncs and settle rows (with neighbours).

usage: tracesum.py <csv>
"""
import csv, sys

rows = list(csv.DictReader(open(sys.argv[1], newline="")))
print("rows", len(rows))
keep = set()
for i, r in enumerate(rows):
    p = rows[i - 1] if i else None
    if r["action"] not in ("gate", "step", "catchup") or (p and (p["paused"] != r["paused"] or abs(int(r["tick"]) - int(p["tick"])) > 1)):
        keep.update(range(max(0, i - 2), min(len(rows), i + 3)))
last = -2
for i in sorted(keep):
    if i != last + 1:
        print("  ...")
    r = rows[i]
    print(f"{i:5} {float(r['wall']):10.3f} pl{r['playing']} pa{r['paused']} tick {r['tick']} frac {r['frac']} P {r['P_before']}->{r['P_after']} "
          f"dt {r['dt']} phys {r['cl_phys_timescale']} st {r['step_ticks']} x{r['steps']} {r['action']}")
    last = i
