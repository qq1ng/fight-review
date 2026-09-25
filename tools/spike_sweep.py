"""Tune spike detection: try detector settings on per-second damage and score them against the downs.

A spike is a second that is the highest within RADIUS seconds, at least OVER x the round's median busy second and
OF x its biggest (src/Analysis.cpp, Spikes). Optionally the series is summed over 2 s first.

Scored per side (our spikes against enemy downs, enemy spikes against our downs), on rounds of 30 s or more:
- precision: spikes with a down from 1 s before to 4 s after
- recall: downs with a spike in that window
- lift: precision over what random seconds would score (a detector that marks everything has lift 1)

Input: `frcheck --series <logs>` output (kept in data/: it's derived from logs).

Usage:
    python tools/spike_sweep.py data/spike_series_train.tsv [data/spike_series_test.tsv]
"""
import itertools
import statistics
import sys

BEFORE, AFTER = 1000, 4000


def load(path):
    rounds, cur = [], None
    with open(path, encoding="utf-8-sig") as fh:
        for line in fh:
            f = line.rstrip("\n").split("\t")
            if f[0] == "round":
                cur = {"stamp": f[1], "ms": int(f[2])}
                rounds.append(cur)
            elif f[0] in ("out", "in", "enemydowns", "squaddowns"):
                cur[f[0]] = [int(v) for v in f[1:] if v]
    return [r for r in rounds if r["ms"] >= 30000]


def spikes(series, over, of, radius, smooth):
    if smooth > 1:
        series = [sum(series[i:i + smooth]) for i in range(len(series))]
    busy = sorted(v for v in series if v > 0)
    if not busy:
        return []
    floor = max(over * statistics.median(busy), of * busy[-1])
    out, last = [], -1000
    for i, v in enumerate(series):
        if v < floor:
            continue
        lo, hi = max(0, i - radius), min(len(series) - 1, i + radius)
        if any(series[j] > v for j in range(lo, hi + 1)):
            continue
        if i - last > radius:
            # the middle of the (summed) window
            out.append(i * 1000 + smooth * 500)
            last = i
    return out


def score(rounds, side, over, of, radius, smooth):
    series_key, downs_key = ("out", "enemydowns") if side == "ours" else ("in", "squaddowns")
    n_spikes = hit = n_downs = caught = 0
    random_hit = 0.0
    minutes = 0.0
    for r in rounds:
        sp = spikes(r[series_key], over, of, radius, smooth)
        downs = r[downs_key]
        n_spikes += len(sp)
        hit += sum(any(s - BEFORE <= d <= s + AFTER for d in downs) for s in sp)
        n_downs += len(downs)
        caught += sum(any(s - BEFORE <= d <= s + AFTER for s in sp) for d in downs)
        # a random second's chance of having a down in its window
        secs = len(r[series_key])
        if secs:
            random_hit += len(sp) * sum(any(t * 1000 + 500 - BEFORE <= d <= t * 1000 + 500 + AFTER for d in downs) for t in range(secs)) / secs
        minutes += r["ms"] / 60000
    p = hit / n_spikes if n_spikes else 0
    rc = caught / n_downs if n_downs else 0
    f1 = 2 * p * rc / (p + rc) if p + rc else 0
    lift = hit / random_hit if random_hit else 0
    return {"p": p, "r": rc, "f1": f1, "lift": lift, "per_min": n_spikes / minutes if minutes else 0}


def main(train_path, test_path=None):
    train = load(train_path)
    test = load(test_path) if test_path else None
    grid = list(itertools.product([1.2, 1.4, 1.6, 1.8, 2.0, 2.5], [0.2, 0.25, 0.3, 0.35, 0.4, 0.5], [2, 3, 4], [1, 2]))
    current = (1.6, 0.35, 3, 1)
    for side in ("ours", "theirs"):
        rows = [(params, score(train, side, *params)) for params in grid]
        rows.sort(key=lambda r: -r[1]["f1"])
        print(f"\n{side} spikes, {len(train)} training rounds: best by F1 (precision x recall)")
        print(f"  {'over':>4} {'of':>5} {'rad':>3} {'sum':>3}   {'prec':>5} {'recall':>6} {'F1':>5} {'lift':>5} {'/min':>5}")
        shown = [r for r in rows[:8]] + [r for r in rows if r[0] == current]
        for params, s in shown:
            tag = "  <- now" if params == current else ""
            print(f"  {params[0]:>4} {params[1]:>5} {params[2]:>3} {params[3]:>3}   {s['p']:5.2f} {s['r']:6.2f} {s['f1']:5.2f} {s['lift']:5.2f} {s['per_min']:5.2f}{tag}")
        if test:
            best = rows[0][0]
            for label, params in (("best", best), ("now", current)):
                s = score(test, side, *params)
                print(f"  test ({len(test)} rounds), {label} {params}: precision {s['p']:.2f}, recall {s['r']:.2f}, F1 {s['f1']:.2f}, lift {s['lift']:.2f}, {s['per_min']:.2f}/min")


if __name__ == "__main__":
    main(*sys.argv[1:3])
