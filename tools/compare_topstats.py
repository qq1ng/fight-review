"""Check tools/fight_stats.py against a TopStats report, player by player, for every fight both have.

The report is the Legacy.json.gz next to a TopStats page (kept in data/, it holds account names). Fights are
matched to log files by end time, to the minute; players by account. TopStats writes -1 for "not in this
fight", and for heal also "no Healing Stats data".

TopStats sometimes read another player's log for a fight (the user is missing from some fights they recorded).
Its squad then differs from ours, and boon generation, which divides by squad size, can't be compared. Those
fights are left out of the boon rows.

Usage:
    python tools/compare_topstats.py <report.json.gz> <log folder> [--detail METRIC]
"""
import collections
import gzip
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from evtc import Log
from fight_stats import FightStats
from boons import BoonGeneration

# TopStats key -> our counter
METRICS = {"heal": "heal", "cleanses": "cleanses", "rips": "strips", "evades": "evades", "blocks": "blocks",
           "invulns": "invulns"}
# TopStats key -> boon id; values are EI squad generation (average stacks or % uptime)
BOON_METRICS = {"stability": 1122, "quickness": 1187, "might": 740, "protection": 717, "aegis": 743,
                "resistance": 26980, "resolution": 873, "fury": 725, "alacrity": 30328, "vigor": 726,
                "regeneration": 718, "swiftness": 719}


def load_report(path):
    with open(path, "rb") as f:
        return json.loads(gzip.decompress(f.read()))


def log_for_fight(fight, folder):
    stamp = fight["end_time"][:16].replace("-", "").replace(" ", "-").replace(":", "")  # 20260922-2137
    names = sorted(n for n in os.listdir(folder) if n.startswith(stamp) and n.endswith(".zevtc"))
    return os.path.join(folder, names[0]) if names else None


def main(report_path, folder, detail=None):
    report = load_report(report_path)
    pairs = collections.defaultdict(list)
    heal_known = collections.Counter()  # (TopStats has heal, we have heal) -> player-fights
    fights = 0
    boon_fights = 0
    for i, fight in enumerate(report["fights"]):
        path = log_for_fight(fight, folder)
        if not path:
            continue
        fights += 1
        log = Log(path)
        fs = FightStats(log)
        bg = BoonGeneration(log)
        same_squad = fight["squad"] == len(bg.squad)
        boon_fights += same_squad
        by_account = {log.agents[a]["account"]: a for a in fs.friends}
        for player in report["players"]:
            theirs = player["stats_per_fight"][i]
            addr = by_account.get(player["account"])
            if theirs.get("evades", -1) == -1 or addr is None:
                continue
            heal_known[(theirs["heal"] != -1, addr in fs.heal_known)] += 1
            for key, ours in METRICS.items():
                if theirs[key] != -1:
                    pairs[key].append((theirs[key], fs.p[addr][ours], player["name"], i))
            for key, boon in BOON_METRICS.items():
                if same_squad and addr in bg.squad:
                    pairs[key].append((theirs[key], bg.squad_generation(addr, boon), player["name"], i))

    print(f"{fights} fights with a matching log, {boon_fights} with the same squad (boon rows)\n")
    print("healing known (TopStats, ours): " + ", ".join(f"{k}: {n}" for k, n in sorted(heal_known.items())))
    print(f"\n{'metric':13} {'pairs':>6} {'exact':>6} {'<=1%':>6} {'<=5%':>6} {'median ours/theirs':>19}")
    for key, rows in pairs.items():
        rounding = 0.0015 if key in BOON_METRICS else 0  # TopStats rounds boon values to 3 decimals
        within = lambda f: sum(abs(t - o) <= max(f * abs(t), rounding) for t, o, *_ in rows)
        ratios = [o / t for t, o, *_ in rows if t]
        med = statistics.median(ratios) if ratios else float("nan")
        print(f"{key:13} {len(rows):>6} {within(0):>6} {within(0.01):>6} {within(0.05):>6} {med:>19.3f}")
        if detail == key:
            for t, o, name, i in sorted(rows, key=lambda r: -abs(r[0] - r[1]))[:15]:
                print(f"    fight {i:2}  theirs {t:>9}  ours {o:>9}  {name}")


if __name__ == "__main__":
    args = sys.argv[1:]
    detail = args[args.index("--detail") + 1] if "--detail" in args else None
    main(args[0], args[1], detail)
