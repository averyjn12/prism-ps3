"""Make USRDIR/titles.txt for the app (src/titles.c): "<ID>|<title>" per line, sorted by ID bytes,
plain ASCII.   python3 mk_titles.py <db dir> <out file>
Sources (app/db/SOURCES.txt says where they come from):
  TitlesDB_PS2.csv            ID|Title|Publisher|...        (PSBBN Definitive Project, CosmicScale)
  TitlesDB_PS1.csv            the same form, the same project (v0.47: the list used before came from
                              a repository with no licence)
The first title seen for an ID is kept."""
import os
import re
import sys
import unicodedata

db, out = sys.argv[1], sys.argv[2]
ID = re.compile(r"^[A-Z]{4}_\d{3}\.\d{2}$")


def plain(t):
    t = t.replace("^!", "!")
    t = unicodedata.normalize("NFKD", t)
    t = "".join(c for c in t if 32 <= ord(c) < 127)
    return " ".join(t.replace("|", " ").split())


titles = {}
with open(os.path.join(db, "TitlesDB_PS2.csv"), encoding="utf-8", errors="replace") as f:
    for ln in f:
        p = ln.rstrip("\r\n").split("|")
        if len(p) >= 2 and ID.match(p[0]) and plain(p[1]):
            titles.setdefault(p[0], plain(p[1]))
n2 = len(titles)
with open(os.path.join(db, "TitlesDB_PS1.csv"), encoding="utf-8", errors="replace") as f:
    for ln in f:
        p = ln.rstrip("\r\n").split("|")
        if len(p) >= 2 and ID.match(p[0]) and plain(p[1]):
            titles.setdefault(p[0], plain(p[1]))
with open(out, "wb") as f:
    for k in sorted(titles, key=lambda s: s.encode("ascii")):
        f.write(("%s|%s\n" % (k, titles[k])).encode("ascii"))
print("titles: %d PS2 + %d PS1 = %d lines" % (n2, len(titles) - n2, len(titles)))
