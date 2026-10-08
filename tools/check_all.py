"""One check before each deploy: the analysis against TopStats, cast counts against Elite Insights, and every view drawn
on every night's logs. A PASS or FAIL line each, against tools/check_baseline.json; exit code 1 on any failure.

Run after building (scripts/build.ps1): it uses build/release/frcheck.exe and fr_uishot.exe as they are.
    python tools/check_all.py            every check, every night (a few minutes)
    python tools/check_all.py --quick    the views on the last three nights only
    python tools/check_all.py --update   run, then save the current numbers as the baseline (after an intended change)
Prints numbers only, no player names.
"""
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
LOGS = os.path.join(os.path.expanduser("~"), "Documents", "Guild Wars 2", "addons", "arcdps", "arcdps.cbtlogs")
BASELINE = os.path.join(HERE, "check_baseline.json")
UISHOT = os.path.join(ROOT, "build", "release", "fr_uishot.exe")
# the TopStats report the checks use (22 Sept: 21 fights, 591 player-rounds), in data/ (gitignored: it has names)
REPORT = os.path.join(ROOT, "data", "topstats", "20260922_j8dOYp.json.gz")
SUMMARY = os.path.join(ROOT, "data", "topstats", "20260922_j8dOYp_summary.json.gz")
# a view's frame on a busy night: the average and the worst of 30 frames, in ms (first frames after a new round aside)
MAX_AVG_MS, MAX_WORST_MS = 3.0, 8.0


def folder_with(night):
    for f in sorted(glob.glob(os.path.join(LOGS, "WvW*"))):
        if glob.glob(os.path.join(f, night + "-*.zevtc")):
            return f
    return None


def run(cmd, timeout=900):
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout, cwd=ROOT)
    return r.returncode, r.stdout + r.stderr


def topstats():
    """Exact matches per metric; downs, deaths, CC taken and the counters must stay at the baseline or better."""
    code, out = run([sys.executable, os.path.join(HERE, "check_topstats_cpp.py"), REPORT, folder_with("20260922")])
    got = {}
    for line in out.splitlines():
        m = re.match(r"(\w+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+) of (\d+)", line)
        if m:
            got[m.group(1)] = {"pairs": int(m.group(2)), "exact": int(m.group(3)), "within5": int(m.group(4))}
    return code == 0 and got, got


def casts():
    code, out = run([sys.executable, os.path.join(HERE, "check_casts_ei.py"), SUMMARY, folder_with("20260922")])
    m = re.search(r"(\d+) player-skill rows, (\d+) exact", out)
    got = {"rows": int(m.group(1)), "exact": int(m.group(2))} if m else {}
    return code == 0 and bool(got), got


def views(quick):
    """fr_uishot --time on each night: every view drawn (ImGui asserts on in the harness), no crash, frames in budget."""
    nights = sorted({os.path.basename(p)[:8] for p in glob.glob(os.path.join(LOGS, "WvW*", "2026*.zevtc"))})
    if quick:
        nights = nights[-3:]
    results, worst_avg, worst_frame, bad = {}, 0.0, 0.0, []
    with tempfile.TemporaryDirectory() as out_dir:
        for night in nights:
            logs = sorted(glob.glob(os.path.join(LOGS, "WvW*", night + "-*.zevtc")), key=os.path.basename)
            code, out = run([UISHOT, "--fake", "--time", *logs, "--out", out_dir])
            avgs = [float(a) for a in re.findall(r"([\d.]+) ms per frame", out)]
            worsts = [float(w) for w in re.findall(r"\(worst ([\d.]+)\)", out)]
            ok = code == 0 and bool(avgs) and max(avgs) <= MAX_AVG_MS and max(worsts or [0]) <= MAX_WORST_MS
            results[night] = {"ok": ok, "avg": max(avgs or [0]), "worst": max(worsts or [0]), "code": code}
            worst_avg = max(worst_avg, max(avgs or [0]))
            worst_frame = max(worst_frame, max(worsts or [0]))
            if not ok:
                bad.append(f"{night} (exit {code}, avg {max(avgs or [0]):.2f} ms, worst {max(worsts or [0]):.2f} ms)")
    return not bad, {"nights": len(nights), "worst_avg_ms": round(worst_avg, 2), "worst_frame_ms": round(worst_frame, 2), "failed": bad}


def main():
    quick, update = "--quick" in sys.argv, "--update" in sys.argv
    for exe in ("frcheck.exe", "fr_uishot.exe"):
        if not os.path.exists(os.path.join(ROOT, "build", "release", exe)):
            print(f"FAIL  build/release/{exe} is missing: build first (scripts/build.ps1)")
            return 1
    base = json.load(open(BASELINE, encoding="utf-8")) if os.path.exists(BASELINE) else {}
    failed = False

    ok, ts = topstats()
    worse = []
    for key, v in (ts or {}).items():
        b = base.get("topstats", {}).get(key)
        if b and v["exact"] < b["exact"]:
            worse.append(f"{key} {v['exact']} of {v['pairs']} exact (was {b['exact']})")
    ok = ok and not worse
    failed |= not ok
    total = sum(v["exact"] for v in (ts or {}).values()), sum(v["pairs"] for v in (ts or {}).values())
    print(f"{'PASS' if ok else 'FAIL'}  TopStats, 22 Sept: {total[0]} of {total[1]} metric values exact" + (": " + "; ".join(worse) if worse else ""))

    ok, cs = casts()
    b = base.get("casts", {})
    if ok and b and cs["exact"] < b["exact"]:
        ok = False
    failed |= not ok
    print(f"{'PASS' if ok else 'FAIL'}  Casts against Elite Insights, 22 Sept: {cs.get('exact', '?')} of {cs.get('rows', '?')} player-skill rows exact"
          + (f" (was {b['exact']})" if b and cs.get("exact") != b.get("exact") else ""))

    ok, vw = views(quick)
    failed |= not ok
    print(f"{'PASS' if ok else 'FAIL'}  Every view on {vw['nights']} nights: no crash, frames up to {vw['worst_avg_ms']} ms on average and "
          f"{vw['worst_frame_ms']} ms at worst (limits {MAX_AVG_MS} and {MAX_WORST_MS})" + (": " + ", ".join(vw["failed"]) if vw["failed"] else ""))

    if update:
        json.dump({"topstats": ts, "casts": cs}, open(BASELINE, "w", encoding="utf-8"), indent=1)
        print(f"Baseline saved to {os.path.relpath(BASELINE, ROOT)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
