"""Guild Wars 2 patch notes from the official wiki, for reading the logs against the game's changes (the meta
moves with the patches, so the patch dates cut the logs into periods).

    python tools/patchnotes.py fetch [--from 2024-01]   every dated update page since then, raw wiki text, to data/patchnotes/
    python tools/patchnotes.py parse                    data/patchnotes/changes.csv: one row per change line

Standard library only. The wiki's API (wiki.guildwars2.com/api.php): the yearly categories ("Category:2024 updates") hold
monthly ones ("Category:August 2024 updates"), which hold the dated pages ("Game updates/2024-08-20"); the monthly page is
only a template that shows them. Balance and skill pages are also in "Category:Balance updates" / "Skill and trait
changes". Polite: one request a second, batches of 50 pages, a user agent that names the project.
"""
import csv
import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "data", "patchnotes")
UA = "FightReview-patchnotes/1.0 (personal log analysis; github.com/qq1ng/fight-review)"
MONTHS = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"]


def api(**params):
    params["format"] = "json"
    url = "https://wiki.guildwars2.com/api.php?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                data = json.load(r)
            time.sleep(1)
            return data
        except Exception as e:  # a hiccup: wait and try again
            if attempt == 2:
                raise
            print("  retry:", e)
            time.sleep(5)


def members(cat):
    out, cont = [], {}
    while True:
        d = api(action="query", list="categorymembers", cmtitle=cat, cmlimit="500", **cont)
        out += [m["title"] for m in d["query"]["categorymembers"]]
        if "continue" not in d:
            return out
        cont = {"cmcontinue": d["continue"]["cmcontinue"]}


def fetch(since):
    os.makedirs(OUT, exist_ok=True)
    y0, m0 = (int(x) for x in since.split("-"))
    this_year = time.gmtime().tm_year
    titles = []
    for year in range(y0, this_year + 1):
        for i, month in enumerate(MONTHS):
            if (year, i + 1) < (y0, m0):
                continue
            cat = f"Category:{month} {year} updates"
            got = [t for t in members(cat) if re.fullmatch(r"Game updates/\d{4}-\d{2}-\d{2}", t)]
            if got:
                print(f"{cat}: {len(got)} pages")
            titles += got
    balance = set(members("Category:Balance updates"))
    skills = set(members("Category:Skill and trait changes"))
    manifest = []
    for k in range(0, len(titles), 50):
        batch = titles[k:k + 50]
        d = api(action="query", prop="revisions", rvprop="content|ids|timestamp", rvslots="main", titles="|".join(batch))
        for page in d["query"]["pages"].values():
            if "revisions" not in page:
                print("  missing:", page.get("title"))
                continue
            rev = page["revisions"][0]
            date = page["title"].split("/")[-1]
            with open(os.path.join(OUT, date + ".wiki"), "w", encoding="utf-8") as f:
                f.write(rev["slots"]["main"]["*"])
            manifest.append({"title": page["title"], "date": date, "revid": rev["revid"], "edited": rev["timestamp"],
                             "balance": page["title"] in balance, "skills": page["title"] in skills})
    manifest.sort(key=lambda m: m["date"])
    with open(os.path.join(OUT, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump({"source": "https://wiki.guildwars2.com", "fetched": time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime()), "pages": manifest}, f, indent=1)
    print(f"{len(manifest)} pages saved to {os.path.normpath(OUT)}; balance updates {sum(m['balance'] for m in manifest)}, "
          f"with skill and trait changes {sum(m['skills'] for m in manifest)}")


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "fetch":
        since = sys.argv[sys.argv.index("--from") + 1] if "--from" in sys.argv else "2024-01"
        fetch(since)
    elif len(sys.argv) >= 2 and sys.argv[1] == "parse":
        from patchnotes_parse import parse  # noqa: E402 (written once the wiki text's shape is known)
        parse(OUT)
    else:
        print(__doc__)
