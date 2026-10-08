"""Patch notes (data/patchnotes/*.wiki, from tools/patchnotes.py fetch) into one row per change, for reading logs against
the game's changes. Used by `python tools/patchnotes.py parse`.

A dated page holds its updates as "== Update - July 14, 2026 ==" sections (also "Update 2", "Late Notes"); a balance
update also holds the preview the team posted weeks before ("== Balance Update Preview - June 22, 2026 =="). The release
day's "Profession Skills" lists only what changed against the preview and supersedes it, and late notes correct both. So
every line is kept with where it came from (preview / update / late) and dated to the release day; for one skill, a later
source wins.

Each change line ("* {{game update icon|Volcano}}: Reduced ... in WvW only.") is split into sentences, each with its game
modes: "in WvW only" -> WvW; "in PvE and WvW" -> PvE+WvW; "in PvP and WvW" -> PvP+WvW; no mode named -> all. Numbers
"from X to Y" are kept as from / to.

Output: data/patchnotes/changes.csv
    date, page, source, section, profession, spec, skill, modes, wvw, text, from, to
"""
import csv
import json
import os
import re

PROFESSIONS = ["Guardian", "Warrior", "Engineer", "Ranger", "Thief", "Elementalist", "Mesmer", "Necromancer", "Revenant"]
SPECS = {
    "Dragonhunter": "Guardian", "Firebrand": "Guardian", "Willbender": "Guardian", "Luminary": "Guardian",
    "Berserker": "Warrior", "Spellbreaker": "Warrior", "Bladesworn": "Warrior", "Paragon": "Warrior",
    "Scrapper": "Engineer", "Holosmith": "Engineer", "Mechanist": "Engineer", "Amalgam": "Engineer",
    "Druid": "Ranger", "Soulbeast": "Ranger", "Untamed": "Ranger", "Galeshot": "Ranger",
    "Daredevil": "Thief", "Deadeye": "Thief", "Specter": "Thief", "Antiquary": "Thief",
    "Tempest": "Elementalist", "Weaver": "Elementalist", "Catalyst": "Elementalist", "Evoker": "Elementalist",
    "Chronomancer": "Mesmer", "Mirage": "Mesmer", "Virtuoso": "Mesmer", "Troubadour": "Mesmer",
    "Reaper": "Necromancer", "Scourge": "Necromancer", "Harbinger": "Necromancer", "Ritualist": "Necromancer",
    "Herald": "Revenant", "Renegade": "Revenant", "Vindicator": "Revenant", "Conduit": "Revenant",
}
MONTHS = {m: i + 1 for i, m in enumerate(["January", "February", "March", "April", "May", "June", "July", "August", "September",
                                           "October", "November", "December"])}


def clean(s):
    """wiki markup to plain text"""
    s = re.sub(r"\{\{game update icon\|([^}|]*)(\|[^}]*)?\}\}", r"\1", s, flags=re.I)
    s = re.sub(r"\{\{[^{}]*\}\}", "", s)
    s = re.sub(r"\[\[(?:[^]|]*\|)?([^]]*)\]\]", r"\1", s)
    s = re.sub(r"\[https?://\S+ ([^]]*)\]", r"\1", s)
    s = re.sub(r"</?[a-z]+[^>]*>", "", s)
    s = s.replace("'''", "").replace("''", "")
    return re.sub(r"\s+", " ", s).strip()


def section_date(title, page_date):
    """'Update - July 14, 2026' -> 2026-07-14; else the page's date"""
    m = re.search(r"(January|February|March|April|May|June|July|August|September|October|November|December) (\d{1,2}),? (\d{4})", title)
    if m:
        return f"{int(m.group(3)):04d}-{MONTHS[m.group(1)]:02d}-{int(m.group(2)):02d}"
    return page_date


def modes_of(sentence):
    s = sentence.lower()
    m = re.search(r"\bin (pve|pvp|wvw)(?:,? (?:and|or) (pve|pvp|wvw))?(?: only)?\b", s)
    if not m:
        return "all"
    found = [g for g in m.groups() if g]
    return "+".join(sorted({{"pve": "PvE", "pvp": "PvP", "wvw": "WvW"}[g] for g in found}))


def numbers(sentence):
    m = re.search(r"from (-?[\d,.]+%?)(?: seconds?| s)? to (-?[\d,.]+%?)", sentence)
    return (m.group(1), m.group(2)) if m else ("", "")


def parse(folder):
    manifest = json.load(open(os.path.join(folder, "manifest.json"), encoding="utf-8"))
    rows = []
    for page in manifest["pages"]:
        text = open(os.path.join(folder, page["date"] + ".wiki"), encoding="utf-8").read()
        # the release day: the first "Update" section's date (a preview's own date isn't when it went live)
        live = page["date"]
        for title in re.findall(r"^==\s*([^=].*?)\s*==\s*$", text, re.M):
            if title.lower().startswith("update"):
                live = section_date(title, page["date"])
                break
        source, section, prof, spec = "update", "", "", ""
        for line in text.splitlines():
            h = re.match(r"^(=+)\s*(.*?)\s*=+\s*$", line)
            if h:
                level, title = len(h.group(1)), clean(h.group(2))
                if level == 2:
                    low = title.lower()
                    source = "preview" if "preview" in low else "late" if "late note" in low else "update"
                    section, prof, spec = title, "", ""
                elif title in PROFESSIONS:
                    prof, spec = title, ""
                elif title in SPECS:
                    prof, spec = SPECS[title], title
                else:
                    section, prof, spec = title, "", ""
                continue
            if not line.startswith("*"):
                continue
            body = line.lstrip("*").strip()
            # "{{game update icon|Volcano}}: ..." (also "Game update icon"), or a plain link "[[Release Potential]]: ..."
            m = re.match(r"\{\{game update icon\|([^}|]*)(?:\|[^}]*)?\}\}\s*:?\s*(.*)", body, re.I) or \
                re.match(r"\[\[([^]|]*)(?:\|[^]]*)?\]\]\s*:\s*(.*)", body)
            skill, rest = (re.sub(r"\s*\([^)]*\)$", "", m.group(1)).strip(), m.group(2)) if m else ("", body)
            rest = clean(rest)
            if not rest:
                continue
            # a sentence each: modes can differ inside one line ("... in PvE and reduced ... in WvW")
            sentences = [x.strip() for x in re.split(r"(?<=[.!])\s+(?=[A-Z])", rest) if x.strip()]
            for s in sentences:
                parts = re.split(r",? and (?=(?:increased|reduced|decreased|lowered|raised|changed)\b)", s, flags=re.I)
                stated = [modes_of(p) for p in parts]
                # a clause with no mode of its own takes the one that closes its sentence ("Increased the duration ... and
                # increased the power coefficient ... in WvW only": both WvW); a sentence that names each mode keeps them
                for k in range(len(parts) - 2, -1, -1):
                    if stated[k] == "all" and stated[k + 1] != "all":
                        stated[k] = stated[k + 1]
                for part, md in zip(parts, stated):
                    lo, hi = numbers(part)
                    rows.append({"date": live, "page": page["date"], "source": source, "section": section, "profession": prof, "spec": spec,
                                 "skill": skill, "modes": md, "wvw": int(md == "all" or "WvW" in md), "text": part, "from": lo, "to": hi})
    out = os.path.join(folder, "changes.csv")
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    prof_rows = [r for r in rows if r["profession"]]
    print(f"{len(rows)} change sentences from {len(manifest['pages'])} pages -> {os.path.normpath(out)}")
    print(f"  profession changes: {len(prof_rows)}; affecting WvW: {sum(r['wvw'] for r in prof_rows)}; WvW only: {sum(1 for r in prof_rows if r['modes'] == 'WvW')}")
    by_date = {}
    for r in prof_rows:
        if r["wvw"]:
            by_date[r["date"]] = by_date.get(r["date"], 0) + 1
    print("  updates with 20+ profession changes affecting WvW:")
    for d, n in sorted(by_date.items()):
        if n >= 20:
            print(f"    {d}: {n}")
