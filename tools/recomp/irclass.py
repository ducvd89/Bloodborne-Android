#!/usr/bin/env python3
"""Groups the census op histogram (out/recomp/ir_ops.csv) by IR.json category.
usage: irclass.py <IR.json> <ir_ops.csv>   (IR.json: FEXCore/Source/Interface/IR/IR.json)"""
import csv, json, re, sys
ops = json.load(open(sys.argv[1]))['Ops']
cat = {}
for c, defs in ops.items():
    for sig in defs:
        # "GPR:$Quotient, GPR:$Remainder = Div ..." or "GPR = Add ..." or "Fence ..."
        cat[re.match(r'(\w+)', sig.split(' = ')[-1]).group(1)] = c
count, funcs = {}, {}
for r in csv.DictReader(open(sys.argv[2])):
    count[r['op']] = int(r['count']); funcs[r['op']] = int(r['functions'])
tot = sum(count.values())
rows = {}
for op, c in cat.items():
    n = count.get(op, 0)
    t = rows.setdefault(c, [0, 0, 0]); t[0] += 1; t[1] += n > 0; t[2] += n
print(f"{'category':<14}{'ops':>5}{'used':>6}{'count':>13}{'%':>7}")
for c, (n, u, k) in sorted(rows.items(), key=lambda kv: -kv[1][2]):
    print(f"{c:<14}{n:>5}{u:>6}{k:>13}{100.0 * k / tot:>7.2f}")
unknown = sorted(set(count) - set(cat))
if unknown: print("not in IR.json (pseudo ops):", ", ".join(unknown))
