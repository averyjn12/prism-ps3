#!/usr/bin/env python3
"""v0.67: read the two compiled RSX programs (cgcomp output, PSL1GHT's rsxVertexProgram / rsxFragmentProgram
layout, big-endian: ppu/include/rsx/rsx_program.h) and check what src/gfx.c looks up by name is there.

    python3 host/shader_check.py <prism_v.vpo> <prism_f.fpo>

gfx.c asks for the vertex attributes "position", "texcoord", "color" and the fragment sampler "texture";
a name cgcomp spelled differently would leave the RSX path off on every console (gfx_gpu_why -22)."""
import struct
import sys


def cstr(d, off):
    end = d.index(b"\0", off)
    return d[off:end].decode("ascii")


def attrs(d, off, n):
    # rsxProgramAttrib: u32 name_off, u32 index, u8 type, 3 pad  (rsx_program.h)
    out = []
    for i in range(n):
        name_off, index = struct.unpack_from(">II", d, off + 12 * i)
        out.append((cstr(d, name_off) if name_off else "", index))
    return out


def main():
    vp = open(sys.argv[1], "rb").read()
    fp = open(sys.argv[2], "rb").read()
    bad = 0
    magic, _pad, regs, n_attr, n_const, n_insn, attr_off, const_off, ucode_off, in_mask, out_mask = struct.unpack_from(">HHHHHHIIIII", vp, 0)
    va = attrs(vp, attr_off, n_attr)
    print("vertex program: magic %04x, %d instructions, %d attributes %s, input mask %08x, output mask %08x, ucode at %d of %d bytes"
          % (magic, n_insn, n_attr, va, in_mask, out_mask, ucode_off, len(vp)))
    want = {"position": 0, "texcoord": 8, "color": 9}      # GCM_VERTEX_ATTRIB_POS / TEX0 / TEX1
    for name, idx in want.items():
        got = [i for n, i in va if n == name]
        if got != [idx]:
            print("FAIL: vertex attribute %s: expected index %d, found %s" % (name, idx, got))
            bad += 1
    if in_mask != (1 << 0) | (1 << 8) | (1 << 9):
        print("FAIL: vertex input mask %08x is not position + texcoord0 + texcoord1" % in_mask)
        bad += 1
    if n_insn != 3:
        print("FAIL: the pass-through program should be 3 moves, found %d instructions" % n_insn)
        bad += 1
    # rsx_fp: u16 magic, pad, num_regs, num_attr, num_const, num_insn; u32 attr_off, const_off, ucode_off; ...
    magic, _pad, regs, n_attr, n_const, n_insn, attr_off, const_off, ucode_off = struct.unpack_from(">HHHHHHIII", fp, 0)
    fa = attrs(fp, attr_off, n_attr)
    print("fragment program: magic %04x, %d instructions, %d attributes %s, %d constants, ucode at %d of %d bytes"
          % (magic, n_insn, n_attr, fa, n_const, ucode_off, len(fp)))
    tex = [i for n, i in fa if n == "texture"]
    if tex != [0]:
        print("FAIL: fragment sampler 'texture': expected unit 0, found %s" % tex)
        bad += 1
    print("shader check: %s" % ("FAILED" if bad else "ok"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
