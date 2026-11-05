# IDA Python: makes the byte patterns in src/signatures.cpp for a game build open in IDA.
#
#   code_sig(ea)            pattern of a function's first instructions (CODE entries)
#   ref_sig(target)         pattern of code calling or referencing `target`, with the rel32 offset
#                           and instruction end inside the match (REF entries); for functions or
#                           data whose own bytes aren't unique
#
# Branch targets, rip-relative displacements and absolute image addresses are wildcarded ("?").
# A pattern is grown one instruction at a time until it has at least MIN_FIXED fixed bytes and
# matches exactly once in .text.

import re
import struct

import ida_bytes
import ida_funcs
import ida_nalt
import ida_segment
import ida_ua
import idautils

MIN_FIXED = 16
MAX_BYTES = 120
BACK_MAX = 8  # instructions a REF pattern may start before the referencing one

_text = ida_segment.get_segm_by_name(".text")
_TEXT = ida_bytes.get_bytes(_text.start_ea, _text.end_ea - _text.start_ea)
_BASE = ida_nalt.get_imagebase()


def _insn(ea):
    insn = ida_ua.insn_t()
    n = ida_ua.decode_insn(insn, ea)
    if n <= 0:
        return None
    raw = ida_bytes.get_bytes(ea, n)
    mask = [True] * n
    for op in insn.ops:
        if op.type == ida_ua.o_void:
            break
        if op.type in (ida_ua.o_mem, ida_ua.o_near, ida_ua.o_far) and op.offb:
            end = n if op.type != ida_ua.o_mem else min(n, op.offb + 4)
            mask[op.offb:end] = [False] * (end - op.offb)
        elif op.type == ida_ua.o_imm and op.offb and n >= op.offb + 8 and _BASE <= op.value < _BASE + 0x10000000:
            mask[op.offb:op.offb + 8] = [False] * 8
    return insn, raw, mask


def _unique(raw, mask):
    rx = re.compile(b"".join(re.escape(bytes([b])) if m else b"." for b, m in zip(raw, mask)), re.DOTALL)
    count = 0
    for _ in rx.finditer(_TEXT):
        count += 1
        if count == 2:
            return False
    return count == 1


def _fmt(raw, mask):
    return " ".join("%02X" % b if m else "?" for b, m in zip(raw, mask))


def _grow(insns, start, raw=b"", mask=None):
    mask = list(mask or [])
    for _, r, m in insns[start:]:
        if len(raw) >= MAX_BYTES:
            break
        raw += r
        mask += m
        if sum(mask) >= MIN_FIXED and _unique(raw, mask):
            return raw, mask
    return None


def _func_insns(f):
    out, ea = [], f.start_ea
    while ea < f.end_ea:
        d = _insn(ea)
        if d is None:
            break
        out.append((ea, d[1], d[2]))
        ea += len(d[1])
    return out


def code_sig(ea):
    f = ida_funcs.get_func(ea)
    found = _grow(_func_insns(f), 0) if f and f.start_ea == ea else None
    return _fmt(*found) if found else None


def _ref_offset(ea, target):
    insn, raw, _ = _insn(ea)
    for op in insn.ops:
        if op.type == ida_ua.o_void:
            break
        if op.type in (ida_ua.o_mem, ida_ua.o_near) and op.addr == target and op.offb:
            return op.offb, len(raw)
    return None


def ref_sig(target):
    """Shortest REF pattern over all code references: (pattern, disp, next) or None."""
    best = None
    for x in idautils.XrefsTo(target):
        f = ida_funcs.get_func(x.frm)
        ref = _ref_offset(x.frm, target) if f else None
        if not ref:
            continue
        insns = _func_insns(f)
        idx = next(i for i, (ea, _, _) in enumerate(insns) if ea == x.frm)
        for back in range(0, BACK_MAX + 1):
            start = idx - back
            if start < 0:
                break
            raw = b"".join(r for _, r, _ in insns[start:idx + 1])
            mask = sum((m for _, _, m in insns[start:idx + 1]), [])
            disp = len(raw) - len(insns[idx][1]) + ref[0]
            nxt = len(raw) - len(insns[idx][1]) + ref[1]
            found = (raw, mask) if sum(mask) >= MIN_FIXED and _unique(raw, mask) else _grow(insns, idx + 1, raw, mask)
            if found and (best is None or len(found[0]) < best[0]):
                best = (len(found[0]), _fmt(*found), disp, nxt)
            if found:
                break
    return best[1:] if best else None


def check(pattern):
    """Number of matches of a pattern in .text (should be 1)."""
    rx = re.compile(b"".join(b"." if t == "?" else re.escape(bytes([int(t, 16)])) for t in pattern.split()), re.DOTALL)
    return sum(1 for _ in rx.finditer(_TEXT))
