"""Check tools/skills.py against a TopStats WvW_Combat_Summary report (the TiddlyWiki pages behind the web
report). These tables are totals over the report's fights, so ours are summed over the same fights.

The report's Overview lists every fight with its time and whose log TopStats used ("Log POV"). Fights are
matched to log files by time, within 60 s.

Checked: Skill Usage (casts per minute per skill, per player, and each player's fight time) and Healing by
Skill (hits and total per skill, per player).

Usage:
    python tools/compare_summary.py <summary.json.gz> <log folder> [--player ACCOUNT] [--detail BOON]
"""
import collections
import datetime
import gzip
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from evtc import Log, spec_name
from skills import SkillUsage
from boons import BoonGeneration
from fight_stats import BOONS

strip = lambda s: re.sub(r"<[^>]+>", "", s).strip()
number = lambda s: 0.0 if s in ("-", "") else float(s.replace(",", "").rstrip("%"))


def load(path):
    with open(path, "rb") as f:
        pages = json.loads(gzip.decompress(f.read()))
    by_title = {p.get("title", ""): p for p in pages}
    stamp = next(t for t in by_title if t.endswith("-Log-Summary"))[:-len("Log-Summary")]
    return {t[len(stamp):]: p["text"] for t, p in by_title.items() if t.startswith(stamp)}


def rows(text):
    return [l for l in text.split("\n") if l.startswith("|") and not l.endswith(("|k", "|c"))]


def fights(pages):
    out = []
    for line in rows(pages["Overview"]):
        m = re.match(r"\|(\d+) \|(\d{4})-(\d\d)-(\d\d) - (\d\d):(\d\d):(\d\d) - ", line)
        if m:
            when = datetime.datetime(*map(int, m.group(2, 3, 4, 5, 6, 7)))
            pov = re.search(r'data-tooltip="([^"]+)"', line)
            out.append((int(m.group(1)), when, pov.group(1) if pov else "?"))
    return out


def log_near(when, folder):
    best = None
    for n in os.listdir(folder):
        try:
            t = datetime.datetime.strptime(n[:15], "%Y%m%d-%H%M%S")
        except ValueError:
            continue
        dt = abs((t - when).total_seconds())
        if dt <= 60 and (best is None or dt < best[0]):
            best = (dt, os.path.join(folder, n))
    return best[1] if best else None


def skill_usage(pages):
    """(account, spec) -> (fight time s, {skill name: casts}). A player who swapped spec has a row per spec."""
    out = {}
    for title, text in pages.items():
        if not title.startswith("Skill-Usage-"):
            continue
        spec = title[len("Skill-Usage-"):]
        lines = rows(text)
        header = next(l for l in lines if "!Name" in l)
        names = re.findall(r"\[img[^\[\]]*\[([^|\]]+)\|", header)
        for line in lines:
            cells = [strip(c) for c in line.split("|")[1:-1]]
            if len(cells) < 6 or cells[1] == "!Name" or not re.match(r"[\d.]+$", cells[3]):
                continue
            fight_time = float(cells[3])
            out[(cells[2], spec)] = (fight_time, {n: number(r) * fight_time / 60 for n, r in zip(names, cells[5:])})
    return out


def healing_by_skill(pages):
    """(account, spec) -> {skill name: (hits, total)}"""
    out = {}
    for title, text in pages.items():
        if not title.startswith("Healing-By-Skill-"):
            continue
        account = title.rsplit("-", 1)[1]
        spec = title[len("Healing-By-Skill-"):].split("-", 1)[0]
        table = {}
        for line in rows(text):
            # |[img width=24 [Name|https://...png]]-Name | hits | total | avg | max | pct|
            m = re.search(r"\]\]-(.*?) \| *([\d,]+) \| *([\d,]+) \|", line)
            if m:
                table[m.group(1).strip()] = (number(m.group(2)), number(m.group(3)))
        out[(account, spec)] = table
    return out


GROUP_BOONS = ["Might", "Fury", "Quickness", "Alacrity", "Protection", "Regeneration", "Vigor", "Aegis",
               "Stability", "Swiftness", "Resistance", "Resolution"]


def group_generation(pages):
    """(account, prof abbreviation) -> {boon: seconds generated on the own subgroup, self excluded}.
    The first table on the page is the "Total Gen" view."""
    out = {}
    text = pages["Group-Generation"].split("</$reveal>")[0]
    for line in rows(text):
        acc = re.search(r'data-tooltip="([^"]+)">[^<]*</span> \|\{\{(\w+)\}\}', line)
        if not acc:
            continue
        cells = [strip(c) for c in line.split("|")[1:-1]]
        values = [number(c) for c in cells[4:4 + len(GROUP_BOONS)]]  # party, name, prof, fight time first
        out[(acc.group(1), acc.group(2))] = dict(zip(GROUP_BOONS, values))
    return out


def main(summary, folder, only=None, detail=None):
    pages = load(summary)
    fight_list = fights(pages)
    casts = collections.defaultdict(collections.Counter)
    heal = collections.defaultdict(collections.Counter)
    hits = collections.defaultdict(collections.Counter)
    active = collections.Counter()
    group_gen = collections.defaultdict(collections.Counter)  # (account, spec) -> boon name -> seconds
    povs = collections.Counter(pov for *_, pov in fight_list)
    missing = []
    for n, when, pov in fight_list:
        path = log_near(when, folder)
        if not path:
            missing.append(n)
            continue
        log = Log(path)
        su = SkillUsage(log)
        bg = BoonGeneration(log)
        for a in bg.squad:
            group = {t for t in bg.squad if t != a and log.agents[t]["subgroup"] == log.agents[a]["subgroup"]}
            key = (log.agents[a]["account"], spec_name(log.agents[a]))
            for (src, tgt, boon), ms in bg.credit.items():
                if src == a and tgt in group:
                    group_gen[key][BOONS[boon]] += ms / 1000
        for a in su.players:
            acc = (su.log.agents[a]["account"], spec_name(su.log.agents[a]))
            active[acc] += su.active_ms[a] / 1000
            for s, c in su.casts[a].items():
                casts[acc][su.name(s)] += c
            for s, h in su.heal[a].items():
                heal[acc][su.name(s)] += h
                hits[acc][su.name(s)] += su.heal_hits[a][s]
    print(f"{len(fight_list)} fights, {len(fight_list) - len(missing)} with a log; log POV: {dict(povs)}\n")

    usage = skill_usage(pages)
    exact = close = total = 0
    worst = []
    time_diff = []
    for acc, (fight_time, ts) in usage.items():
        if only and acc[0] != only:
            continue
        time_diff.append(active[acc] - fight_time)
        for skill, theirs in ts.items():
            theirs, ours = round(theirs), casts[acc].get(skill, 0)
            if theirs == 0 and ours == 0:
                continue
            total += 1
            exact += ours == theirs
            close += abs(ours - theirs) <= max(1, 0.05 * theirs)
            worst.append((abs(ours - theirs), acc, skill, theirs, ours))
    print(f"casts per skill: {total} player-skills, {exact} exact, {close} within 1 or 5%")
    if time_diff:
        time_diff.sort()
        print(f"fight time, ours - theirs (s): median {time_diff[len(time_diff) // 2]:+.1f}, "
              f"range {time_diff[0]:+.1f} .. {time_diff[-1]:+.1f}")
    for d, acc, skill, theirs, ours in sorted(worst, reverse=True)[:12]:
        print(f"    {skill[:30]:30} theirs {theirs:>4} ours {ours:>4}  {acc[0]} ({acc[1]})")

    hbs = healing_by_skill(pages)
    exact = close = total = 0
    worst = []
    for acc, table in hbs.items():
        if only and acc[0] != only:
            continue
        for skill, (their_hits, theirs) in table.items():
            ours = heal[acc].get(skill, 0)
            total += 1
            exact += ours == theirs
            close += abs(ours - theirs) <= 0.05 * theirs
            worst.append((abs(ours - theirs) / max(theirs, 1), acc, skill, theirs, ours, their_hits, hits[acc].get(skill, 0)))
    print(f"\nhealing per skill: {total} player-skills, {exact} exact, {close} within 5%")
    for d, acc, skill, theirs, ours, th, oh in sorted(worst, reverse=True)[:12]:
        print(f"    {skill[:30]:30} theirs {theirs:>9,.0f} ({th:.0f} hits) ours {ours:>9,} ({oh} hits)  {acc[0]} ({acc[1]})")

    report_group_generation(pages, group_gen, only, detail)


def report_group_generation(pages, group_gen, only, detail=None):
    theirs_all = group_generation(pages)
    rows_ = collections.defaultdict(list)
    for (acc, prof), theirs in theirs_all.items():
        if only and acc != only:
            continue
        ours_key = next((k for k in group_gen if k[0] == acc and k[1].startswith(prof)), None)
        for boon, t in theirs.items():
            o = group_gen[ours_key][boon] if ours_key else 0.0
            if t or o:
                rows_[boon].append((t, o, acc))
    print(f"\ngroup generation (seconds given to own subgroup), {len(theirs_all)} players")
    print(f"{'boon':13}{'pairs':>6}{'<=2%':>7}{'<=5%':>7}{'<=10%':>7}")
    for boon in GROUP_BOONS:
        r = rows_[boon]
        within = lambda f: sum(abs(o - t) <= max(f * t, 0.5) for t, o, _ in r)
        print(f"{boon:13}{len(r):>6}{within(0.02):>7}{within(0.05):>7}{within(0.10):>7}")
        if boon == detail:
            for t, o, acc in sorted(r, key=lambda x: -abs(x[1] - x[0])):
                print(f"    theirs {t:>9,.1f} ours {o:>9,.1f}  {acc}")


if __name__ == "__main__":
    args = sys.argv[1:]
    only = args[args.index("--player") + 1] if "--player" in args else None
    detail = args[args.index("--detail") + 1] if "--detail" in args else None
    main(args[0], args[1], only, detail)
