"""Assemble the publishable repository from the working folders.
    python export_repo.py <lr dir> <repo dir>
The working folders hold things that must never be published (a Sony emulator file and key files in
the Cobra tree, game covers, console logs, build output). So the repository is a COPY made from the
lists below, and the script refuses to finish if anything forbidden got in. It never deletes .git.
The repository keeps lr's layout (app/, tree/, launcher/ui, out/) so that app/build_app.sh finds
the helper and the card-reader source where it expects them."""
import os
import re
import shutil
import sys

lr, repo = sys.argv[1], sys.argv[2]
SRC_EXT = (".c", ".h", ".S", ".s", ".ld", ".py", ".sh", ".md", ".txt", ".csv", ".bat", ".xml")


def want_source(name):
    return name.endswith(SRC_EXT) or name in ("Makefile",)


def copy_tree(rel, keep=want_source, skip_dirs=()):
    n = 0
    for root, dirs, files in os.walk(os.path.join(lr, rel)):
        dirs[:] = [d for d in dirs if d not in skip_dirs and d != "__pycache__"]
        for f in files:
            if not keep(f):
                continue
            src = os.path.join(root, f)
            dst = os.path.join(repo, os.path.relpath(src, lr))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)
            n += 1
    return n


def copy_file(rel, to=None):
    dst = os.path.join(repo, to or rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(os.path.join(lr, rel), dst)


# start clean, but keep the repository's own .git
os.makedirs(repo, exist_ok=True)
for e in os.listdir(repo):
    if e == ".git":
        continue
    p = os.path.join(repo, e)
    shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)

count = 0
# the app
count += copy_tree("app/src")
# (expect_probe.*: a one-off tool tied to one PC's folders, left out; edat.py: it holds Sony's EDAT keys, never published)
LEFT_OUT_TOOLS = ("edat.py",)
count += copy_tree("app/tools", keep=lambda f: (want_source(f) or f == "font_ps3_atlas.png") and not f.startswith("expect_probe")
                   and f not in LEFT_OUT_TOOLS)
count += copy_tree("app/db")
count += copy_tree("app/boot")
count += copy_tree("app/sprx")
count += copy_tree("app/ref")
# the PC tests: code and scripts only (no screenshots, no art file with game covers)
count += copy_tree("app/host", skip_dirs=("shots", "shots_before", "shots_v15", "shots_v22", "shots_v27", "shots_v28", "shots_v46", "sound"))
for f in ("app/build_app.sh", "app/README.md", "app/setup/carrier.iso", "app/setup/carrier.png", "app/setup/tinytees.iso",
          # v0.57 / v0.59: what build_app.sh packages besides (the cover SOURCES list, no covers; Tiny Tees' own cover)
          "app/setup/cover_sources.txt", "app/setup/tinytees_cover.png", "app/setup/tinytees_cover.rgba",
          # v0.57: BearSSL's build (the library itself is fetched from bearssl.org, not copied)
          "third_party/build_bearssl.sh",
          # v0.55: the webMAN shortcut templates build_app.sh makes
          "app_a01/tools/mk_templates.py"):
    copy_file(f)
    count += 1
# the PS2 helper: only what its build uses (gxemu). NOT netemu (key files), NOT ps2netemu_gen (a Sony file)
for d in ("tree/debug", "tree/lv1", "tree/lv2", "tree/ps2emu", "tree/ps2emu_stage2/common", "tree/ps2emu_stage2/gxemu", "tree/ps2emu_stage2/prism"):
    count += copy_tree(d)
for f in ("build.sh", "port_tree.sh", "apply_prism.py", "out/lr_g5.bin", "out/lr_g5.tested"):
    copy_file(f)
    count += 1
# the PS2-side launcher image's source
count += copy_tree("launcher/ui", keep=lambda f: want_source(f) or f.endswith((".ttf", ".png")))
# the front page and the papers
for src, dst in (("app/repo/README.md", "README.md"), ("app/LICENSE", "LICENSE"), ("app/NOTICE.md", "NOTICE.md"),
                 ("app/TERMS.md", "TERMS.md"), ("app/INSTALL.md", "INSTALL.md")):
    copy_file(src, dst)
    count += 1
with open(os.path.join(repo, ".gitattributes"), "w", newline="\n") as f:
    f.write("* -text\n")
with open(os.path.join(repo, ".gitignore"), "w", newline="\n") as f:
    f.write("app/build/\napp/out/\napp/logs/\n*.o\n*.elf\n*.map\n__pycache__/\n")

# ---- the published copy is anonymous: the owner's name becomes a handle, PC paths become generic ----
# (the working folders keep the real notes; only the copy is changed)
PARTS = ("A" + "very", "ray" + "ne")	# put together here so that this file does not hold them itself
U = "Us" + "ers"
SWAPS = (
    ("C:\\" + U + "\\" + PARTS[1], "C:\\path\\to"),
    ("C:/" + U + "/" + PARTS[1], "C:/path/to"),
    ("/mnt/c/" + U + "/" + PARTS[1], "/path/to"),
    ("/c/" + U + "/" + PARTS[1], "/path/to"),
    (PARTS[0], "Bananafuzzbuddy"),
    # the bench console's address on the home network (used only by the helper's debug builds)
    ("0xC0A8" + "28F0", "0xC0A80002"),
    ("192.168." + "40.240, the mule's static address", "192.168.0.2: put the console's own address here"),
    ("192.168." + "40.240", "192.168.0.2"),
)
LAN = ("192.168." + "40.", "C0A8" + "28")


def is_data(rel):
    """the title lists and the licence text: other people's words, left exactly as they are"""
    return rel.startswith("app/db/") and rel.endswith(".csv") or rel == "LICENSE"


swapped = 0
for root, dirs, files in os.walk(repo):
    if ".git" in dirs:
        dirs.remove(".git")
    for f in files:
        if not (f.endswith(SRC_EXT) or f == "Makefile"):
            continue
        if is_data(os.path.relpath(os.path.join(root, f), repo).replace("\\", "/")):
            continue
        p = os.path.join(root, f)
        raw = open(p, "rb").read()
        new = raw
        for a, b in SWAPS:
            new = new.replace(a.encode(), b.encode())
        if new != raw:
            open(p, "wb").write(new)
            swapped += 1
print("%d files had a name or a PC path replaced" % swapped)

# ---- the donation link (owner, 2026-11-24): the ONE place a real name may show, by the owner's choice ----
# It is kept in app/repo/support_link.txt (not copied), put where the front page says {{SUPPORT_LINK}} and
# into .github/FUNDING.yml (GitHub's Sponsor button). It goes in AFTER the name swap, which would break it.
SUPPORT = ""
sl = os.path.join(lr, "app", "repo", "support_link.txt")
if os.path.exists(sl):
    SUPPORT = open(sl).read().strip()
    if not re.fullmatch(r"https://paypal\.me/[A-Za-z0-9]{1,20}", SUPPORT):
        sys.exit("support_link.txt does not hold a plain paypal.me link")
front = os.path.join(repo, "README.md")
page = open(front, "rb").read()
if SUPPORT:
    if page.count(b"{{SUPPORT_LINK}}") < 1:
        sys.exit("the front page has no {{SUPPORT_LINK}} to fill")
    open(front, "wb").write(page.replace(b"{{SUPPORT_LINK}}", SUPPORT.encode()))
    os.makedirs(os.path.join(repo, ".github"), exist_ok=True)
    with open(os.path.join(repo, ".github", "FUNDING.yml"), "w", newline="\n") as f:
        f.write('custom: ["%s"]\n' % SUPPORT)
    print("donation link written to the front page and .github/FUNDING.yml")
elif b"{{SUPPORT_LINK}}" in page:
    sys.exit("the front page wants a donation link and app/repo/support_link.txt is missing")

# ---- refuse anything that must not be published ----
bad = []
# key VALUES, not only key-file names: every long hex constant in the tools left out for holding keys (v0.66)
KEYS = set()
for t in LEFT_OUT_TOOLS:
    kp = os.path.join(lr, "app", "tools", t)
    if os.path.exists(kp):
        KEYS |= {m.lower() for m in re.findall(rb"[0-9A-Fa-f]{32,}", open(kp, "rb").read())}
if not KEYS and os.path.exists(os.path.join(lr, "app", "tools", "edat.py")):
    sys.exit("edat.py is there but no key constant was found in it: the key check would be blind")
print("%d key constant(s) to refuse" % len(KEYS))
for root, dirs, files in os.walk(repo):
    if ".git" in dirs:
        dirs.remove(".git")
    for f in files:
        p = os.path.join(root, f)
        rel = os.path.relpath(p, repo).replace("\\", "/")
        low = f.lower()
        size = os.path.getsize(p)
        if low.endswith((".self", ".sprx", ".pkg", ".rap", ".rif", ".edat", ".dat", ".log", ".ppm", ".wav", ".vm1", ".vm2", ".o", ".elf")):
            bad.append((rel, "a kind of file that is never published"))
        if low in ("keys", "rif-key", "npdrm-const", "curves", "ldr_curves", "vsh_curves"):
            bad.append((rel, "a key file"))
        if "netemu" in rel or "hwemu" in rel:
            bad.append((rel, "from a folder that is left out"))
        if size > 3 * 1024 * 1024:
            bad.append((rel, "over 3 MB (%d)" % size))
        if f in LEFT_OUT_TOOLS:
            bad.append((rel, "a tool that holds keys"))
        if low.endswith(SRC_EXT) or low.endswith(".rgba"):
            t0 = open(p, "rb").read().lower()
            if any(k in t0 for k in KEYS):
                bad.append((rel, "contains a key value"))
        if low.endswith(SRC_EXT):
            t = open(p, "rb").read()
            # (the needles are put together here so that this file does not hold them itself)
            for needle in (b"risty" + b"benefits", b"snord" + b"bye"):
                if needle in t:
                    bad.append((rel, "contains a private address"))
            # (the account name in the repository's own address is the one place the name may stay)
            account = (PARTS[0].lower() + "jn12").encode()
            if not is_data(rel):
                seen = t.lower().replace(account, b"")
                if SUPPORT and rel in ("README.md", ".github/FUNDING.yml"):
                    seen = seen.replace(SUPPORT.lower().encode(), b"")	# the donation link, and only there
                if PARTS[0].lower().encode() in seen:
                    bad.append((rel, "still names the owner"))
                if PARTS[1].encode() in t.lower() or (b"c:\\us" + b"ers") in t.lower() or (b"/us" + b"ers/") in t.lower():
                    bad.append((rel, "still holds a PC user path"))
                if LAN[0].encode() in t or LAN[1].encode() in t.upper():
                    bad.append((rel, "still holds the home network address"))
            # a console's own identifiers: 32 hex digits after the word, or a hardware address
            if re.search(rb"(IDPS|PSID).{0,24}[0-9A-Fa-f]{32}", t) or re.search(rb"([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", t):
                bad.append((rel, "contains a console identifier or a hardware address"))
print("%d files copied to %s" % (count, repo))
if bad:
    print("REFUSED, %d problem(s):" % len(bad))
    for b in bad:
        print("  ", b)
    sys.exit(1)
print("nothing forbidden found")
