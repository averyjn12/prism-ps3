#!/usr/bin/env python3
"""v0.67: plant one bug at a time in a COPY of src/gfx.c and run host/gx_test.c against it: a test that has
never failed is untested.

    python3 host/gx_mutants.py            (after host/rsx_host_run.sh, which compiles the two programs)

A mutant counts as caught only on the test's own FAIL lines or the stand-in's refusal (exit 70). A mutant
that does not compile, or whose text is not found exactly once, is BROKEN and stops the run. The unmutated
file must pass first."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
S = os.path.join(HERE, "..", "src")
PSL = os.path.expanduser("~/ps3dev/ps3dev")
B = os.path.expanduser("~/prism_app_host/rsx_host")
M = os.path.join(B, "mut")

MUTANTS = [
    # v0.69: the RSX has finished a frame before the next one is built (gfx.c cb_rewind), so a texture slot
    # or vertex buffer reused "too early" can no longer be read late: the three mutants that planted that
    # (slot reused a frame early / at once, a forgotten texture's slot freed at once) are not bugs any more
    # and are gone from this list. In their place: the rewind itself, v0.68's two faults first.
    ("v0.68: sent to the start without waiting for the frame", "\tctrl->put = cur;\n\twhile (ctrl->get != cur)", "\twhile (0)"),
    ("v0.68: rewound onto the library's set-up commands", "\tctx->current = ctx0.current;\n}", "\tctx->current = ctx0.begin;\n}"),
    ("context not set back after the jump", "\tctx->current = ctx0.current;\n}", "}"),
    ("jump written but PUT not moved", "\tctrl->put = cb_start;\n", ""),
    ("jump to the buffer's begin, context to after the set-up", "rsxSetJumpCommand(ctx, cb_start);", "rsxSetJumpCommand(ctx, cb_start - 0x3a0);"),
    ("no rewind at all, 30 KB of commands and on", "\tif (gfx_gpu)\n\t\tcb_rewind();", "\t(void)cb_rewind;"),
    ("one vertex buffer written", "p = vb[cur] + nv;", "p = vb[0] + nv;"),
    ("texel array bound 4 bytes off", "vb_off[cur] + 12, sizeof(gxv_t), 2", "vb_off[cur] + 16, sizeof(gxv_t), 2"),
    ("colour array bound as 3 floats", "vb_off[cur] + 20, sizeof(gxv_t), 4", "vb_off[cur] + 20, sizeof(gxv_t), 3"),
    # v0.72: the depth buffer
    ("position bound without its depth", "vb_off[cur], sizeof(gxv_t), 3", "vb_off[cur], sizeof(gxv_t), 2"),
    ("depth buffer never emptied", "\t\trsxClearSurface(ctx, GCM_CLEAR_Z | GCM_CLEAR_S);\n", ""),
    ("depth test never turned off", "rsxSetDepthTestEnable(ctx, on ? GCM_TRUE : GCM_FALSE);", "rsxSetDepthTestEnable(ctx, GCM_TRUE);"),
    ("depth never written", "rsxSetDepthWriteEnable(ctx, on ? GCM_TRUE : GCM_FALSE);", "rsxSetDepthWriteEnable(ctx, GCM_FALSE);"),
    ("depth switched without a flush", "void gx_depth(int on)\n{\n\tgx_flush();", "void gx_depth(int on)\n{"),
    ("depth turned over", "p->z = depth01 * 2.0f - 1.0f;", "p->z = 1.0f - depth01 * 2.0f;"),
    ("changed pixels not noticed", "if (e->sum == sum)", "if (1)"),
    ("drawing into the buffer on screen", "set_target(cur);", "set_target(cur ^ 1);"),
    ("scissor without a flush", "gx_flush();\n\tclip_empty = x1 <= x0", "clip_empty = x1 <= x0"),
    ("opaque alpha sent as half", "u32 a = s[3] >= 0x80 ? 0xFF : (u32)s[3] * 2;", "u32 a = s[3];"),
    ("red and blue swapped", "d[x] = (a << 24) | ((u32)s[0] << 16) | ((u32)s[1] << 8) | s[2];",
     "d[x] = (a << 24) | ((u32)s[2] << 16) | ((u32)s[1] << 8) | s[0];"),
    ("texture pitch from its width", "t.pitch = TEX_PITCH;", "t.pitch = e->w * 4;"),
    ("no check of the vertex buffer's end", "if (nv + (unsigned)n > VB_MAX)", "if (0)"),
    ("frames never counted", "frame_no++;\n\tset_target", "set_target"),
    ("texture rows written at the texture's width", "u32 *d = e->mem + y * TEX_DIM;", "u32 *d = e->mem + y * w;"),
    ("flip of the wrong buffer", "flip(cur);\n\t\tcur ^= 1;\n\t\tframe_begin();", "flip(cur ^ 1);\n\t\tcur ^= 1;\n\t\tframe_begin();"),
    ("empty scissor still draws", "if (clip_empty || n <= 0)", "if (n <= 0)"),
    ("texture bound without a flush", "gx_flush();\n\t\tbind(e);", "bind(e);"),
    ("clear without a flush", "gx_flush();\n\trsxSetScissor(ctx, 0, 0, dw, dh);", "rsxSetScissor(ctx, 0, 0, dw, dh);"),
    ("y not turned over", "p->y = 1.0f - Y * (2.0f / CH);", "p->y = Y * (2.0f / CH) - 1.0f;"),
]


def build(gfx):
    r = subprocess.run(["gcc", "-O2", "-w", "-DUI_RSX", "-I" + os.path.join(HERE, "fake_rsx"), "-I" + S, "-idirafter", PSL + "/ppu/include",
                        os.path.join(HERE, "gx_test.c"), gfx, os.path.join(HERE, "fake_rsx.c"), os.path.join(B, "shaders.c"), "-lm",
                        "-o", os.path.join(M, "gx_test")], capture_output=True, text=True)
    return r.returncode == 0, r.stderr


def run():
    r = subprocess.run([os.path.join(M, "gx_test")], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    os.makedirs(M, exist_ok=True)
    ok, err = build(os.path.join(S, "gfx.c"))
    if not ok:
        print("BROKEN: the unmutated build fails\n" + err[:400])
        return 2
    rc, out = run()
    if rc != 0:
        print("BROKEN: the unmutated test fails\n" + out[-400:])
        return 2
    print("baseline:", out.strip().splitlines()[-1])
    # gfx.h holds gx_put: mutants there go through a copied header directory
    src = open(os.path.join(S, "gfx.c")).read()
    hdr = open(os.path.join(S, "gfx.h")).read()
    caught = 0
    for name, old, new in MUTANTS:
        in_c, in_h = src.count(old), hdr.count(old)
        if in_c + in_h != 1:
            print("BROKEN: %s: the text to change is found %d times" % (name, in_c + in_h))
            return 2
        open(os.path.join(M, "gfx.c"), "w").write(src.replace(old, new) if in_c else src)
        open(os.path.join(M, "gfx.h"), "w").write(hdr.replace(old, new) if in_h else hdr)
        r = subprocess.run(["gcc", "-O2", "-w", "-DUI_RSX", "-I" + M, "-I" + os.path.join(HERE, "fake_rsx"), "-I" + S, "-idirafter", PSL + "/ppu/include",
                            os.path.join(HERE, "gx_test.c"), os.path.join(M, "gfx.c"), os.path.join(HERE, "fake_rsx.c"), os.path.join(B, "shaders.c"),
                            "-lm", "-o", os.path.join(M, "gx_test")], capture_output=True, text=True)
        if r.returncode != 0:
            print("BROKEN: %s does not compile\n%s" % (name, r.stderr[:300]))
            return 2
        rc, out = run()
        why = [l for l in out.splitlines() if l.startswith("FAIL:") or l.startswith("FAKE RSX")]
        if why and (rc == 70 or rc == 1):
            caught += 1
            print("caught    %-46s %s" % (name, why[0][:80]))
        else:
            print("SURVIVED  %-46s (exit %d)" % (name, rc))
    print("gx_mutants: %d of %d caught" % (caught, len(MUTANTS)))
    return 0 if caught == len(MUTANTS) else 1


if __name__ == "__main__":
    sys.exit(main())
