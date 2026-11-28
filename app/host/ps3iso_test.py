#!/usr/bin/env python3
# REPORT (2026-11-27, ps3iso_core.c/.h + this harness)
# API (ps3iso_core.h): ps3iso_build_plan(const ps3iso_src*, ps3iso_build**, uint64_t *total) -> pass 1, exact size;
#   ps3iso_build_emit(b, sink(ctx,buf,len<=256K), ctx) -> pass 2, in order; ps3iso_build_free(b); title_id/title/default_name/msg getters.
#   ps3iso_src = {ctx, list(ctx,dir,add,u), read(ctx,path,off,buf,len), root_mtime, creation_time, seed}.
#   ps3iso_unpack(const ps3iso_rd*{read(ctx,off,buf,len),size_hint}, const ps3iso_out*{mkdir,create(path,size,&h),write,close}, ps3iso_unpack_stats*).
#   No globals; two plans can live at once (tested). Memory: tables by tree + one header buffer + 256 KiB chunk.
# JUDGE RESULTS (all baseline PASS):
#   MAIN-build: BLUS30386 vs makeps3iso ISO: 3,464,851,456 bytes compared, 480 masked, 0 other differences;
#     plan size == emitted == reference. Masks: 0x840+0x1B0 and 0x9F0+0x10 (makeps3iso get_rand; ours xorshift),
#     0x832D+0x10 and 0x8B2D+0x10 (PVD/SVD creation date = build time). Dir/file dates match exactly (callbacks
#     pass the same local/UTC-shifted times makeps3iso uses). 57-65 s (disk bound).
#   MAIN-unpack: BLUS30386.iso -> folder: 568 paths (18 dirs, 550 files, 3,464,477,731 bytes), same path set,
#     every file sha256-identical, counts equal. 66-69 s (+31 s once for the source sha256).
#   SYN: generated 308-dir/1700-file tree (overflowing dirs, unicode, empties, PS3_UPDATE), 42 MB: byte-identical to
#     makeps3iso (same masks), unpack of its ISO == tree; 4 KiB-extent roundtrip (multi-extent, emoji names) OK,
#     also under ASAN+UBSAN. 0.5 s. BIG: 4.5 GB virtual file via default 0xFFFFF800 multi-extent, build+unpack
#     verified, 26 s. refuse: error codes + two builds at once.
# PLANTED BUGS: 23 (13 builder, 10 unpacker), 23 KILLED, 0 survived, 0 broken. 7 of them pass MAIN (BLUS names are all
#   upper case, ASCII, root dirs same size) and are killed by SYN: B4 Joliet case, B13 Joliet root size, U1, U9, U10.
#   Whole run (baseline + 23 mutants) about 25 min.
# DEVIATIONS FROM makeps3iso (documented in the header): entries keep callback order; deterministic "random" bytes;
#   names > 213 bytes / 105 UTF-16 units are refused (makeps3iso writes >255-byte records = corrupt); 4-byte UTF-8
#   names get correct surrogates (makeps3iso omits -0x10000); *.666NN files refused (E_SPLIT666); >4096 dirs allowed
#   (65535). Files > 4 GB: multi-extent like makeps3iso. PS3_UPDATE folder contents skipped like makeps3iso.
# NOT MATCHED: nothing in the judged output. Unpacker names in ISO-9660-only mode are upper case (no case info).
# Note: /tmp in the PSBBN distro is tmpfs and is wiped between wsl calls; the script rebuilds what it needs.
#
# ps3iso_test.py - judges + planted-bug (mutation) run for src/ps3iso_core.c
#
# Run inside WSL (the PSBBN distro) with gcc and python3:
#     wsl -d PSBBN -- python3 /mnt/c/.../lr/app/host/ps3iso_test.py [--no-mutants] [--only-mutants]
#
# FROZEN JUDGES (mutants may change only src/ps3iso_core.c, never this file):
#   MAIN-build   ps3iso_test judge-build  BLUS30386/  vs  BLUS30386.iso (makeps3iso output)
#                byte compare, only the MASKS in ps3iso_test.c may differ; plan size == emitted == reference
#   MAIN-unpack  ps3iso_test unpack BLUS30386.iso -> folder; same path set (empty dirs included),
#                every file sha256-identical, counts (dirs/files/bytes) equal to the folder's
#   SYN-*        a generated 300-dir / 1500-file tree (overflowing directories, unicode names,
#                empty dirs/files, PS3_UPDATE): judge-build vs a makeps3iso run on it, unpack of that
#                reference ISO vs the tree, and a roundtrip with 4 KiB extents (multi-extent)
#   BIG          4.5 GB virtual file through the default multi-extent path (no disk)
# Mutants run MAIN first and the SYN judges when MAIN lets them through.
# A mutant is KILLED only on a judge's own FAIL verdict; a crash / compile error / timeout is BROKEN.
import hashlib, json, os, random, re, shutil, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, '..', 'src'))
CORE = os.path.join(SRC, 'ps3iso_core.c')
TESTC = os.path.join(HERE, 'ps3iso_test.c')
GAME = os.environ.get('PS3ISO_GAME', '/path/to/ps3_rips/BLUS30386')
REFISO = os.environ.get('PS3ISO_REF', '/path/to/ps3_rips/BLUS30386.iso')
MAKEISO_C = '/path/to/PS3 software/lr/research/ps3iso-utils/makeps3iso.c'
MAKEISO = '/tmp/mkiso/makeps3iso'
WORK = '/tmp/ps3iso_work'                       # tmpfs: small things only
UNPACK = os.environ.get('PS3ISO_UNPACK', '/path/to/AppData/Local/Temp/ps3iso_unpack_judge')


def run(cmd, timeout=900):
    t = time.time()
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        out = p.stdout.decode('utf-8', 'replace')
        return p.returncode, out, time.time() - t
    except subprocess.TimeoutExpired:
        return -999, 'TIMEOUT', time.time() - t


def classify(rc, out):
    if rc == 0 and 'VERDICT: PASS' in out:
        return 'PASS'
    if 'VERDICT: FAIL' in out and rc > 0:
        return 'FAIL'
    return 'BROKEN'


def compile_(core, exe, extra=(), san=False):
    cmd = ['gcc', '-std=gnu99', '-O1' if san else '-O2', '-w', '-o', exe, TESTC, core, '-I' + SRC] + list(extra)
    if san:
        cmd += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
    rc, out, _ = run(cmd, 300)
    return rc == 0, out


# ---------------------------------------------------------------- manifests
def manifest(top, skip_update=False):
    m = {}
    for dp, dn, fn in os.walk(top):
        rel = os.path.relpath(dp, top).replace(os.sep, '/')
        rel = '' if rel == '.' else rel
        if rel:
            m[rel] = ('d',)
        in_update = skip_update and rel.split('/')[-1].endswith('PS3_UPDATE')
        for f in fn:
            if in_update:
                continue
            p = os.path.join(dp, f)
            h = hashlib.sha256()
            with open(p, 'rb') as fh:
                while True:
                    b = fh.read(1 << 20)
                    if not b:
                        break
                    h.update(b)
            m[(rel + '/' if rel else '') + f] = ('f', os.path.getsize(p), h.hexdigest())
    return m


def mdiff(exp, got):
    miss = sorted(set(exp) - set(got))
    extra = sorted(set(got) - set(exp))
    diff = sorted(k for k in exp if k in got and exp[k] != got[k])
    return miss, extra, diff


def counts(m):
    return (sum(1 for v in m.values() if v[0] == 'd'), sum(1 for v in m.values() if v[0] == 'f'),
            sum(v[1] for v in m.values() if v[0] == 'f'))


# ---------------------------------------------------------------- judges
def judge_build_main(exe):
    rc, out, dt = run([exe, 'judge-build', GAME, REFISO, '--first-fail'], 900)
    return classify(rc, out), out, dt


_src_manifest = None


def judge_unpack_main(exe):
    global _src_manifest
    if _src_manifest is None:
        t = time.time()
        _src_manifest = manifest(GAME)
        print('   (source manifest: %d paths, sha256 in %.0fs)' % (len(_src_manifest), time.time() - t))
    shutil.rmtree(UNPACK, ignore_errors=True)
    os.makedirs(UNPACK)
    try:
        rc, out, dt = run([exe, 'unpack', REFISO, UNPACK], 1500)
        v = classify(rc, out)
        if v != 'PASS':
            return v, out, dt
        got = manifest(UNPACK)
        miss, extra, diff = mdiff(_src_manifest, got)
        m = re.search(r'dirs=(\d+) files=(\d+) bytes=(\d+)', out)
        want = counts(_src_manifest)
        cnt_ok = bool(m) and (int(m.group(1)), int(m.group(2)), int(m.group(3))) == want
        extra_msg = ''
        if miss or extra or diff or not cnt_ok:
            extra_msg = 'VERDICT: FAIL tree compare: missing=%d extra=%d differing=%d counts_ok=%s (want dirs/files/bytes=%s) e.g. %s' % (
                len(miss), len(extra), len(diff), cnt_ok, want, (miss + extra + diff)[:3])
            return 'FAIL', out + extra_msg, dt
        return 'PASS', out + 'tree compare: %d paths identical (%d dirs, %d files, %d bytes), counts match' % (len(got), want[0], want[1], want[2]), dt
    finally:
        shutil.rmtree(UNPACK, ignore_errors=True)


# ---------------------------------------------------------------- synthetic tree
def make_sfo(title, tid):
    keys = b'TITLE\0TITLE_ID\0'
    t = title.encode() + b'\0'
    i = tid.encode() + b'\0'
    tl, il = 128, 16
    entries = struct.pack('<HHIII', 0, 0x204, len(t), tl, 0) + struct.pack('<HHIII', 6, 0x204, len(i), il, tl)
    kt = 0x14 + 32
    dt = (kt + len(keys) + 3) & ~3
    hdr = b'\0PSF' + struct.pack('<IIII', 0x101, kt, dt, 2)
    blob = hdr + entries + keys
    blob += b'\0' * (dt - len(blob))
    blob += t + b'\0' * (tl - len(t)) + i + b'\0' * (il - len(i))
    return blob


def gen_tree(top, seed=1234, nfiles=1500, extra_sizes=(), emoji=False):
    rnd = random.Random(seed)
    shutil.rmtree(top, ignore_errors=True)
    os.makedirs(top + '/PS3_GAME/USRDIR')
    with open(top + '/PS3_GAME/PARAM.SFO', 'wb') as f:
        f.write(make_sfo('Synthetic Game é Test', 'BLES01234'))
    with open(top + '/PS3_DISC.SFB', 'wb') as f:
        f.write(b'.SFB' + os.urandom(0) + bytes(rnd.randrange(256) for _ in range(100)))
    dirs = ['PS3_GAME/USRDIR']
    alpha = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_- .'
    uni = ['café', '日本語', 'Ünï', 'naïve', 'да']
    if emoji:   # makeps3iso writes a wrong surrogate pair for these, so only the roundtrip tree has them
        uni += ['x\U0001F600y', '\U00020BB7野']

    def nm(lo, hi):
        n = rnd.randint(lo, hi)
        s = ''.join(rnd.choice(alpha) for _ in range(n)).strip(' .') or 'q'
        return s

    for i in range(300):
        par = rnd.choice(dirs) if rnd.random() < 0.8 else rnd.choice(['', 'PS3_GAME', 'PS3_GAME/USRDIR'])
        if par.count('/') >= 6:
            par = 'PS3_GAME'
        name = nm(2, 30) + str(i)
        if i % 17 == 0:
            name = rnd.choice(uni) + str(i)
        p = (par + '/' if par else '') + name
        os.makedirs(top + '/' + p, exist_ok=True)
        dirs.append(p)
    os.makedirs(top + '/PS3_UPDATE')
    for k in range(3):
        open(top + '/PS3_UPDATE/UPD%d.PUP' % k, 'wb').write(os.urandom(5000))
    os.makedirs(top + '/PS3_GAME/USRDIR/LONG_PS3_UPDATE')
    open(top + '/PS3_GAME/USRDIR/LONG_PS3_UPDATE/x.bin', 'wb').write(b'skipped too')
    os.makedirs(top + '/EMPTY1/EMPTY2/EMPTY3')
    big = top + '/PS3_GAME/USRDIR/MANY'
    os.makedirs(big)
    files = []
    for i in range(260):                         # one directory that needs many sectors
        files.append('MANY/' + ('F%03d_' % i) + nm(50, 95))
    allfiles = []
    for p in files:
        allfiles.append(top + '/PS3_GAME/USRDIR/' + p)
    for i in range(nfiles):
        par = rnd.choice(dirs + [''])
        name = nm(1, 60)
        if i % 23 == 0:
            name = rnd.choice(uni) + '_' + nm(1, 10) + '.dat'
        if i % 7 == 0:
            name = name.lower()
        allfiles.append(top + '/' + (par + '/' if par else '') + name + ('.%s' % rnd.choice(['bin', 'DAT', 'sprx', 'edat', 'txt'])))
    for i, p in enumerate(allfiles):
        if os.path.exists(p):
            continue
        r = rnd.random()
        size = 0 if r < 0.05 else (rnd.randint(1, 3000) if r < 0.7 else (rnd.randint(3000, 60000) if r < 0.98 else rnd.randint(250000, 700000)))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(rnd.randbytes(size) if hasattr(rnd, 'randbytes') else bytes(rnd.randrange(256) for _ in range(size)))
    for k, size in enumerate(extra_sizes):
        with open(top + '/PS3_GAME/USRDIR/SIZE_%d.bin' % size, 'wb') as f:
            f.write(rnd.randbytes(size))
    # fixed mtimes (some in DST, some not), files first, directories deepest first
    base = 1325376000
    paths = []
    for dp, dn, fn in os.walk(top):
        for f in fn:
            paths.append(os.path.join(dp, f))
    for p in paths:
        t = base + rnd.randrange(0, 14 * 365 * 86400)
        os.utime(p, (t, t))
    dl = []
    for dp, dn, fn in os.walk(top):
        dl.append(dp)
    for p in sorted(dl, key=lambda x: -x.count('/')):
        t = base + rnd.randrange(0, 14 * 365 * 86400)
        os.utime(p, (t, t))


def ensure_makeiso():
    if not os.path.exists(MAKEISO):
        os.makedirs(os.path.dirname(MAKEISO), exist_ok=True)
        rc, out, _ = run(['gcc', '-O2', '-w', '-o', MAKEISO, MAKEISO_C], 300)
        assert rc == 0, out


SYN_TREE = WORK + '/syn_tree'
SYN_REF = WORK + '/syn_ref.iso'
SYN_RT = WORK + '/rt_tree'


def prep_syn():
    ensure_makeiso()
    os.makedirs(WORK, exist_ok=True)
    if not os.path.exists(SYN_REF):
        gen_tree(SYN_TREE)
        rc, out, _ = run([MAKEISO, '-p0', SYN_TREE, SYN_REF], 600)
        assert rc == 0 and os.path.exists(SYN_REF), out
    if not os.path.exists(WORK + '/rt.done'):
        gen_tree(SYN_RT, seed=99, nfiles=700, extra_sizes=(0, 1, 4095, 4096, 4097, 8192, 12288, 20001, 4096 * 5), emoji=True)
        open(WORK + '/rt.done', 'w').close()


_syn_manifest = None
_rt_manifest = None


def judge_syn(exe, exe_small=None, quick=False):
    """returns (verdict, text, secs)"""
    global _syn_manifest, _rt_manifest
    prep_syn()
    t0 = time.time()
    txt = []
    rc, out, _ = run([exe, 'judge-build', SYN_TREE, SYN_REF, '--first-fail'], 600)
    v = classify(rc, out)
    txt.append('SYN-build: ' + v)
    if v != 'PASS':
        return v, '\n'.join(txt) + '\n' + out[-600:], time.time() - t0
    if _syn_manifest is None:
        _syn_manifest = manifest(SYN_TREE, skip_update=True)
    od = WORK + '/syn_out'
    shutil.rmtree(od, ignore_errors=True)
    os.makedirs(od)
    rc, out, _ = run([exe, 'unpack', SYN_REF, od], 600)
    v = classify(rc, out)
    if v == 'PASS':
        got = manifest(od)
        miss, extra, diff = mdiff(_syn_manifest, got)
        m = re.search(r'dirs=(\d+) files=(\d+) bytes=(\d+)', out)
        want = counts(_syn_manifest)
        cnt_ok = bool(m) and tuple(int(x) for x in m.groups()) == want
        if miss or extra or diff or not cnt_ok:
            v = 'FAIL'
            out += 'tree compare: missing=%d extra=%d differing=%d counts_ok=%s e.g. %s' % (len(miss), len(extra), len(diff), cnt_ok, (miss + extra + diff)[:3])
    shutil.rmtree(od, ignore_errors=True)
    txt.append('SYN-unpack: ' + v)
    if v != 'PASS':
        return v, '\n'.join(txt) + '\n' + out[-600:], time.time() - t0
    if exe_small:
        isoo = WORK + '/rt.iso'
        rc, out, _ = run([exe_small, 'build', SYN_RT, isoo], 600)
        v = classify(rc, out)
        if v == 'PASS':
            od = WORK + '/rt_out'
            shutil.rmtree(od, ignore_errors=True)
            os.makedirs(od)
            rc, out, _ = run([exe_small, 'unpack', isoo, od], 600)
            v = classify(rc, out)
            if v == 'PASS':
                if _rt_manifest is None:
                    _rt_manifest = manifest(SYN_RT, skip_update=True)
                got = manifest(od)
                miss, extra, diff = mdiff(_rt_manifest, got)
                if miss or extra or diff:
                    v = 'FAIL'
                    out += 'roundtrip tree compare: missing=%d extra=%d differing=%d e.g. %s' % (len(miss), len(extra), len(diff), (miss + extra + diff)[:3])
            shutil.rmtree(od, ignore_errors=True)
            if os.path.exists(isoo):
                os.remove(isoo)
        txt.append('SYN-roundtrip(4KiB extents): ' + v)
        if v != 'PASS':
            return v, '\n'.join(txt) + '\n' + out[-600:], time.time() - t0
    return 'PASS', '\n'.join(txt), time.time() - t0


def judge_big(exe):
    rc, out, dt = run([exe, 'big'], 900)
    v = classify(rc, out)
    if v == 'PASS':
        rc, out2, dt2 = run([exe, 'refuse'], 300)
        v = classify(rc, out2)
        out += out2
        dt += dt2
    return v, out, dt


# ---------------------------------------------------------------- planted bugs
MUTANTS = [
    # module, id, what, old, new
    ('build', 'B1', 'both-endian 733: little/big halves swapped',
     'static void set733(u8 *p, u32 n) { set731(p, n); set732(p + 4, n); }',
     'static void set733(u8 *p, u32 n) { set732(p, n); set731(p + 4, n); }'),
    ('build', 'B2', 'both-endian 723: halves swapped (volume seq / block size)',
     'static void set723(u8 *p, u32 n) { set721(p, n); set722(p + 2, n); }',
     'static void set723(u8 *p, u32 n) { set722(p, n); set721(p + 2, n); }'),
    ('build', 'B3', 'ISO file directory record length off by one (+9 instead of +8)',
     'add = 34 - 1 + (lname + 8); add += add & 1;\n                if ((rc = rec_room(&lo',
     'add = 34 - 1 + (lname + 9); add += add & 1;\n                if ((rc = rec_room(&lo'),
    ('build', 'B4', 'Joliet file names upper-cased',
     '                put_u16be(W + wo + 33, w, ls);\n                W[wo + 33 + ls * 2 + 0] = 0;',
     '                { u32 q; for (q = 0; q < ls; q++) if (w[q] < 128) w[q] = upcase((u8)w[q]); }\n                put_u16be(W + wo + 33, w, ls);\n                W[wo + 33 + ls * 2 + 0] = 0;'),
    ('build', 'B5', 'file data off by a sector (ISO records point one sector early)',
     'put_rec(L + lo, add, lba, fsize, e->mtime, flags, lname + 2);',
     'put_rec(L + lo, add, lba - 1, fsize, e->mtime, flags, lname + 2);'),
    ('build', 'B6', 'wrong title-ID source (separator after the first 4 chars)',
     "title_id[4] = '-';", "title_id[4] = '_';"),
    ('build', 'B7', 'path table / directory order: sort comparator reversed',
     'if (b->dir[n].parent > b->dir[m].parent) {', 'if (b->dir[n].parent < b->dir[m].parent) {'),
    ('build', 'B8', 'PVD volume size is toc-1',
     '    set733(isd + 80, b->toc);\n    set723(isd + 120, 1);\n    set723(isd + 124, 1);',
     '    set733(isd + 80, b->toc - 1);\n    set723(isd + 120, 1);\n    set723(isd + 124, 1);'),
    ('build', 'B9', 'path table parent index 0-based',
     'set721(r0, ln); set731(r0 + 2, d->llba); set721(r0 + 6, (u32)d->parent);',
     'set721(r0, ln); set731(r0 + 2, d->llba); set721(r0 + 6, (u32)d->parent - 1);'),
    ('build', 'B10', "'..' record points at the directory itself",
     'put_rec(L + lo, 40, pd->llba, pd->ldir * SEC,', 'put_rec(L + lo, 40, d->llba, d->ldir * SEC,'),
    ('build', 'B11', 'Joliet escape sequence wrong (SVD byte 89)',
     'isd[89] = 0x2f;', 'isd[89] = 0x2e;'),
    ('build', 'B12', 'file tail padded with 0xAA instead of zeros',
     'memset(buf + want, 0, padded - want);', 'memset(buf + want, 0xAA, padded - want);'),
    ('build', 'B13', 'Joliet root record size = ISO size',
     'set733(idr + 10, b->dir[0].wdir * SEC);', 'set733(idr + 10, b->dir[0].ldir * SEC);'),
    ('unpack', 'U1', 'Joliet ignored: ISO 9660 names used even when Joliet exists',
     'if (have_svd) { u.joliet = 1;', 'if (0 && have_svd) { u.joliet = 1;'),
    ('unpack', 'U2', 'both-endian field read from the wrong byte order',
     'u32 a = get731(p), b = get732(p + 4);', 'u32 a = get732(p), b = get732(p + 4);'),
    ('unpack', 'U3', 'record walk off by one byte',
     '            nrec++;\n            off += len;', '            nrec++;\n            off += len + 1;'),
    ('unpack', 'U4', 'Joliet ";1" version suffix kept in names',
     "if (c == ';') break;", "if (c == ';' && 0) break;"),
    ('unpack', 'U5', 'file data read one sector late',
     'u64 off = (u64)ex[k].extent * SEC, left = ex[k].size;', 'u64 off = ((u64)ex[k].extent + 1) * SEC, left = ex[k].size;'),
    ('unpack', 'U6', 'last chunk written rounded up to the sector (padding leaks into files)',
     'u32 n = left > CHUNK ? CHUNK : (u32)left;', 'u32 n = left > CHUNK ? CHUNK : (u32)((left + 2047) & ~2047u);'),
    ('unpack', 'U7', 'folders deeper than 2 levels skipped',
     'rc = walk_dir(u, subs[i].extent, subs[i].size, depth + 1);', 'rc = depth >= 2 ? 0 : walk_dir(u, subs[i].extent, subs[i].size, depth + 1);'),
    ('unpack', 'U8', 'file counter not incremented',
     'if (!rc) { u->st->files++; u->st->bytes += total; }', 'if (!rc) { u->st->bytes += total; }'),
    ('unpack', 'U9', 'UTF-16 name decoding keeps only the low byte',
     'u32 c = ((u32)nm[i] << 8) | nm[i + 1];', 'u32 c = (u32)nm[i + 1];'),
    ('unpack', 'U10', 'multi-extent chains: only the first extent of a chain is written',
     'for (k = 0; k < nex && !rc; k++) {', 'for (k = 0; k < (nex > 1 ? 1 : nex) && !rc; k++) {'),
]


def make_mutant(mid, old, new, build_dir):
    s = open(CORE, encoding='utf-8', newline='').read()
    assert s.count(old) == 1, '%s: pattern occurs %d times' % (mid, s.count(old))
    p = os.path.join(build_dir, 'core_%s.c' % mid)
    open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new))
    return p


def run_mutants(only=None):
    bd = WORK + '/mut'
    shutil.rmtree(bd, ignore_errors=True)
    os.makedirs(bd)
    results = []
    for mod, mid, what, old, new in MUTANTS:
        if only and mid not in only:
            continue
        t0 = time.time()
        core = make_mutant(mid, old, new, bd)
        exe = os.path.join(bd, 'exe_' + mid)
        exe_s = os.path.join(bd, 'exes_' + mid)
        ok1, o1 = compile_(core, exe)
        ok2, o2 = compile_(core, exe_s, ['-DPS3ISO_EXTENT_MAX=0x1000'])
        if not (ok1 and ok2):
            results.append((mid, mod, what, 'BROKEN', 'compile error', time.time() - t0))
            print('%-4s %-6s BROKEN (compile)  %s' % (mid, mod, what))
            continue
        main = judge_build_main if mod == 'build' else judge_unpack_main
        v, out, dt = main(exe)
        by = 'MAIN-' + mod
        if v == 'PASS':
            v, out, dt2 = judge_syn(exe, exe_s)
            by = 'SYN'
            dt += dt2
        if v == 'PASS':
            v, out, dt2 = judge_big(exe)
            by = 'BIG'
            dt += dt2
        verdict = {'FAIL': 'KILLED', 'BROKEN': 'BROKEN', 'PASS': 'SURVIVED'}[v]
        tail = [l for l in out.strip().split('\n') if l][-1][:150] if out.strip() else ''
        results.append((mid, mod, what, verdict, by if verdict == 'KILLED' else '', time.time() - t0))
        print('%-4s %-6s %-8s by %-11s %5.0fs  %s\n        | %s' % (mid, mod, verdict, by, time.time() - t0, what, tail))
        sys.stdout.flush()
        for f in (exe, exe_s, core):
            try:
                os.remove(f)
            except OSError:
                pass
    return results


def main():
    args = sys.argv[1:]
    os.makedirs(WORK, exist_ok=True)
    if '--only-mutants' not in args:
        print('== baseline (unmutated) ==')
        ok, out = compile_(CORE, WORK + '/exe_base')
        assert ok, out
        ok, out = compile_(CORE, WORK + '/exe_base_small', ['-DPS3ISO_EXTENT_MAX=0x1000'])
        assert ok, out
        ok, out = compile_(CORE, WORK + '/exe_base_san', san=True)
        assert ok, out
        ok, out = compile_(CORE, WORK + '/exe_base_small_san', ['-DPS3ISO_EXTENT_MAX=0x1000'], san=True)
        assert ok, out
        r = subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-pedantic', '-Werror', '-c', CORE, '-o', '/dev/null', '-I' + SRC],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print('strict C99 -Wall -Wextra -pedantic -Werror: %s' % ('clean' if r.returncode == 0 else 'WARNINGS\n' + r.stdout.decode()))
        res = {}
        quick = '--quick' in args
        skip = ('PASS', 'skipped (--quick)', 0)
        for name, fn in (('MAIN-build', lambda: skip if quick else judge_build_main(WORK + '/exe_base')),
                         ('MAIN-unpack', lambda: skip if quick else judge_unpack_main(WORK + '/exe_base')),
                         ('SYN (plain)', lambda: judge_syn(WORK + '/exe_base', WORK + '/exe_base_small')),
                         ('SYN (asan+ubsan)', lambda: judge_syn(WORK + '/exe_base_san', WORK + '/exe_base_small_san')),
                         ('BIG', lambda: judge_big(WORK + '/exe_base'))):
            v, out, dt = fn()
            res[name] = v
            print('%-18s %-6s %6.1fs' % (name, v, dt))
            print('   ' + '\n   '.join(out.strip().split('\n')[-14:]))
            sys.stdout.flush()
        if any(v != 'PASS' for v in res.values()):
            print('BASELINE DOES NOT PASS - not running mutants')
            sys.exit(1)
    if '--no-mutants' in args:
        return
    print('== planted bugs ==')
    only = [a for a in args if re.match(r'^[BU]\d+$', a)]
    results = run_mutants(only or None)
    killed = sum(1 for r in results if r[3] == 'KILLED')
    print('\nSUMMARY: %d mutants, %d KILLED, %d SURVIVED, %d BROKEN' % (
        len(results), killed, sum(1 for r in results if r[3] == 'SURVIVED'), sum(1 for r in results if r[3] == 'BROKEN')))
    for mod in ('build', 'unpack'):
        rs = [r for r in results if r[1] == mod]
        print('  %s: %d/%d killed' % (mod, sum(1 for r in rs if r[3] == 'KILLED'), len(rs)))
    sys.exit(0 if all(r[3] == 'KILLED' for r in results) else 1)


if __name__ == '__main__':
    main()
