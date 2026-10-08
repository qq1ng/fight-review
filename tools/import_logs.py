"""Logs from other players, as they come (folders, zips, half-written files), into one list of fights with each fight once.

    python tools/import_logs.py [folder ...] [--out dir]     writes data/import/

Folders to read: the ones given, else those listed in data/log_sources.txt (local; one folder a line, any drive: other
players' logs can live elsewhere, the player's own ArcDPS folder stays where ArcDPS writes it), else data/logs. Under a
folder, one subfolder per player ("player A", ...): the manifest's "source" says whose a log is (the ArcDPS folder
itself: "own"), for tools/causal/compare.py.

Logs come from several players, and players of one squad send the same nights. So a fight can arrive as the same file twice (a zip next to its own files), or as two players' recordings of it.

Found under the folders:
    *.zevtc               the usual: a zip holding one EVTC file
    *.zip                 an archive of logs: its entries are read too (unpacked to data/import/unzipped/ when not
                          already present loose); a zip that won't open is reported as broken
    any other file        a raw EVTC file when it starts with "EVTC" (ArcDPS leaves one when the game closes while it
                          compresses: "20250102-222918" next to a zero-filled "20250102-222918.tmp.zip")
    html, json, ...       not logs; skipped

Duplicates, in two steps:
 1. The same log twice: the same EVTC bytes (sha1 of the unzipped log).
 2. The same fight recorded by two players: same map instance, same squad, overlapping time. Every log names its map
    instance (CBTS_INSTANCESTART: when the instance started, in the recorder's clock, and from 2025 the server's
    address) and the server's clock at squad combat start and end (CBTS_SQCOMBATSTART / END "value"). Two recorders
    in the same instance get the same instance start to within a second or two. Same squad: most of the smaller
    squad's accounts are in the other's. Among the logs of one fight the longest (then the one with most events) is
    kept; a log mostly inside a kept one (half its time or more) is a duplicate of it; one that runs well past it is
    kept too, and the overlap is reported.

Writes (data/ is gitignored: accounts are kept as hashes here, but paths and recorders are still personal):
    data/import/manifest.csv   one row per file found: what it is, kept or why not
    data/import/logs.txt       the kept logs in server-time order, "path<TAB>night", for tools/causal (frcauses @list)

Standard library only.
"""
import csv
import hashlib
import json
import multiprocessing
import os
import struct
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from evtc import AGENT, BUFF_DMG, EVENT, NOT_PLAYER, SC, SRC, STATECHANGE, TIME, VALUE  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, ".."))
OUT = os.path.join(ROOT, "data", "import")
SOURCES = os.path.join(ROOT, "data", "log_sources.txt")
CACHE = os.path.join(OUT, "cache.json")
SKIP_EXT = {".html", ".htm", ".json", ".txt", ".csv", ".png", ".jpg", ".ini", ".log", ".md"}
HEAD_EVENTS = 20000   # the log's identity statechanges are written at its start
TAIL_EVENTS = 4000    # and its end
NIGHT_GAP_S = 3 * 3600
MAX_SPAN_MS = 3 * 3600 * 1000  # no fight log runs longer
SAME_INSTANCE_S = 5   # recorders in one instance agree on its start to within this
SAME_SQUAD = 0.5      # share of the smaller squad's accounts found in the other


def fnv(s):
    h = 2166136261
    for c in s.encode("utf-8"):
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def identity(data):
    """What a log says about itself; raises ValueError when it isn't a readable EVTC log."""
    if len(data) < 16 or data[:4] != b"EVTC":
        raise ValueError("not an EVTC log")
    if len(data) < 20:
        raise ValueError(f"only a header ({len(data)} bytes)")
    if data[12] != 1:
        raise ValueError(f"EVTC revision {data[12]}")
    pos = 16
    (n,) = struct.unpack_from("<I", data, pos)
    pos += 4
    if pos + n * AGENT.size > len(data):
        raise ValueError("cut short in the agent table")
    accounts, addr_account = set(), {}
    for i in range(n):
        addr, prof, elite, *_, raw = AGENT.unpack_from(data, pos + i * AGENT.size)
        if elite == NOT_PLAYER:
            continue
        parts = raw.split(b"\0")
        acct = parts[1].decode("utf-8", "replace").lstrip(":") if len(parts) > 1 else ""
        sub = parts[2] if len(parts) > 2 else b""
        if acct:
            addr_account[addr] = acct
            if sub.isdigit() and int(sub) > 0:
                accounts.add(acct)
    pos += n * AGENT.size
    if pos + 4 > len(data):
        raise ValueError("cut short in the skill table")
    (m,) = struct.unpack_from("<I", data, pos)
    pos += 4 + m * 68
    events = (len(data) - pos) // EVENT.size if pos <= len(data) else 0
    if events < 10:
        raise ValueError("no events")
    head = EVENT.iter_unpack(data[pos:pos + min(events, HEAD_EVENTS) * EVENT.size])
    tail_from = max(min(events, HEAD_EVENTS), events - TAIL_EVENTS)
    tail = EVENT.iter_unpack(data[pos + tail_from * EVENT.size:pos + events * EVENT.size])
    info = {"arc": data[4:12].decode("ascii", "replace"), "species": struct.unpack_from("<H", data, 13)[0], "events": events,
            "squad": len(accounts), "accounts": " ".join(f"{fnv(a):08x}" for a in sorted(accounts)),
            "gw2": 0, "map": 0, "shard": 0, "socket": 0, "server_start": 0, "server_end": 0, "local_start": 0, "start_ms": 0,
            "end_ms": 0, "inst_ms": None, "recorder": "", "first_ms": 0, "last_ms": 0}
    for e in list(head) + list(tail):
        sc, t = e[STATECHANGE], e[TIME]
        if sc == 0:
            if t and (not info["first_ms"] or t < info["first_ms"]):
                info["first_ms"] = t
            info["last_ms"] = max(info["last_ms"], t)
        elif sc == SC["SQCOMBATSTART"] and not info["server_start"]:
            info["server_start"], info["local_start"], info["start_ms"] = e[VALUE] & 0xFFFFFFFF, e[BUFF_DMG] & 0xFFFFFFFF, t
        elif sc == SC["SQCOMBATEND"]:
            info["server_end"], info["end_ms"] = e[VALUE] & 0xFFFFFFFF, t
        elif sc == SC["POINTOFVIEW"]:
            info["recorder"] = addr_account.get(e[SRC], "")
        elif sc == SC["GWBUILD"]:
            info["gw2"] = e[SRC]
        elif sc == SC["SHARDID"]:
            info["shard"] = e[SRC]
        elif sc == SC["MAPID"]:
            info["map"] = e[SRC]
        elif sc == SC["INSTANCESTART"] and info["inst_ms"] is None:
            # src: when the instance started, in the same clock as the events' time (signed: before the clock's zero)
            info["inst_ms"] = e[SRC] - (1 << 64) if e[SRC] >= 1 << 63 else e[SRC]
            info["socket"] = e[VALUE] & 0xFFFFFFFF
    if not info["server_start"]:
        raise ValueError("no squad combat start")
    # the log's span in server seconds: its events, placed by the start event's server time
    t0 = info["first_ms"] or info["start_ms"]
    t1 = max(info["last_ms"], info["end_ms"])
    if not 0 <= t1 - t0 <= MAX_SPAN_MS:
        # a clock jump inside the log (in a 10,000-log zip: spans of years that stalled the duplicate check):
        # squad combat start to end instead, else only the start
        t0, t1 = info["start_ms"], info["end_ms"]
        if not 0 <= t1 - t0 <= MAX_SPAN_MS:
            t1 = t0
    info["from"] = info["server_start"] + (t0 - info["start_ms"]) / 1000.0
    info["to"] = info["server_start"] + (t1 - info["start_ms"]) / 1000.0
    info["seconds"] = round(info["to"] - info["from"], 1)
    info["instance"] = round(info["server_start"] - (info["start_ms"] - info["inst_ms"]) / 1000.0) if info["inst_ms"] is not None else 0
    info["recorder"] = f"{fnv(info['recorder']):08x}" if info["recorder"] else ""
    del info["inst_ms"], info["first_ms"], info["last_ms"], info["start_ms"], info["end_ms"]
    return info


def unzip_log(raw):
    """a .zevtc's bytes to the EVTC inside"""
    import io
    with zipfile.ZipFile(io.BytesIO(raw)) as z:
        names = [i for i in z.infolist() if not i.is_dir()]
        if not names:
            raise ValueError("empty zip")
        return z.read(names[0])


def read_one(job):
    """(path, member or None) -> manifest row; member: an entry of an archive"""
    path, member = job
    row = {"path": path, "member": member or "", "status": "", "why": "", "bytes": 0, "sha1": ""}
    try:
        if member is None:
            with open(path, "rb") as f:
                raw = f.read()
        else:
            with zipfile.ZipFile(path) as z:
                raw = z.read(member)
        row["bytes"] = len(raw)
        data = unzip_log(raw) if raw[:2] == b"PK" else raw
        row["sha1"] = hashlib.sha1(data).hexdigest()
        row.update(identity(data))
        row["status"] = "log"
    except (ValueError, zipfile.BadZipFile, struct.error, OSError, EOFError) as e:
        row["status"], row["why"] = "broken", str(e)[:120]
    return row


def rel(p):
    """a path as the manifest shows it: from the project when on its drive, else in full (logs kept on another drive)"""
    try:
        return os.path.relpath(p, ROOT)
    except ValueError:
        return os.path.abspath(p)


def source_of(root, p):
    """whose logs: the folder right under the root they were found in (one folder per player), "own" for the ArcDPS log
    folder itself"""
    if os.path.basename(os.path.normpath(root)).lower() == "arcdps.cbtlogs":
        return "own"
    part = os.path.relpath(p, root).split(os.sep)
    if len(part) == 1 and part[0].lower().endswith(".zip"):
        return os.path.splitext(part[0])[0]  # an archive right under the root: its own player folder
    return part[0] if len(part) > 1 else os.path.basename(os.path.normpath(root))


def find(roots):
    """every file that may be a log: (path, None), and archives' entries: (zip, member); and whose each file is"""
    jobs, notes, sources = [], [], {}
    for root in roots:
        for d, _, files in os.walk(root):
            for name in files:
                p = os.path.join(d, name)
                sources[p] = source_of(root, p)
                ext = os.path.splitext(name)[1].lower()
                if ext in SKIP_EXT:
                    continue
                if ext == ".zip":
                    try:
                        with zipfile.ZipFile(p) as z:
                            members = [i.filename for i in z.infolist() if not i.is_dir()
                                       and os.path.splitext(i.filename)[1].lower() not in SKIP_EXT]
                        # an archive inside an archive isn't read: reported
                        for m in [m for m in members if m.lower().endswith(".zip")]:
                            notes.append({"path": p, "member": m, "status": "not read", "why": "an archive inside an archive: unpack it to read it"})
                        jobs += [(p, m) for m in members if not m.lower().endswith(".zip")]
                    except (zipfile.BadZipFile, OSError) as e:
                        notes.append({"path": p, "member": "", "status": "broken", "why": f"archive won't open: {e}"[:120]})
                    continue
                if ext in (".zevtc", ".evtc"):
                    jobs.append((p, None))
                    continue
                # anything else: a log only when it starts like one
                try:
                    with open(p, "rb") as f:
                        magic = f.read(4)
                except OSError:
                    continue
                if magic == b"EVTC" or magic[:2] == b"PK":
                    jobs.append((p, None))
                elif os.path.getsize(p) < 64 or magic == b"\0\0\0\0":
                    notes.append({"path": p, "member": "", "status": "broken", "why": f"empty ({os.path.getsize(p)} bytes)"})
    return jobs, notes, sources


def overlap(a, b):
    return max(0.0, min(a["to"], b["to"]) - max(a["from"], b["from"]))


def same_squad(a, b):
    sa, sb = set(a["accounts"].split()), set(b["accounts"].split())
    if not sa or not sb:
        return False
    return len(sa & sb) >= SAME_SQUAD * min(len(sa), len(sb))


def same_instance(a, b):
    if a["map"] != b["map"]:
        return False
    if a["socket"] and b["socket"] and a["socket"] != b["socket"]:
        return False
    if a["instance"] and b["instance"]:
        return abs(a["instance"] - b["instance"]) <= SAME_INSTANCE_S
    return True  # an old log with no instance event: map, squad and time decide


def dedup(rows):
    logs = [r for r in rows if r["status"] == "log"]
    # 1. the same bytes: keep the loose file over an archive's copy, then the first path
    by_hash = {}
    for r in sorted(logs, key=lambda r: (r["member"] != "", r["path"], r["member"])):
        if r["sha1"] in by_hash:
            first = by_hash[r["sha1"]]
            r["status"], r["why"] = "same file", where(first)
        else:
            by_hash[r["sha1"]] = r
    # 2. the same fight from another recorder (or another cut of the same recording)
    uniq = sorted(by_hash.values(), key=lambda r: r["from"])
    kept, by_hour = [], {}
    hours = lambda r: range(int(r["from"] // 3600), int(min(r["to"], r["from"] + MAX_SPAN_MS / 1000) // 3600) + 1)
    for r in sorted(uniq, key=lambda r: (-r["seconds"], -r["events"])):
        best, most = None, 0.0
        near = {id(k): k for h in hours(r) for k in by_hour.get(h, [])}
        for k in near.values():
            o = overlap(r, k)
            if o > most and same_instance(r, k) and same_squad(r, k):
                best, most = k, o
        if best and most >= 0.5 * max(r["seconds"], 1.0):
            r["status"] = "same fight"
            r["why"] = f"{round(100 * most / max(r['seconds'], 1.0))}% inside {where(best)}" + ("" if r["recorder"] != best["recorder"] else " (same recorder)")
        else:
            if best:
                r["why"] = f"overlaps {where(best)} by {round(most)} s"
            r["status"] = "kept"
            kept.append(r)
            for h in hours(r):
                by_hour.setdefault(h, []).append(r)
    return sorted(kept, key=lambda r: r["from"])


def where(r):
    return rel(r["path"]) + (f"!{r['member']}" if r["member"] else "")


def unzip_kept(kept):
    """where tools that take a path read each kept log: the file, or for an archive's entry "<archive>/<entry>", which
    the C++ reader opens inside the archive (Evtc::FromArchive): nothing unpacked (a 33 GB zip)"""
    for r in kept:
        r["file"] = os.path.join(r["path"], *r["member"].split("/")) if r["member"] else r["path"]


def raw_named(kept):
    """a raw log with no extension: the C++ reader wants .evtc or .zevtc, so a copy named .evtc"""
    for r in kept:
        ext = os.path.splitext(r["file"])[1].lower()
        if ext in (".zevtc", ".evtc") or r["member"]:
            continue
        dest = os.path.join(OUT, "raw", os.path.basename(r["file"]) + (".zevtc" if open(r["file"], "rb").read(2) == b"PK" else ".evtc"))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if not os.path.exists(dest):
            with open(r["file"], "rb") as s, open(dest, "wb") as d:
                d.write(s.read())
        r["file"] = dest


def default_roots():
    """the folders to read when none are given: data/log_sources.txt (local, one folder a line, # for notes), else data/logs"""
    listed = []
    if os.path.exists(SOURCES):
        with open(SOURCES, encoding="utf-8") as f:
            listed = [ln.strip() for ln in f if ln.strip() and not ln.strip().startswith("#")]
    missing = [p for p in listed if not os.path.isdir(p)]
    if missing:
        sys.exit("not found (data/log_sources.txt): " + "; ".join(missing))
    return [os.path.abspath(p) for p in listed] or [os.path.join(ROOT, "data", "logs")]


def main(roots):
    t0 = time.time()
    jobs, notes, sources = find(roots)
    print(f"{len(jobs)} files and archive entries to read under {', '.join(rel(r) for r in roots)}")
    # what was read before (data/import/cache.json: a file's or archive's path, size and time, and the entry): read once
    cache = {}
    if os.path.exists(CACHE):
        with open(CACHE, encoding="utf-8") as f:
            cache = json.load(f)
    stat = {}
    def key(job):
        path, member = job
        if path not in stat:
            st = os.stat(path)
            stat[path] = f"{path}|{st.st_size}|{int(st.st_mtime)}"
        return stat[path] + "|" + (member or "")
    todo = [j for j in jobs if key(j) not in cache]
    rows = [dict(cache[key(j)], path=j[0], member=j[1] or "") for j in jobs if key(j) in cache]
    print(f"  {len(rows)} read before, {len(todo)} to read")
    with multiprocessing.Pool(max(1, (os.cpu_count() or 2) - 1)) as pool:
        for i, r in enumerate(pool.imap_unordered(read_one, todo, chunksize=8)):
            rows.append(r)
            cache[key((r["path"], r["member"] or None))] = {k: v for k, v in r.items() if k not in ("path", "member")}
            if (i + 1) % 1000 == 0:
                print(f"  {i + 1} read, {time.time() - t0:.0f} s")
    os.makedirs(OUT, exist_ok=True)
    with open(CACHE, "w", encoding="utf-8") as f:
        json.dump(cache, f)
    rows += notes
    for r in rows:
        r["source"] = sources.get(r["path"], "")
    kept = dedup(rows)
    unzip_kept(kept)
    raw_named(kept)

    # nights in server time: a new night after 3 h without a fight (with one squad's logs; another squad's
    # night at the same hours would join it, which only matters for grouping, not for the fights)
    night, last = -1, None
    for r in kept:
        if last is None or r["from"] - last > NIGHT_GAP_S:
            night += 1
        r["night"] = night
        last = max(last or 0, r["to"])

    os.makedirs(OUT, exist_ok=True)
    fields = ["path", "member", "status", "why", "bytes", "sha1", "arc", "gw2", "species", "map", "shard", "socket", "instance",
              "recorder", "source", "squad", "accounts", "events", "server_start", "server_end", "local_start", "from", "to", "seconds", "night", "file"]
    with open(os.path.join(OUT, "manifest.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        for r in sorted(rows, key=lambda r: (r.get("from") or 0, r["path"], r["member"])):
            w.writerow({k: (rel(v) if k in ("path", "file") and v else v) for k, v in r.items()})
    with open(os.path.join(OUT, "logs.txt"), "w", encoding="utf-8") as f:
        for r in kept:
            f.write(f"{os.path.abspath(r['file'])}\t{r['night']}\n")

    count = {}
    for r in rows:
        count[r["status"]] = count.get(r["status"], 0) + 1
    recorders = {r["recorder"] for r in kept}
    print(f"done in {time.time() - t0:.0f} s")
    print(f"  kept {count.get('kept', 0)} fights over {night + 1} nights, from {len(recorders)} recorder(s)")
    print(f"  same file twice: {count.get('same file', 0)}; same fight from another log: {count.get('same fight', 0)}; broken or empty: {count.get('broken', 0)}")
    for r in rows:
        if r["status"] in ("broken", "not read"):
            print(f"    {r['status']}: {where(r)}: {r['why']}")
    part = [r for r in kept if r["why"].startswith("overlaps")]
    if part:
        print(f"  kept but partly overlapping another kept log: {len(part)}")
    by = {}
    for r in kept:
        by[r["source"]] = by.get(r["source"], 0) + 1
    print("  kept per player folder: " + ", ".join(f"{k} {v}" for k, v in sorted(by.items(), key=lambda kv: -kv[1])))
    print(f"  -> {rel(os.path.join(OUT, 'manifest.csv'))}, {rel(os.path.join(OUT, 'logs.txt'))}")


if __name__ == "__main__":
    argv = sys.argv[1:]
    if "--out" in argv:
        i = argv.index("--out")
        OUT = os.path.abspath(argv[i + 1])
        del argv[i:i + 2]
    main([os.path.abspath(a) for a in argv] or default_roots())
