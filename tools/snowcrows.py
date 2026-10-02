"""Snow Crows' WvW builds as readable text, for checking our spec jobs against them. Standard library only.

The build pages are server-rendered: gear, traits and skills are GW2 armory embeds carrying ids (data-armory-ids,
data-armory-<spec>-traits), and the guide text names skills and traits the same way. This fetches every WvW build page,
resolves the ids through the GW2 API and writes one file per build to reference/snowcrows/ (third-party material,
gitignored), plus index.tsv (build, skill bar, traits, build template code).

Usage:
    python tools/snowcrows.py            fetch everything (about 40 pages and a few API calls)
    python tools/snowcrows.py --cached   rebuild the text from the pages already saved
"""
import html
import json
import os
import re
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "reference", "snowcrows")
SITE = "https://snowcrows.com"
PROFESSIONS = ["guardian", "warrior", "engineer", "ranger", "thief", "elementalist", "mesmer", "necromancer", "revenant"]
UA = {"User-Agent": "Mozilla/5.0 (fight-review build notes)"}


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=30) as r:
        return r.read().decode("utf-8", "replace")


def api(kind, ids, cache):
    """Names from the GW2 API (v2/skills, traits, items, specializations), 150 ids a call, cached."""
    want = sorted({i for i in ids if i not in cache[kind]})
    for n in range(0, len(want), 150):
        part = want[n:n + 150]
        try:
            for e in json.loads(get(f"https://api.guildwars2.com/v2/{kind}?ids={','.join(map(str, part))}&lang=en")):
                cache[kind][e["id"]] = e.get("name", "")
        except Exception as ex:  # a missing id makes the whole call fail with 404: try one by one
            print(f"  api {kind}: {ex}; one by one")
            for i in part:
                try:
                    cache[kind][i] = json.loads(get(f"https://api.guildwars2.com/v2/{kind}/{i}?lang=en")).get("name", "")
                except Exception:
                    cache[kind][i] = ""
    return cache


EMBED = re.compile(r'<(?:div|span)[^>]*data-armory-embed="(skills|traits|items|specializations)"[^>]*?data-armory-ids?="([\d,]+)"[^>]*>')


def page_text(src):
    """The page's text from the build's title to the comments box, embeds as {kind:id}."""
    body = re.sub(r"<script.*?</script>|<style.*?</style>", "", src, flags=re.S)
    body = EMBED.sub(lambda m: " " + " ".join(f"{{{m.group(1)}:{i}}}" for i in m.group(2).split(",")) + " ", body)
    t = html.unescape(re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", body)))
    start = t.find("Build Difficulty")
    end = t.find("Have a comment or question")
    return t[start if start >= 0 else 0:end if end >= 0 else None].strip()


def parse(slug, src):
    b = {"slug": slug}
    m = re.search(r"<title>(.*?)</title>", src, re.S)
    b["title"] = html.unescape(m.group(1)).split(" - Guild Wars 2")[0].strip() if m else slug
    m = re.search(r"writeText\('(\[&amp;[^']+\])'\)", src)
    b["template"] = html.unescape(m.group(1)) if m else ""
    b["specs"] = [(int(s), [int(x) for x in t.split(",")]) for s, t in
                  re.findall(r'data-armory-embed="specializations" data-armory-ids="(\d+)" data-armory-\d+-traits="([\d,]+)"', src)]
    # the bar: the first skill embed that isn't inline text (5 skills; a Revenant's 2 legends)
    bars = re.findall(r'data-armory-embed="skills" data-armory-ids="([\d,]+)"(?! data-armory-size="20" data-armory-inline-text)', src)
    b["bar"] = [int(x) for x in bars[0].split(",")] if bars else []
    b["text"] = page_text(src)
    return b


def main():
    os.makedirs(os.path.join(OUT, "pages"), exist_ok=True)
    cached = "--cached" in sys.argv
    slugs = []
    for p in PROFESSIONS:
        if cached:
            slugs += [f"{p}/{f[len(p) + 1:-5]}" for f in os.listdir(os.path.join(OUT, "pages")) if f.startswith(p + "_")]
            continue
        slugs += sorted(set(re.findall(rf"/builds/wvw/({p}/[a-z0-9-]+)", get(f"{SITE}/builds/wvw/{p}"))))
        time.sleep(0.5)
    builds = []
    for s in slugs:
        path = os.path.join(OUT, "pages", s.replace("/", "_") + ".html")
        if not cached:
            print("fetch", s)
            with open(path, "w", encoding="utf-8") as f:
                f.write(get(f"{SITE}/builds/wvw/{s}"))
            time.sleep(0.5)
        with open(path, encoding="utf-8") as f:
            builds.append(parse(s, f.read()))
    cache_path = os.path.join(OUT, "names.json")
    cache = {k: {} for k in ("skills", "traits", "items", "specializations")}
    if os.path.exists(cache_path):
        with open(cache_path, encoding="utf-8") as f:
            cache = {k: {int(i): n for i, n in v.items()} for k, v in json.load(f).items()}
    ids = {k: set() for k in cache}
    for b in builds:
        for kind, i in re.findall(r"\{(skills|traits|items|specializations):(\d+)\}", b["text"]):
            ids[kind].add(int(i))
        ids["skills"].update(b["bar"])
        for spec, traits in b["specs"]:
            ids["specializations"].add(spec)
            ids["traits"].update(traits)
    for kind in ids:
        api(kind, ids[kind], cache)
    with open(cache_path, "w", encoding="utf-8") as f:
        json.dump(cache, f, indent=0)
    cache["skills"][10586] = "(flexible slot)"  # Snow Crows' placeholder; the API calls it "Locked"
    name = lambda kind, i: cache[kind].get(int(i)) or f"{kind[:-1]} {i}"
    index = ["slug\ttitle\tskill bar\ttraits\ttemplate"]
    for b in builds:
        text = re.sub(r"\{(skills|traits|items|specializations):(\d+)\}", lambda m: f"[{name(m.group(1), m.group(2))}]", b["text"])
        text = re.sub(r"(\[[^\]]+\]) (?:\1 ?)+", r"\1 ", text)  # an item shown twice (icon and name)
        bar = ", ".join(name("skills", i) for i in b["bar"])
        traits = "; ".join(f"{name('specializations', s)}: {', '.join(name('traits', t) for t in ts)}" for s, ts in b["specs"])
        with open(os.path.join(OUT, b["slug"].replace("/", "_") + ".md"), "w", encoding="utf-8") as f:
            f.write(f"# {b['title']}\n\n{SITE}/builds/wvw/{b['slug']}\n\nSkill bar (heal, utilities, elite; Revenant: legends): {bar}\n\n"
                    f"Traits: {traits}\n\nTemplate: {b['template']}\n\n## Page text\n\n{text}\n")
        index.append("\t".join([b["slug"], b["title"], bar, traits, b["template"]]))
    with open(os.path.join(OUT, "index.tsv"), "w", encoding="utf-8") as f:
        f.write("\n".join(index) + "\n")
    unresolved = sum(1 for k in cache for n in cache[k].values() if not n)
    print(f"{len(builds)} builds written to {os.path.normpath(OUT)}; {unresolved} ids without a name")


if __name__ == "__main__":
    main()
