"""Parameter sweep: python tune.py key=v1,v2 [key2=...] -- runs all_open + mech coverage over all areas for each combination."""
import itertools, json, sys
from multiprocessing import Pool
import coverage
from navconf import AREAS, PARAMS


def job(args):
    a, params, scs = args
    try:
        o = coverage.run_area(a, scenarios=scs, params=params)
        return a, {s: (v['lamp_pairs_ok'], v['lamp_pairs'], v['pickups_ok'], v['pickups'], v['npcs_ok'], v['npcs']) for s, v in o['scenarios'].items()}
    except Exception as e:
        return a, repr(e)


if __name__ == '__main__':
    keys, vals = [], []
    for arg in sys.argv[1:]:
        k, v = arg.split('=')
        keys.append(k); vals.append([float(x) for x in v.split(',')])
    scs = ['all_open', 'mech']
    with Pool(12) as pool:
        for combo in itertools.product(*vals):
            params = dict(zip(keys, combo))
            res = pool.map(job, [(a, params, scs) for a in AREAS])
            tot = {s: [0] * 6 for s in scs}
            for a, r in res:
                if isinstance(r, str):
                    print(a, r); continue
                for s in scs:
                    for i in range(6):
                        tot[s][i] += r[s][i]
            print(params, {s: f'L {t[0]}/{t[1]} P {t[2]}/{t[3]} N {t[4]}/{t[5]}' for s, t in tot.items()}, flush=True)
