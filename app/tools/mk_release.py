"""Make the download for a release: out/Prism_<version>.zip, holding the terms FIRST, then the .pkg.
    python3 mk_release.py <app dir>
The zip is what goes on the GitHub release page (never the bare .pkg), so that the terms travel with
every copy. It also writes out/RELEASE_NOTES_<version>.md: the text for the release page, which
starts with the terms and the sentence that downloading means agreeing to them."""
import os
import re
import sys
import zipfile

app = sys.argv[1]
ver = re.search(r'APP_VERSION\s+"([0-9.]+)', open(os.path.join(app, "src", "version.h")).read()).group(1)
terms = open(os.path.join(app, "TERMS.md"), encoding="utf-8").read()
pkg = os.path.join(app, "out", "prism_app_%s.pkg" % ver)
if not os.path.exists(pkg):
    sys.exit("no %s: build that version first" % pkg)
tv = re.search(r"terms of use \(version (\d+)\)", terms).group(1)
code = re.search(r"#define TERMS_VER\s+(\d+)", open(os.path.join(app, "src", "help.h")).read()).group(1)
if tv != code:
    sys.exit("TERMS.md is version %s but the app asks for version %s (src/help.h): make them agree" % (tv, code))

out = os.path.join(app, "out", "Prism_%s.zip" % ver)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    # "0 - " sorts the terms above everything else in the zip
    z.writestr("0 - READ FIRST - TERMS.txt", terms)
    z.writestr("1 - HOW TO INSTALL.txt", open(os.path.join(app, "INSTALL.md"), encoding="utf-8").read())
    z.write(pkg, "Prism_%s.pkg" % ver)
    # GPL-3.0: the licence and the credits go with every copy; the source is the repository
    z.writestr("LICENSE.txt", open(os.path.join(app, "LICENSE"), encoding="utf-8").read())
    z.writestr("NOTICE.txt", open(os.path.join(app, "NOTICE.md"), encoding="utf-8").read())

notes = os.path.join(app, "out", "RELEASE_NOTES_%s.md" % ver)
# 2026-11-27: a second run kept wiping the "what is new" text already written for this version: it is kept now
new = "(what is new goes here)"
if os.path.exists(notes):
    old = open(notes, encoding="utf-8").read()
    head = "## Prism %s\n\n" % ver
    if head in old and new not in old:
        a = old.index(head) + len(head)
        b = old.find("\n## ", a)
        new = old[a:b if b >= 0 else len(old)].strip()
# 1.00 on (Bananafuzzbuddy, 2026-11-27): every release thanks the testers, from ONE file (gh_release.py refuses notes without it)
thanks = open(os.path.join(app, "repo", "THANKS.md"), encoding="utf-8").read().strip()
with open(notes, "w", encoding="utf-8", newline="\n") as f:
    f.write("## Read this before you download\n\n")
    f.write("**By downloading `Prism_%s.zip` you agree to the terms of use below. "
            "If you do not agree, do not download it.**\n\n" % ver)
    f.write("\n".join(terms.splitlines()[5:]).strip() + "\n\n")
    f.write("## Prism %s\n\n%s\n\n" % (ver, new))
    f.write(thanks + "\n\n")
    f.write("## Licence and source\n\nGPL-3.0. The source of this exact version is this repository at the tag of the same "
            "name; `LICENSE.txt` and `NOTICE.txt` are in the zip.\n\n")
    f.write("## Installing\n\nUnzip, read `0 - READ FIRST - TERMS.txt`, copy `Prism_%s.pkg` to the console and "
            "install it with Package Manager. The first start shows the terms again and waits for X.\n" % ver)
print("made %s (%d bytes) and %s" % (out, os.path.getsize(out), notes))
