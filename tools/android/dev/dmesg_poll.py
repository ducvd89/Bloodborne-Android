import re, subprocess, sys, time

# Polls the Thor's kernel log and appends only new lines (by timestamp) to argv[1].
out = open(sys.argv[1], 'a')
last = -1.0
while True:
    try:
        txt = subprocess.run(['adb', '-s', '70538e86', 'shell', 'dmesg | tail -n 400'], capture_output=True,
                             text=True, errors='replace', timeout=10).stdout
    except Exception:
        time.sleep(1)
        continue
    stamps = [float(m.group(1)) for m in re.finditer(r'^\[\s*(\d+\.\d+)\]', txt, re.M)]
    if stamps and stamps[-1] < last - 30:  # the device rebooted
        out.write('==== new boot ====\n')
        last = -1.0
    for line in txt.splitlines():
        m = re.match(r'\[\s*(\d+\.\d+)\]', line)
        if m and float(m.group(1)) > last:
            out.write(line + '\n')
            last = float(m.group(1))
    out.flush()
    time.sleep(0.15)
