"""PC test of src/busy.c, the loading-screen ticker (Prism 1.00), with real threads; run in WSL:

    python3 host/busy_test.py

A COPY of busy.c is built beside host/busy_shim (LV2 threads and mutexes as pthreads, with LV2's refusals counted)
and host/busy_test.c (the drawing stand-in: one frame = one thread). The copy's mftb (the PPU's clock) is replaced by
the host's monotonic clock at the same 79.8 MHz. The unplanted copy must pass first; then each planted bug must make
the test FAIL or hang (a build failure is BROKEN, never a catch)."""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
W = "/tmp/prism_busy"
MFTB = '__asm__ volatile("mftb %0" : "=r"(t));'
HOST_TB = ("{ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); "
           "t = (u64)ts.tv_sec * 79800000ULL + (u64)ts.tv_nsec * 798ULL / 10000ULL; }")

PLANTS = [
    ("the take-back does not lock", "	sysMutexLock(mx, 0);\n	gx_lent = 0;", "	gx_lent = 0;"),
    ("the ticker not told apart from the main thread", "	return a >= tk_lo && a < tk_hi;", "	return 0;"),
    ("lending without unlocking", "	gx_lent = 1;\n	sysMutexUnlock(mx);", "	gx_lent = 1;"),
    ("the ticker draws without the mutex", "		if (!gx_lent || sysMutexTryLock(mx) != 0)\n			continue;",
     "		if (!gx_lent)\n			continue;"),
    ("the screen copied without taking the drawing back", "	take_back();\n	if (!ok)\n		return;\n	s_has_game",
     "	if (!ok)\n		return;\n	s_has_game"),
    ("the main thread's lock released at start", "	if (sysMutexLock(mx, 0) != 0)\n	{\n		sysMutexDestroy(mx);\n		return -3;\n	}", ""),
]


def build(code):
    shutil.rmtree(W, ignore_errors=True)
    shutil.copytree(os.path.join(HERE, "busy_shim"), W)
    shutil.copy(os.path.join(SRC, "busy.h"), W)
    shutil.copy(os.path.join(HERE, "busy_test.c"), W)
    assert code.count(MFTB) == 1, "busy.c's mftb line is not there exactly once"
    code = code.replace(MFTB, HOST_TB).replace("#include <stdint.h>", "#include <stdint.h>\n#include <time.h>", 1)
    open(W + "/busy.c", "w").write(code)
    r = subprocess.run(["gcc", "-O2", "-Wall", "-pthread", "-I" + W, W + "/busy_test.c", W + "/busy.c", "-o", W + "/t"],
                       capture_output=True, text=True)
    return r.stderr if r.returncode else None


def run():
    try:
        r = subprocess.run([W + "/t"], capture_output=True, text=True, timeout=90)
        return r.returncode, r.stdout
    except subprocess.TimeoutExpired:
        return 3, "FAIL  hung past 90 s"


def main():
    src = open(os.path.join(SRC, "busy.c")).read()
    err = build(src)
    if err:
        print("BROKEN: the unplanted build failed\n" + err)
        return 2
    rc, out = run()
    print(out, end="")
    if rc != 0:
        print("the unplanted code FAILED: the planted bugs are not run")
        return 1
    killed = broken = 0
    for what, old, new in PLANTS:
        if src.count(old) != 1:
            print("BROKEN  plant %r: its text is not in busy.c exactly once" % what)
            broken += 1
            continue
        err = build(src.replace(old, new))
        if err:
            print("BROKEN  plant %r: did not compile" % what)
            broken += 1
            continue
        rc, out = run()
        fl = [l for l in out.splitlines() if l.startswith("FAIL")]
        if rc != 0 and fl:
            killed += 1
            print("killed  %s: %s" % (what, fl[0][6:90]))
        elif rc != 0:
            print("BROKEN  plant %r: exit %d with no FAIL line (a crash is not a catch)" % (what, rc))
            broken += 1
        else:
            print("SURVIVED  %s" % what)
    print("planted bugs: %d of %d killed, %d broken" % (killed, len(PLANTS), broken))
    return 0 if killed == len(PLANTS) else 1


if __name__ == "__main__":
    sys.exit(main())
