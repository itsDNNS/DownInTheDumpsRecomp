"""Static recompiler: translate self-contained x86-32 routines of DID.EXE into portable C++.

Used for the hand-written, heavily unrolled codecs (decomp.ASM, dcpbiz.ASM, playfil.ASM ...) whose
exact behaviour matters more than readability.  The generated code works on a virtual 32-bit address
space (blub::Arena): the original data object is mapped at its link address (tables, globals) and
the caller places its buffers into the arena.  Correctness is checked against the original code
running in the Unicorn emulator (scripts/didemu.py) by the port's test suite.

Usage: python generator/lift.py <out.cpp> <name> <entry> <start-end> [<start-end> ...]
"""
import json
import struct
import sys
from pathlib import Path

import capstone
from capstone import x86_const as X

import os

HERE = Path(__file__).resolve().parent
OBJ_DIR = Path(os.environ.get('BLUB_OBJ_DIR', HERE.parent.parent / 'work'))
CODE = (OBJ_DIR / 'obj1.bin').read_bytes()
DATA = (OBJ_DIR / 'obj2.bin').read_bytes()
SENTINEL = 0xDEAD0000

R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
R16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
R8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
R8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
BITS = {1: 8, 2: 16, 4: 32}
CT = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t'}


class LiftError(Exception):
    pass


def get_reg(n):
    if n in R32:
        return n
    if n in R16:
        return '(uint16_t)%s' % R16[n]
    if n in R8L:
        return '(uint8_t)%s' % R8L[n]
    if n in R8H:
        return '(uint8_t)(%s >> 8)' % R8H[n]
    raise LiftError('register %s' % n)


def set_reg(n, v):
    if n in R32:
        return '%s = (uint32_t)(%s);' % (n, v)
    if n in R16:
        r = R16[n]
        return '%s = (%s & 0xFFFF0000u) | (uint16_t)(%s);' % (r, r, v)
    if n in R8L:
        r = R8L[n]
        return '%s = (%s & 0xFFFFFF00u) | (uint8_t)(%s);' % (r, r, v)
    if n in R8H:
        r = R8H[n]
        return '%s = (%s & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(%s) << 8);' % (r, r, v)
    raise LiftError('register %s' % n)


def reg_size(n):
    return 4 if n in R32 else 2 if n in R16 else 1


class Lifter:
    def __init__(self, ranges):
        self.ranges = ranges
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.insns = []
        for a, b in ranges:
            self.insns += list(self.md.disasm(CODE[a - 0x10000:b - 0x10000], a))
        self.addrs = {i.address for i in self.insns}
        self.dispatch = {SENTINEL}

    def inside(self, a):
        return a in self.addrs

    # -- operands
    def addr_expr(self, i, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append(get_reg(i.reg_name(m.base)))
        if m.index:
            idx = get_reg(i.reg_name(m.index))
            parts.append('%s * %d' % (idx, m.scale) if m.scale != 1 else idx)
        if m.disp or not parts:
            parts.append('0x%Xu' % (m.disp & 0xFFFFFFFF))
        return '(uint32_t)(%s)' % ' + '.join(parts)

    def read(self, i, op, size=None):
        if op.type == X.X86_OP_REG:
            return get_reg(i.reg_name(op.reg))
        if op.type == X.X86_OP_IMM:
            s = size or op.size
            return '0x%Xu' % (op.imm & ((1 << (8 * s)) - 1))
        if op.type == X.X86_OP_MEM:
            return 'm.r%d(%s)' % (BITS[size or op.size], self.addr_expr(i, op))
        raise LiftError('operand')

    def write(self, i, op, v):
        if op.type == X.X86_OP_REG:
            return set_reg(i.reg_name(op.reg), v)
        if op.type == X.X86_OP_MEM:
            return 'm.w%d(%s, %s);' % (BITS[op.size], self.addr_expr(i, op), v)
        raise LiftError('write operand')

    # -- one instruction -> C++ statements
    def lift(self, i):
        mn = i.mnemonic.replace('notrack ', '')
        ops = i.operands
        nxt = i.address + i.size
        s = []
        if mn in ('mov', 'movzx'):
            s.append(self.write(i, ops[0], '(%s)%s' % (CT[ops[0].size], self.read(i, ops[1]))))
        elif mn == 'movsx':
            st = {1: 'int8_t', 2: 'int16_t'}[ops[1].size]
            s.append(self.write(i, ops[0], '(%s)(int32_t)(%s)%s' % (CT[ops[0].size], st, self.read(i, ops[1]))))
        elif mn == 'lea':
            s.append(self.write(i, ops[0], self.addr_expr(i, ops[1])))
        elif mn in ('add', 'sub', 'cmp', 'and', 'or', 'xor', 'test', 'adc', 'sbb'):
            sz = ops[0].size
            a, b = self.read(i, ops[0]), self.read(i, ops[1], sz)
            fn = {'cmp': 'sub', 'test': 'and'}.get(mn, mn)
            call = 'f.%s%d(%s, %s)' % (fn, BITS[sz], a, b)
            if mn in ('cmp', 'test'):
                s.append('%s;' % call)
            else:
                s.append(self.write(i, ops[0], call))
        elif mn in ('inc', 'dec', 'neg', 'not'):
            sz = ops[0].size
            s.append(self.write(i, ops[0], 'f.%s%d(%s)' % (mn, BITS[sz], self.read(i, ops[0]))))
        elif mn in ('shr', 'shl', 'sar', 'rol', 'ror'):
            sz = ops[0].size
            cnt = self.read(i, ops[1], 1) if len(ops) > 1 else '1u'
            s.append(self.write(i, ops[0], 'f.%s%d(%s, %s)' % (mn, BITS[sz], self.read(i, ops[0]), cnt)))
        elif mn in ('bt', 'bts', 'btr'):
            sz = ops[0].size
            v = self.read(i, ops[0])
            bit = '(%s & %du)' % (self.read(i, ops[1], 1), BITS[sz] - 1)
            s.append('f.cf = (%s >> %s) & 1;' % (v, bit))
            if mn == 'bts':
                s.append(self.write(i, ops[0], '%s | (1u << %s)' % (v, bit)))
            if mn == 'btr':
                s.append(self.write(i, ops[0], '%s & ~(1u << %s)' % (v, bit)))
        elif mn == 'xchg':
            a, b = self.read(i, ops[0]), self.read(i, ops[1])
            s.append('{ uint32_t t_ = %s; %s %s }' % (a, self.write(i, ops[0], b), self.write(i, ops[1], 't_')))
        elif mn == 'bswap':
            r = i.reg_name(ops[0].reg)
            s.append('%s = __builtin_bswap32(%s);' % (r, r))
        elif mn in ('cbw', 'cwde', 'cdq'):
            s.append({'cbw': 'eax = (eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)eax;',
                      'cwde': 'eax = (uint32_t)(int32_t)(int16_t)eax;',
                      'cdq': 'edx = ((int32_t)eax < 0) ? 0xFFFFFFFFu : 0u;'}[mn])
        elif mn in ('imul', 'mul', 'div'):
            s.append(self.muldiv(i, mn, ops))
        elif mn in ('clc', 'stc'):
            s.append('f.cf = %d;' % (mn == 'stc'))
        elif mn in ('push', 'pop'):
            sz = ops[0].size
            if mn == 'push':
                s.append('esp -= %d; m.w%d(esp, %s);' % (sz, BITS[sz], self.read(i, ops[0])))
            else:
                s.append('{ uint32_t t_ = m.r%d(esp); esp += %d; %s }' % (BITS[sz], sz, self.write(i, ops[0], 't_')))
        elif mn == 'pushal':
            s.append('{ uint32_t sp_ = esp; ' + ' '.join('esp -= 4; m.w32(esp, %s);' % (r if r != 'esp' else 'sp_')
                                                          for r in R32) + ' }')
        elif mn == 'popal':
            s.append(' '.join(('%s = m.r32(esp); esp += 4;' % r) if r != 'esp' else 'esp += 4;'
                              for r in reversed(R32)))
        elif mn.startswith(('lods', 'stos', 'movs', 'rep ')) and not mn.startswith('movsx'):
            s.append(self.string_op(i, mn))
        elif mn == 'jmp':
            s.append(self.jump(i, ops[0]))
        elif mn in COND:
            t = ops[0].imm
            if not self.inside(t):
                raise LiftError('conditional jump outside at %#x' % i.address)
            s.append('if (%s) goto L_%X;' % (COND[mn], t))
        elif mn == 'loop':
            s.append('if (--ecx != 0) goto L_%X;' % ops[0].imm)
        elif mn == 'call':
            if ops[0].type != X.X86_OP_IMM or not self.inside(ops[0].imm):
                raise LiftError('call outside the lifted code at %#x' % i.address)
            self.dispatch.add(nxt)
            s.append('esp -= 4; m.w32(esp, 0x%Xu); goto L_%X;' % (nxt, ops[0].imm))
        elif mn == 'ret':
            extra = ops[0].imm if ops else 0
            s.append('target = m.r32(esp); esp += %d; goto dispatch;' % (4 + extra))
        elif mn == 'nop':
            pass
        else:
            raise LiftError('unsupported %s %s at %#x' % (mn, i.op_str, i.address))
        return s

    def muldiv(self, i, mn, ops):
        if mn == 'imul' and len(ops) == 3:
            return self.write(i, ops[0], '(uint32_t)((int32_t)%s * (int32_t)%s)' % (self.read(i, ops[1]),
                                                                                  self.read(i, ops[2])))
        if mn == 'imul' and len(ops) == 2:
            sz = ops[0].size
            st = {2: 'int16_t', 4: 'int32_t'}[sz]
            return self.write(i, ops[0], '(%s)((%s)%s * (%s)%s)' % (CT[sz], st, self.read(i, ops[0]), st,
                                                                    self.read(i, ops[1])))
        sz = ops[0].size
        src = self.read(i, ops[0])
        if mn == 'mul' and sz == 2:
            return '{ uint32_t p_ = (uint32_t)(uint16_t)eax * (uint16_t)%s; %s %s }' % (
                src, set_reg('ax', 'p_'), set_reg('dx', 'p_ >> 16'))
        if mn == 'mul' and sz == 4:
            return '{ uint64_t p_ = (uint64_t)eax * %s; eax = (uint32_t)p_; edx = (uint32_t)(p_ >> 32); }' % src
        if mn == 'imul' and sz == 4:
            return ('{ int64_t p_ = (int64_t)(int32_t)eax * (int32_t)%s; eax = (uint32_t)p_; '
                    'edx = (uint32_t)((uint64_t)p_ >> 32); }' % src)
        if mn == 'div' and sz == 1:
            return ('{ uint16_t n_ = (uint16_t)eax; uint8_t d_ = %s; %s %s }' % (
                src, set_reg('al', 'n_ / d_'), set_reg('ah', 'n_ % d_')))
        if mn == 'div' and sz == 2:
            return ('{ uint32_t n_ = ((edx & 0xFFFF) << 16) | (uint16_t)eax; uint16_t d_ = %s; %s %s }' % (
                src, set_reg('ax', 'n_ / d_'), set_reg('dx', 'n_ % d_')))
        if mn == 'div' and sz == 4:
            return ('{ uint64_t n_ = ((uint64_t)edx << 32) | eax; uint32_t d_ = %s; '
                    'eax = (uint32_t)(n_ / d_); edx = (uint32_t)(n_ %% d_); }' % src)
        raise LiftError('%s size %d at %#x' % (mn, sz, i.address))

    def string_op(self, i, mn):
        rep = mn.startswith('rep ')
        base = mn.split()[1] if rep else mn
        k = base[:4]
        sz = {'b': 1, 'w': 2, 'd': 4}[base[4]]
        b = BITS[sz]
        body = {'lods': set_reg({1: 'al', 2: 'ax', 4: 'eax'}[sz], 'm.r%d(esi)' % b) + ' esi += %d;' % sz,
                'stos': 'm.w%d(edi, %s); edi += %d;' % (b, get_reg({1: 'al', 2: 'ax', 4: 'eax'}[sz]), sz),
                'movs': 'm.w%d(edi, m.r%d(esi)); esi += %d; edi += %d;' % (b, b, sz, sz)}[k]
        if rep:
            if k == 'lods':
                raise LiftError('rep lods')
            return 'while (ecx) { %s --ecx; }' % body
        return body

    def jump(self, i, op):
        if op.type == X.X86_OP_IMM:
            if not self.inside(op.imm):
                raise LiftError('jump outside at %#x' % i.address)
            return 'goto L_%X;' % op.imm
        if op.type == X.X86_OP_MEM:
            m = op.mem
            if m.index and m.scale == 4 and not m.base:
                tb = m.disp & 0xFFFFFFFF
                for k in range(256):
                    o = tb - 0x40000 + 4 * k
                    if 0 <= o < len(DATA) - 3:
                        t = struct.unpack_from('<I', DATA, o)[0]
                        if self.inside(t):
                            self.dispatch.add(t)
            else:
                self.dispatch.update(self.addrs)      # unknown table: allow any target
            return 'target = %s; goto dispatch;' % self.read(i, op)
        if op.type == X.X86_OP_REG:
            self.dispatch.update(self.addrs)
            return 'target = %s; goto dispatch;' % get_reg(i.reg_name(op.reg))
        raise LiftError('jump operand')

    def generate(self, name, entry):
        body = []
        targets = set()
        for i in self.insns:
            for op in i.operands:
                if op.type == X.X86_OP_IMM and (i.mnemonic in COND or i.mnemonic in ('jmp', 'loop', 'call')):
                    targets.add(op.imm)
        stmts = []
        for i in self.insns:
            stmts.append((i, self.lift(i)))
        labels = targets | self.dispatch | {entry}
        for i, ss in stmts:
            if i.address in labels:
                body.append('L_%X:' % i.address)
            body.append('    ' + ' '.join(ss) + '  // %s %s' % (i.mnemonic, i.op_str))
        cases = sorted(a for a in self.dispatch if a in self.addrs)
        out = ['// Generated by scripts/lift.py from DID.EXE - do not edit.',
               '// Ranges: ' + ', '.join('%#x-%#x' % r for r in self.ranges),
               '#include "codec/lifted.h"', '', 'namespace blub {', '',
               'void %s(Cpu &c, Arena &m) {' % name,
               '    uint32_t &eax = c.eax, &ecx = c.ecx, &edx = c.edx, &ebx = c.ebx, &esp = c.esp, '
               '&ebp = c.ebp, &esi = c.esi, &edi = c.edi;',
               '    Flags &f = c.f;',
               '    uint32_t target = 0;',
               '    esp -= 4; m.w32(esp, 0x%Xu);' % SENTINEL,
               '    goto L_%X;' % entry,
               'dispatch:',
               '    switch (target) {',
               '    case 0x%Xu: return;' % SENTINEL]
        out += ['    case 0x%Xu: goto L_%X;' % (a, a) for a in cases]
        out += ['    default: c.fault = target; return;', '    }']
        out += body
        out += ['    c.fault = 0xFFFFFFFFu;  // fell off the end', '}', '', '}  // namespace blub', '']
        return '\n'.join(out)


COND = {'je': 'f.zf', 'jne': '!f.zf', 'jb': 'f.cf', 'jae': '!f.cf', 'jbe': '(f.cf || f.zf)',
        'ja': '(!f.cf && !f.zf)', 'jl': '(f.sf != f.of)', 'jge': '(f.sf == f.of)',
        'jle': '(f.zf || f.sf != f.of)', 'jg': '(!f.zf && f.sf == f.of)', 'js': 'f.sf', 'jns': '!f.sf',
        'jecxz': '(ecx == 0)', 'jcxz': '((uint16_t)ecx == 0)'}


def main():
    out, name, entry = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
    ranges = [tuple(int(x, 16) for x in r.split('-')) for r in sys.argv[4:]]
    lf = Lifter(ranges)
    Path(out).write_text(lf.generate(name, entry))
    print('%s: %d instructions, %d dispatch targets' % (name, len(lf.insns), len(lf.dispatch)))


if __name__ == '__main__':
    main()
