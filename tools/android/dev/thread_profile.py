"""Per-thread CPU profile of the running game (bb-probe) on an Android phone, over adb, no root.

Samples /proc/<pid>/task/*/stat (utime, stime, last CPU) through run-as, the cpufreq of each
policy and the battery current/voltage once a second, then prints per thread: CPU % of one core,
the share of samples on each core class, and the CPU time split over cores (from the 'processor'
field at each sample, weighted by the time used since the previous sample).

usage: python3 -I thread_profile.py SERIAL SECONDS OUT.json
"""
import json
import re
import subprocess
import sys
import time

serial, seconds, out_path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
PKG = 'com.ducvd89.bloodborne'


def sh(cmd, timeout=20):
    return subprocess.run(['adb', '-s', serial, 'shell', cmd], capture_output=True, text=True,
                          errors='replace', timeout=timeout).stdout


pid = sh('pidof bb-probe').strip().split()
if not pid:
    sys.exit('bb-probe is not running')
pid = pid[0]
ncpu = int(sh('ls -d /sys/devices/system/cpu/cpu[0-9]* | wc -l').strip())
caps = [int(sh(f'cat /sys/devices/system/cpu/cpu{c}/cpu_capacity').strip() or 0) for c in range(ncpu)]
policies = [p for p in sh('ls /sys/devices/system/cpu/cpufreq').split() if p.startswith('policy')]
ticks = int(sh('getconf CLK_TCK').strip() or 100)

# One device-side loop: each second print a marker, the time, the stats of every task, cpufreq
# and the battery. run-as (the game's uid) can read its /proc entries.
loop = (
    f"end=$(( $(cut -d. -f1 /proc/uptime) + {seconds} )); while [ $(cut -d. -f1 /proc/uptime) -lt $end ]; do "
    "echo \"@T $(cat /proc/uptime)\"; "
    f"cat /proc/{pid}/task/*/stat 2>/dev/null; "
    "echo \"F $(cat /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq)\"; "
    "echo \"B $(cat /sys/class/power_supply/battery/current_now) $(cat /sys/class/power_supply/battery/voltage_now)\"; "
    "sleep 1; done"
)
raw = subprocess.run(['adb', '-s', serial, 'shell', f"run-as {PKG} sh -c '{loop}'"],
                     capture_output=True, text=True, errors='replace', timeout=seconds + 60).stdout

samples, cur = [], None
for line in raw.splitlines():
    if line.startswith('@T '):
        cur = {'t': float(line.split()[1]), 'tasks': {}, 'freq': {}, 'bat': None}
        samples.append(cur)
    elif cur is None:
        continue
    elif line[:1].isdigit():
        m = re.match(r'(\d+) \((.*)\) (.*)', line)
        if not m:
            continue
        tid, name, rest = int(m.group(1)), m.group(2), m.group(3).split()
        # rest[0] is field 3 (state); utime=14, stime=15, processor=39 -> indices 11, 12, 36
        cur['tasks'][tid] = (name, int(rest[11]) + int(rest[12]), int(rest[36]))
    elif line.startswith('F '):
        for pol, f in zip(policies, line.split()[1:]):
            cur['freq'][pol] = int(f)
    elif line.startswith('B '):
        parts = line.split()
        if len(parts) == 3:
            cur['bat'] = (int(parts[1]), int(parts[2]))

threads = {}
for a, b in zip(samples, samples[1:]):
    dt = b['t'] - a['t']
    for tid, (name, cpu_b, proc) in b['tasks'].items():
        if tid not in a['tasks']:
            continue
        used = (cpu_b - a['tasks'][tid][1]) / ticks
        th = threads.setdefault(tid, {'name': name, 'cpu_s': 0.0, 'wall': 0.0, 'by_cpu': [0.0] * ncpu})
        th['cpu_s'] += used
        th['wall'] += dt
        th['by_cpu'][proc] += used

classes = sorted(set(caps))
label = {c: n for c, n in zip(classes, ['little', 'mid', 'prime', 'x'][:len(classes)])}
if len(classes) == 2:
    label = {classes[0]: 'perf', classes[1]: 'prime'}
rows = []
for tid, th in threads.items():
    if th['wall'] <= 0:
        continue
    pct = 100 * th['cpu_s'] / th['wall']
    split = {}
    for c, s in enumerate(th['by_cpu']):
        split[label[caps[c]]] = split.get(label[caps[c]], 0) + s
    total = sum(split.values()) or 1
    rows.append((pct, tid, th['name'], {k: round(100 * v / total) for k, v in split.items()}))
rows.sort(reverse=True)
total_pct = sum(r[0] for r in rows)
print(f'pid {pid}, {len(samples)} samples, cores {ncpu} caps {caps}')
print(f'process total: {total_pct:.0f}% of one core ({total_pct / ncpu:.0f}% of the phone)')
for pct, tid, name, split in rows[:30]:
    if pct < 1:
        break
    print(f'{pct:6.1f}%  {name:28s} {tid:6d}  ' + ' '.join(f'{k} {v}%' for k, v in split.items()))
freq = {}
for s in samples:
    for pol, f in s['freq'].items():
        freq.setdefault(pol, []).append(f)
print('average clocks: ' + ', '.join(f'{p} {sum(v) / len(v) / 1000:.0f} MHz' for p, v in sorted(freq.items())))
bats = [s['bat'] for s in samples if s['bat']]
if bats:
    # current_now is in uA (sign differs per device), voltage_now in uV
    watts = [abs(c) * v / 1e12 for c, v in bats]
    print(f'battery: {sum(watts) / len(watts):.2f} W average (current {sum(abs(c) for c, _ in bats) / len(bats) / 1000:.0f} mA)')
json.dump({'rows': rows, 'freq': freq, 'bat': bats, 'caps': caps}, open(out_path, 'w'))
