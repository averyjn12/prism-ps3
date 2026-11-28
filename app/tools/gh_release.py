"""GitHub releases for averyjn12/prism-ps3. The token comes in GH_TOKEN (from the credential manager) and is never
printed.   python gh_release.py list | create <ver> <commit sha>"""
import json
import os
import sys
import urllib.request

API = "https://api.github.com/repos/averyjn12/prism-ps3"
OUT = r"C:\path\to\PS3 software\lr\app\out"
TOK = os.environ.get("GH_TOKEN", "")
if not TOK:
    sys.exit("no token")


def call(url, data=None, ctype="application/json", method=None):
    req = urllib.request.Request(url, data=data, method=method or ("POST" if data is not None else "GET"))
    req.add_header("Authorization", "token " + TOK)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "prism-release")
    if data is not None:
        req.add_header("Content-Type", ctype)
    with urllib.request.urlopen(req, timeout=300) as r:
        return json.loads(r.read() or b"null")


if sys.argv[1] == "list":
    for r in call(API + "/releases"):
        print("%s | tag %s | draft %s | pre %s | target %s | assets: %s" % (r["name"], r["tag_name"], r["draft"], r["prerelease"],
              r["target_commitish"], ", ".join(a["name"] for a in r["assets"])))
    for t in call(API + "/tags")[:5]:
        print("tag %s -> %s" % (t["name"], t["commit"]["sha"][:7]))
elif sys.argv[1] == "create":
    ver, sha = sys.argv[2], sys.argv[3]
    notes = open(os.path.join(OUT, "RELEASE_NOTES_%s.md" % ver), encoding="utf-8").read()
    assert "(what is new goes here)" not in notes
    # 1.00 on (Bananafuzzbuddy, 2026-11-27): every release carries the thanks block (app/repo/THANKS.md, added by mk_release.py)
    thanks = open(os.path.join(OUT, "..", "repo", "THANKS.md"), encoding="utf-8").read().strip()
    if thanks not in notes:
        sys.exit("the release notes do not carry app/repo/THANKS.md: run mk_release.py again")
    # 1.00 is a full release, not a pre-release (Bananafuzzbuddy, 2026-11-27); a draft until Bananafuzzbuddy publishes it, as always
    pre = float(ver) < 1.0
    body = {"tag_name": "v" + ver, "target_commitish": sha, "name": "Prism " + ver, "body": notes, "draft": True, "prerelease": pre}
    r = call(API + "/releases", json.dumps(body).encode())
    print("created draft %s, tag %s at %s" % (r["id"], r["tag_name"], sha[:7]))
    z = os.path.join(OUT, "Prism_%s.zip" % ver)
    up = "https://uploads.github.com/repos/averyjn12/prism-ps3/releases/%d/assets?name=Prism_%s.zip" % (r["id"], ver)
    a = call(up, open(z, "rb").read(), "application/zip")
    print("asset %s: %d bytes (local %d), state %s" % (a["name"], a["size"], os.path.getsize(z), a["state"]))
