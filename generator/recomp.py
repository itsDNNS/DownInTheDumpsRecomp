"""Whole-program static recompiler for DID.EXE -> portable C++ (port/src/engine/recomp/).

Every function of the original program becomes a C++ function `void F_<addr>(Cpu&, Arena&, uint32_t entry)`
working on the virtual 32-bit address space of the original (blub::Arena):
  * registers/flags/x87 stack live in blub::Cpu, memory accesses go through the arena,
  * call/ret become native C++ calls (the return address is still pushed, a mismatch is reported),
  * jumps between functions become tail calls, jumps into the middle of a function use `entry`,
  * indirect calls/jumps go through a global dispatcher, local jump tables through a local switch,
  * `int`, `in`/`out` go to the host layer, functions listed in data/recomp_hle.txt are replaced by
    native host implementations (C runtime, SOS sound library, hardware probes).
Functions/bodies come from Ghidra (generator/data/functions.json, exported with
ghidra_scripts/ExportFuncs.java).

Usage: python scripts/recomp.py
"""
import bisect
import collections
import json
import struct
import sys
from pathlib import Path

import capstone
from capstone import x86_const as X

import os

HERE = Path(__file__).resolve().parent
# inputs/outputs (set by generate.py): the object images of DID.EXE, the function list, the
# configuration files and the output directory for the generated C++
OBJ_DIR = Path(os.environ.get('BLUB_OBJ_DIR', HERE.parent.parent / 'work'))
CONFIG = Path(os.environ.get('BLUB_CONFIG', HERE / 'data'))
FUNCTIONS = Path(os.environ.get('BLUB_FUNCTIONS', CONFIG / 'functions.json'))
CODE = (OBJ_DIR / 'obj1.bin').read_bytes()
DATA = (OBJ_DIR / 'obj2.bin').read_bytes()
CODE_BASE, DATA_BASE = 0x10000, 0x40000
# start of the linked Watcom C runtime / HMI SOS library; game code only calls into it via the HLE set
LIB_START = 0x2FC2E
OUT = Path(os.environ.get('BLUB_OUT', HERE.parent / 'generated' / 'recomp'))

R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
R16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
R8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
R8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
BITS = {1: 8, 2: 16, 4: 32}
CT = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t'}
ST = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t'}

COND = {'je': 'f.zf', 'jne': '!f.zf', 'jb': 'f.cf', 'jae': '!f.cf', 'jbe': '(f.cf || f.zf)',
        'ja': '(!f.cf && !f.zf)', 'jl': '(f.sf != f.of)', 'jge': '(f.sf == f.of)',
        'jle': '(f.zf || f.sf != f.of)', 'jg': '(!f.zf && f.sf == f.of)', 'js': 'f.sf', 'jns': '!f.sf',
        'jo': 'f.of', 'jno': '!f.of', 'jp': 'f.pf()', 'jnp': '!f.pf()',
        'jecxz': '(ecx == 0)', 'jcxz': '((uint16_t)ecx == 0)'}
SETCC = {'set' + k[1:]: v for k, v in COND.items() if not k.startswith('jecx') and not k.startswith('jcx')}


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


class Program:
    def __init__(self):
        fs = json.loads(FUNCTIONS.read_text())
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.hle_names = {l.strip() for l in (CONFIG / 'recomp_hle.txt').read_text().splitlines()
                          if l.strip() and not l.startswith('#')}
        self.funcs = {}             # entry -> dict(name, body, insns)
        for f in fs:
            self.funcs[f['entry']] = dict(name=f['name'], body=[tuple(r) for r in f['body']])
        self.hle = {e for e, f in self.funcs.items() if f['name'] in self.hle_names}
        # functions that call host_hook(c, m, addr) on entry (the original code still runs afterwards)
        self.hook_names = {l.strip() for l in (CONFIG / 'recomp_hooks.txt').read_text().splitlines()
                           if l.strip() and not l.startswith('#')}
        self.hook_addrs = {int(n.split()[0], 16) for n in self.hook_names if n.startswith('0x')}
        self.hook_names = {n for n in self.hook_names if not n.startswith('0x')}
        self.hooks = {e for e, f in self.funcs.items() if f['name'] in self.hook_names}
        # corrections of Ghidra's function boundaries (data/recomp_code.txt)
        self.extra_entries = []
        for line in (CONFIG / 'recomp_code.txt').read_text().splitlines():
            parts = line.split('#')[0].split()
            if not parts:
                continue
            if parts[0] == 'body':
                e = next(a for a, f in self.funcs.items() if f['name'] == parts[1])
                self.funcs[e]['body'] = [(int(parts[k], 16), int(parts[k + 1], 16)) for k in range(2, len(parts), 2)]
            elif parts[0] == 'extend':
                e = next(a for a, f in self.funcs.items() if f['name'] == parts[1])
                self.funcs[e]['body'].append((int(parts[2], 16), int(parts[3], 16)))
            elif parts[0] == 'code':
                self.extra_entries.append(int(parts[1], 16))
        missing = self.hle_names - {self.funcs[e]['name'] for e in self.hle}
        if missing:
            print('warning: HLE names not found:', sorted(missing))
        self.rebuild_index()

    def rebuild_index(self):
        self.ranges = sorted((a, b, e) for e, f in self.funcs.items() for a, b in f['body'])
        self.starts = [r[0] for r in self.ranges]

    def owner(self, addr):
        """function entry whose body contains addr"""
        k = bisect.bisect_right(self.starts, addr) - 1
        if k >= 0:
            a, b, e = self.ranges[k]
            if a <= addr < b:
                return e
        return None

    def decode(self, entry):
        f = self.funcs[entry]
        if 'insns' not in f:
            f['insns'] = []
            seen = set()
            for a, b in sorted(f['body']):
                for i in self.md.disasm(CODE[a - CODE_BASE:b - CODE_BASE], a):
                    if i.address not in seen:      # overlapping body ranges
                        seen.add(i.address)
                        f['insns'].append(i)
            f['addrs'] = seen
        return f

    def discover(self, target):
        """code reached by a jump/call that is not inside any Ghidra function: new pseudo function"""
        seen, work = set(), [target]
        while work:
            a = work.pop()
            while a not in seen and 0x10000 <= a < 0x3EE19 and self.owner(a) is None:
                ins = next(self.md.disasm(CODE[a - CODE_BASE:a - CODE_BASE + 16], a), None)
                if ins is None:
                    break
                seen.add(a)
                m = ins.mnemonic
                if m in COND or m == 'loop':
                    work.append(ins.operands[0].imm)
                if m in ('ret', 'retf', 'iretd') or m.startswith('jmp') or m.endswith(' jmp') or m == 'hlt':
                    if m == 'jmp' and ins.operands[0].type == X.X86_OP_IMM:
                        work.append(ins.operands[0].imm)
                    break
                a += ins.size
        if not seen:
            return
        body, cur = [], None
        for a in sorted(seen):
            ins = next(self.md.disasm(CODE[a - CODE_BASE:a - CODE_BASE + 16], a))
            if cur and cur[1] == a:
                cur[1] = a + ins.size
            else:
                cur = [a, a + ins.size]
                body.append(cur)
        self.funcs[target] = dict(name='sub_%X' % target, body=[tuple(b) for b in body])
        self.rebuild_index()


class FuncLifter:
    def __init__(self, prog, entry):
        self.p = prog
        self.e = entry
        self.f = prog.decode(entry)
        self.ext_calls = set()        # functions called/jumped to (for declarations)
        self.refs = set()             # functions whose address appears as an immediate (callbacks)
        self.indirect_local = False

    def inside(self, a):
        return a in self.f['addrs']

    def target_code(self, t, tail):
        """C++ statement transferring control to t outside this function"""
        p = self.p
        if t in p.hle:
            call = 'host_call(c, m, 0x%Xu);' % t
        else:
            own = p.owner(t)
            if own is None:
                p.discover(t)
                own = p.owner(t)
            if own is None:
                return 'c.bad_target(0x%Xu); return;' % t
            self.ext_calls.add(own)
            call = 'F_%X(c, m, %s);' % (own, '0' if own == t else '0x%Xu' % t)
            if own != t:
                p.secondary[own].add(t)
        return call + (' return;' if tail else '')

    # ---- operands
    SEGS = {'es': 0, 'cs': 1, 'ss': 2, 'ds': 3, 'fs': 4, 'gs': 5}

    def seg_of(self, i, op):
        mm = op.mem
        if mm.segment:
            return self.SEGS[i.reg_name(mm.segment)]
        if mm.base and i.reg_name(mm.base) in ('ebp', 'esp', 'bp', 'sp'):
            return 2
        return 3

    def addr_expr(self, i, op):
        return '(uint32_t)(c.sb[%d] + %s)' % (self.seg_of(i, op), self.ea(i, op))

    def ea(self, i, op):
        mm = op.mem
        parts = []
        if mm.base:
            parts.append(get_reg(i.reg_name(mm.base)))
        if mm.index:
            idx = get_reg(i.reg_name(mm.index))
            parts.append('%s * %d' % (idx, mm.scale) if mm.scale != 1 else idx)
        if mm.disp or not parts:
            parts.append('0x%Xu' % (mm.disp & 0xFFFFFFFF))
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

    # ---- instructions
    def lift(self, i):
        mn = i.mnemonic.replace('notrack ', '')
        ops = i.operands
        nxt = i.address + i.size
        if mn.startswith('f') and mn not in ('f',):
            return self.fpu(i, mn, ops)
        segreg = [o for o in ops if o.type == X.X86_OP_REG and i.reg_name(o.reg) in self.SEGS]
        if segreg and mn in ('mov', 'push', 'pop'):
            if mn == 'push':
                return 'esp -= 4; m.w32(esp, c.sel[%d]);' % self.SEGS[i.reg_name(ops[0].reg)]
            if mn == 'pop':
                return 'c.load_seg(%d, (uint16_t)m.r32(esp)); esp += 4;' % self.SEGS[i.reg_name(ops[0].reg)]
            if ops[0].type == X.X86_OP_REG and i.reg_name(ops[0].reg) in self.SEGS:
                return 'c.load_seg(%d, (uint16_t)%s);' % (self.SEGS[i.reg_name(ops[0].reg)], self.read(i, ops[1], 2))
            return self.write(i, ops[0], 'c.sel[%d]' % self.SEGS[i.reg_name(ops[1].reg)])
        if mn in ('mov', 'movzx'):
            return self.write(i, ops[0], '(%s)%s' % (CT[ops[0].size], self.read(i, ops[1])))
        if mn == 'movsx':
            return self.write(i, ops[0], '(%s)(int32_t)(%s)%s' % (CT[ops[0].size], ST[ops[1].size], self.read(i, ops[1])))
        if mn == 'lea':
            return self.write(i, ops[0], self.ea(i, ops[1]))
        if mn in ('add', 'sub', 'cmp', 'and', 'or', 'xor', 'test', 'adc', 'sbb'):
            sz = ops[0].size
            a, b = self.read(i, ops[0]), self.read(i, ops[1], sz)
            fn = {'cmp': 'sub', 'test': 'and'}.get(mn, mn)
            call = 'f.%s%d(%s, %s)' % (fn, BITS[sz], a, b)
            return '%s;' % call if mn in ('cmp', 'test') else self.write(i, ops[0], call)
        if mn in ('inc', 'dec', 'neg', 'not'):
            return self.write(i, ops[0], 'f.%s%d(%s)' % (mn, BITS[ops[0].size], self.read(i, ops[0])))
        if mn in ('shr', 'shl', 'sar', 'rol', 'ror', 'rcl', 'rcr', 'sal'):
            sz = ops[0].size
            cnt = self.read(i, ops[1], 1) if len(ops) > 1 else '1u'
            fn = 'shl' if mn == 'sal' else mn
            return self.write(i, ops[0], 'f.%s%d(%s, %s)' % (fn, BITS[sz], self.read(i, ops[0]), cnt))
        if mn in ('shld', 'shrd'):
            sz = ops[0].size
            return self.write(i, ops[0], 'f.%s%d(%s, %s, %s)' % (mn, BITS[sz], self.read(i, ops[0]),
                                                                 self.read(i, ops[1]), self.read(i, ops[2], 1)))
        if mn in ('bt', 'bts', 'btr', 'btc'):
            sz = ops[0].size
            v = self.read(i, ops[0])
            bit = '(%s & %du)' % (self.read(i, ops[1], 1), BITS[sz] - 1)
            s = 'f.cf = (%s >> %s) & 1;' % (v, bit)
            if mn != 'bt':
                opx = {'bts': '|', 'btr': '& ~', 'btc': '^'}[mn]
                s += ' ' + self.write(i, ops[0], '%s %s(1u << %s)' % (v, opx, bit))
            return s
        if mn == 'bsr':
            return '{ uint32_t v_ = %s; f.zf = v_ == 0; if (v_) { %s } }' % (
                self.read(i, ops[1]), self.write(i, ops[0], '31 - __builtin_clz(v_)'))
        if mn in SETCC:
            return self.write(i, ops[0], '(%s) ? 1u : 0u' % SETCC[mn])
        if mn == 'xchg':
            a, b = self.read(i, ops[0]), self.read(i, ops[1])
            return '{ uint32_t t_ = %s; %s %s }' % (a, self.write(i, ops[0], b), self.write(i, ops[1], 't_'))
        if mn == 'bswap':
            r = i.reg_name(ops[0].reg)
            return '%s = __builtin_bswap32(%s);' % (r, r)
        if mn in ('cbw', 'cwde', 'cdq', 'cwd'):
            return {'cbw': 'eax = (eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)eax;',
                    'cwde': 'eax = (uint32_t)(int32_t)(int16_t)eax;',
                    'cdq': 'edx = ((int32_t)eax < 0) ? 0xFFFFFFFFu : 0u;',
                    'cwd': 'edx = (edx & 0xFFFF0000u) | (((int16_t)eax < 0) ? 0xFFFFu : 0u);'}[mn]
        if mn in ('imul', 'mul', 'div', 'idiv'):
            return self.muldiv(i, mn, ops)
        if mn in ('clc', 'stc', 'cmc', 'cld', 'std'):
            return {'clc': 'f.cf = false;', 'stc': 'f.cf = true;', 'cmc': 'f.cf = !f.cf;',
                    'cld': 'f.df = false;', 'std': 'f.df = true;'}[mn]
        if mn == 'xlatb':
            return set_reg('al', 'm.r8(c.sb[3] + ebx + (uint8_t)eax)')
        if mn == 'aam':
            base = ops[0].imm if ops else 10
            return ('{ uint8_t a_ = (uint8_t)eax; %s %s f.szf<8>(a_ %% %d); }' % (
                set_reg('ah', 'a_ / %d' % base), set_reg('al', 'a_ %% %d' % base), base))
        if mn == 'sahf':
            return 'f.set_low((uint8_t)(eax >> 8));'
        if mn == 'lahf':
            return set_reg('ah', 'f.low()')
        if mn in ('push', 'pop'):
            sz = ops[0].size
            if mn == 'push':
                return 'esp -= %d; m.w%d(esp, %s);' % (sz, BITS[sz], self.read(i, ops[0]))
            return '{ uint32_t t_ = m.r%d(esp); esp += %d; %s }' % (BITS[sz], sz, self.write(i, ops[0], 't_'))
        if mn in ('pushf', 'pushfd'):
            sz = 2 if mn == 'pushf' else 4
            return 'esp -= %d; m.w%d(esp, f.eflags());' % (sz, BITS[sz])
        if mn in ('popf', 'popfd'):
            sz = 2 if mn == 'popf' else 4
            return 'f.set_eflags(m.r%d(esp)); esp += %d;' % (BITS[sz], sz)
        if mn in ('pushal', 'pushaw'):
            sz = 4 if mn == 'pushal' else 2
            regs = R32 if sz == 4 else ['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di']
            return '{ uint32_t sp_ = esp; ' + ' '.join(
                'esp -= %d; m.w%d(esp, %s);' % (sz, BITS[sz], ('sp_' if r in ('esp', 'sp') else get_reg(r)))
                for r in regs) + ' }'
        if mn in ('popal', 'popaw'):
            sz = 4 if mn == 'popal' else 2
            regs = R32 if sz == 4 else ['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di']
            out = []
            for r in reversed(regs):
                if r in ('esp', 'sp'):
                    out.append('esp += %d;' % sz)
                else:
                    out.append('%s esp += %d;' % (set_reg(r, 'm.r%d(esp)' % BITS[sz]), sz))
            return ' '.join(out)
        if mn == 'leave':
            return 'esp = ebp; ebp = m.r32(esp); esp += 4;'
        if (mn.startswith(('lods', 'stos', 'movs', 'scas', 'cmps', 'outs', 'ins', 'rep')) and
                not mn.startswith('movsx') and mn not in ('insertps',)):
            return self.string_op(i, mn)
        if mn == 'in':
            sz = ops[0].size
            port = self.read(i, ops[1], 2) if ops[1].type == X.X86_OP_IMM else '(uint16_t)edx'
            return set_reg({1: 'al', 2: 'ax', 4: 'eax'}[sz], 'host_in(c, %s, %d)' % (port, sz))
        if mn == 'out':
            sz = ops[1].size
            port = self.read(i, ops[0], 2) if ops[0].type == X.X86_OP_IMM else '(uint16_t)edx'
            return 'host_out(c, %s, %s, %d);' % (port, get_reg({1: 'al', 2: 'ax', 4: 'eax'}[sz]), sz)
        if mn == 'int':
            return 'host_int(c, m, 0x%X);' % ops[0].imm
        if mn in ('int3', 'nop', 'wait', 'cli', 'sti', 'fwait'):
            return ''
        if mn == 'jmp':
            return self.jump(i, ops[0])
        if mn in COND:
            t = ops[0].imm
            if self.inside(t):
                return 'if (%s) %s' % (COND[mn], self.goto(i, t))
            return 'if (%s) { %s }' % (COND[mn], self.target_code(t, True))
        if mn in ('loop', 'loope', 'loopne'):
            t = ops[0].imm
            cond = {'loop': '--ecx != 0', 'loope': '--ecx != 0 && f.zf', 'loopne': '--ecx != 0 && !f.zf'}[mn]
            if self.inside(t):
                return 'if (%s) %s' % (cond, self.goto(i, t))
            return 'if (%s) { %s }' % (cond, self.target_code(t, True))
        if mn == 'call':
            return self.call(i, ops[0], nxt)
        if mn == 'ret':
            extra = ops[0].imm if ops else 0
            return 'c.last_ret = m.r32(esp); esp += %d; return;' % (4 + extra)
        if mn == 'hlt':
            return 'c.halt(); return;'
        raise LiftError('unsupported %s %s at %#x' % (mn, i.op_str, i.address))

    def call(self, i, op, nxt):
        push = 'esp -= 4; m.w32(esp, 0x%Xu);' % nxt
        check = 'if (c.last_ret != 0x%Xu) c.bad_return(0x%Xu, 0x%Xu);' % (nxt, i.address, nxt)
        if op.type == X.X86_OP_IMM:
            return '%s %s %s' % (push, self.target_code(op.imm, False), check)
        return '{ uint32_t t_ = %s; %s call_address(c, m, t_); %s }' % (self.read(i, op), push, check)

    def goto(self, i, t):
        if t <= i.address:
            return '{ BLUB_POLL(); goto L_%X; }' % t
        return 'goto L_%X;' % t

    def jump(self, i, op):
        if op.type == X.X86_OP_IMM:
            if self.inside(op.imm):
                return self.goto(i, op.imm)
            return self.target_code(op.imm, True)
        self.indirect_local = True
        return 'target = %s; goto dispatch;' % self.read(i, op)

    def muldiv(self, i, mn, ops):
        if mn == 'imul' and len(ops) == 3:
            sz = ops[0].size
            return self.write(i, ops[0], 'f.imul%d(%s, %s)' % (BITS[sz], self.read(i, ops[1]), self.read(i, ops[2], sz)))
        if mn == 'imul' and len(ops) == 2:
            sz = ops[0].size
            return self.write(i, ops[0], 'f.imul%d(%s, %s)' % (BITS[sz], self.read(i, ops[0]), self.read(i, ops[1], sz)))
        sz = ops[0].size
        return 'c.%s%d(%s);' % (mn, BITS[sz], self.read(i, ops[0]))

    def string_op(self, i, mn):
        parts = mn.split()
        rep = parts[0] if parts[0] in ('rep', 'repe', 'repne', 'repz', 'repnz') else None
        base = parts[1] if rep else parts[0]
        k = base[:4]
        sz = {'b': 1, 'w': 2, 'd': 4}[base[4]]
        b = BITS[sz]
        acc = {1: 'al', 2: 'ax', 4: 'eax'}[sz]
        st = 'c.step(%d)' % sz
        src_seg = 3
        for o in i.operands:
            if o.type == X.X86_OP_MEM and o.mem.segment and i.reg_name(o.mem.base) in ('esi', 'si'):
                src_seg = self.SEGS[i.reg_name(o.mem.segment)]
        S = 'c.sb[%d] + esi' % src_seg
        D = 'c.sb[0] + edi'
        body = {
            'lods': set_reg(acc, 'm.r%d(%s)' % (b, S)) + ' esi += %s;' % st,
            'stos': 'm.w%d(%s, %s); edi += %s;' % (b, D, get_reg(acc), st),
            'movs': 'm.w%d(%s, m.r%d(%s)); esi += %s; edi += %s;' % (b, D, b, S, st, st),
            'scas': 'f.sub%d(%s, m.r%d(%s)); edi += %s;' % (b, get_reg(acc), b, D, st),
            'cmps': 'f.sub%d(m.r%d(%s), m.r%d(%s)); esi += %s; edi += %s;' % (b, b, S, b, D, st, st),
            'outs': 'host_out(c, (uint16_t)edx, m.r%d(%s), %d); esi += %s;' % (b, S, sz, st),
            'insb': None,
        }.get(k)
        if body is None:
            raise LiftError('string op %s at %#x' % (mn, i.address))
        if not rep:
            return body
        if k in ('scas', 'cmps'):
            stop = '!f.zf' if rep in ('repe', 'repz') else 'f.zf'
            return 'while (ecx) { %s --ecx; if (%s) break; }' % (body, stop)
        return 'while (ecx) { %s --ecx; }' % body

    # ---- x87
    def fmem(self, i, op, kind):
        a = self.addr_expr(i, op)
        return a

    def fpu_reg_form(self, i):
        """x87 instructions with register operands, decoded from the opcode bytes (Intel SDM tables):
        the disassembler's operand lists for these forms are not reliable"""
        b0, b1 = i.bytes[0], i.bytes[1]
        k = b1 & 7
        grp = (b1 >> 3) & 7
        push = 'c.fpu.push(c.fpu.st(%d));' % k
        if b0 == 0xD8:                           # st0 = st0 op st(i)
            return {0: "c.fpu.arith(0, c.fpu.st(%d), '+');", 1: "c.fpu.arith(0, c.fpu.st(%d), '*');",
                    2: 'c.fpu.compare(c.fpu.st(0), c.fpu.st(%d));',
                    3: 'c.fpu.compare(c.fpu.st(0), c.fpu.st(%d)); c.fpu.pop();',
                    4: "c.fpu.arith(0, c.fpu.st(%d), '-');", 5: "c.fpu.arith(0, c.fpu.st(%d), 'R');",
                    6: "c.fpu.arith(0, c.fpu.st(%d), '/');", 7: "c.fpu.arith(0, c.fpu.st(%d), 'Q');"}[grp] % k
        if b0 in (0xDC, 0xDE):                   # st(i) = st(i) op st0 [, pop]
            if b0 == 0xDE and b1 == 0xD9:
                return 'c.fpu.compare(c.fpu.st(0), c.fpu.st(1)); c.fpu.pop(); c.fpu.pop();'   # fcompp
            op = {0: '+', 1: '*', 4: 'R', 5: '-', 6: 'Q', 7: '/'}.get(grp)
            if op is None:
                raise LiftError('fpu %s %s at %#x' % (i.mnemonic, i.op_str, i.address))
            return "c.fpu.arith(%d, c.fpu.st(0), '%s');%s" % (k, op, ' c.fpu.pop();' if b0 == 0xDE else '')
        if b0 == 0xD9:
            if grp == 0:
                return push                                                          # fld st(i)
            if grp == 1:
                return 'std::swap(c.fpu.st(0), c.fpu.st(%d));' % k                   # fxch st(i)
            return None
        if b0 == 0xDD:
            if grp == 0:
                return ''                                                            # ffree
            if grp == 2:
                return 'c.fpu.st(%d) = c.fpu.st(0);' % k                             # fst st(i)
            if grp == 3:
                return 'c.fpu.st(%d) = c.fpu.st(0); c.fpu.pop();' % k                # fstp st(i)
            if grp == 4:
                return 'c.fpu.compare(c.fpu.st(0), c.fpu.st(%d));' % k               # fucom
            if grp == 5:
                return 'c.fpu.compare(c.fpu.st(0), c.fpu.st(%d)); c.fpu.pop();' % k  # fucomp
            return None
        if b0 == 0xDA and b1 == 0xE9:
            return 'c.fpu.compare(c.fpu.st(0), c.fpu.st(1)); c.fpu.pop(); c.fpu.pop();'   # fucompp
        return None

    def fpu(self, i, mn, ops):
        def sti(op):
            return int(i.reg_name(op.reg)[3])       # 'st(3)'
        memop = next((o for o in ops if o.type == X.X86_OP_MEM), None)
        if memop is None and len(i.bytes) == 2 and i.bytes[0] in (0xD8, 0xD9, 0xDA, 0xDC, 0xDD, 0xDE)                 and i.bytes[1] >= 0xC0:
            r = self.fpu_reg_form(i)
            if r is not None:
                return r
        if mn in ('fld', 'fild'):
            if memop is None:
                return 'c.fpu.push(c.fpu.st(%d));' % sti(ops[0])
            kind = ('i' if mn == 'fild' else 'f') + str(memop.size)
            return 'c.fpu.push(c.fpu.load_%s(m, %s));' % (kind, self.addr_expr(i, memop))
        if mn in ('fst', 'fstp', 'fist', 'fistp'):
            pop = ' c.fpu.pop();' if mn.endswith('p') else ''
            if memop is None:
                return 'c.fpu.st(%d) = c.fpu.st(0);%s' % (sti(ops[0]), pop)
            kind = ('i' if mn.startswith('fist') else 'f') + str(memop.size)
            return 'c.fpu.store_%s(m, %s);%s' % (kind, self.addr_expr(i, memop), pop)
        consts = {'fldz': '0.0L', 'fld1': '1.0L', 'fldpi': 'kPi', 'fldlg2': 'kLg2', 'fldln2': 'kLn2',
                  'fldl2e': 'kL2e', 'fldl2t': 'kL2t'}
        if mn in consts:
            return 'c.fpu.push(%s);' % consts[mn]
        arith = {'fadd': '+', 'fsub': '-', 'fmul': '*', 'fdiv': '/', 'fsubr': 'R', 'fdivr': 'Q'}
        base = mn[:-1] if mn.endswith('p') and mn[:-1] in arith else mn
        if base in arith:
            op = arith[base]
            pop = mn.endswith('p')
            if memop is not None:
                v = 'c.fpu.load_f%d(m, %s)' % (memop.size, self.addr_expr(i, memop))
                return 'c.fpu.arith(0, %s, \'%s\');' % (v, op)
            if len(ops) == 2:
                d, s = sti(ops[0]), sti(ops[1])
            elif len(ops) == 1:
                d, s = 0, sti(ops[0])
            else:
                d, s = 1, 0
            return 'c.fpu.arith(%d, c.fpu.st(%d), \'%s\');%s' % (d, s, op, ' c.fpu.pop();' if pop else '')
        simple = {'fchs': 'c.fpu.st(0) = -c.fpu.st(0);', 'fabs': 'c.fpu.st(0) = fabsl(c.fpu.st(0));',
                  'fsqrt': 'c.fpu.st(0) = sqrtl(c.fpu.st(0));', 'frndint': 'c.fpu.st(0) = c.fpu.round(c.fpu.st(0));',
                  'fsincos': 'c.fpu.sincos();', 'f2xm1': 'c.fpu.st(0) = exp2l(c.fpu.st(0)) - 1.0L;',
                  'fscale': 'c.fpu.st(0) = ldexpl(c.fpu.st(0), (int)truncl(c.fpu.st(1)));',
                  'fyl2x': 'c.fpu.fyl2x();', 'fninit': 'c.fpu.init();', 'fincstp': 'c.fpu.top = (c.fpu.top + 1) & 7;',
                  'ftst': 'c.fpu.compare(c.fpu.st(0), 0.0L);', 'fcompp': 'c.fpu.compare(c.fpu.st(0), c.fpu.st(1)); c.fpu.pop(); c.fpu.pop();',
                  'fwait': '', 'fnclex': 'c.fpu.sw &= 0x7F00;'}
        if mn in simple:
            return simple[mn]
        if mn in ('fcom', 'fcomp'):
            pop = ' c.fpu.pop();' if mn == 'fcomp' else ''
            if memop is not None:
                return 'c.fpu.compare(c.fpu.st(0), c.fpu.load_f%d(m, %s));%s' % (memop.size, self.addr_expr(i, memop), pop)
            return 'c.fpu.compare(c.fpu.st(0), c.fpu.st(%d));%s' % (sti(ops[0]) if ops else 1, pop)
        if mn == 'fxch':
            return 'std::swap(c.fpu.st(0), c.fpu.st(%d));' % (sti(ops[0]) if ops else 1)
        if mn == 'ffree':
            return ''
        if mn == 'fnstsw':
            if memop is not None:
                return 'm.w16(%s, c.fpu.status());' % self.addr_expr(i, memop)
            return set_reg('ax', 'c.fpu.status()')
        if mn == 'fnstcw':
            return 'm.w16(%s, c.fpu.cw);' % self.addr_expr(i, memop)
        if mn == 'fldcw':
            return 'c.fpu.cw = m.r16(%s);' % self.addr_expr(i, memop)
        raise LiftError('fpu %s %s at %#x' % (mn, i.op_str, i.address))

    def generate(self):
        f = self.f
        body = []
        labels = {self.e}
        for i in f['insns']:
            for op in i.operands:
                if op.type == X.X86_OP_IMM and (i.mnemonic in COND or i.mnemonic in ('jmp', 'loop', 'loope', 'loopne')):
                    labels.add(op.imm)
        stmts = []
        for i in f['insns']:
            for op in i.operands:
                if op.type == X.X86_OP_IMM and (op.imm & 0xFFFFFFFF) in self.p.funcs:
                    self.refs.add(op.imm & 0xFFFFFFFF)
            stmts.append((i, self.lift(i)))
        entries = sorted(self.p.secondary.get(self.e, set()))
        labels.update(entries)
        if self.indirect_local:
            labels.update(f['addrs'])
        for idx, (i, s) in enumerate(stmts):
            if i.address in labels:
                body.append('L_%X:' % i.address)
            if i.address in self.p.hook_addrs:
                body.append('    host_hook(c, m, 0x%Xu);' % i.address)
            body.append('    %s  // %X %s %s' % (s, i.address, i.mnemonic, i.op_str))
            # falling off a body range: continue at the next instruction of the program
            end = i.address + i.size
            nxt_in_body = idx + 1 < len(stmts) and stmts[idx + 1][0].address == end
            term = i.mnemonic in ('ret', 'jmp', 'notrack jmp', 'hlt')
            if not nxt_in_body and not term:
                if self.inside(end):
                    body.append('    goto L_%X;' % end)
                elif self.p.owner(end) is None and end not in self.p.extra_entries:
                    # e.g. behind "int 21h / ax=4C00h": padding, not code
                    body.append('    c.bad_target(0x%Xu); return;' % end)
                else:
                    body.append('    %s' % self.target_code(end, True))
        name = self.f['name']
        out = ['// %s' % name,
               'void F_%X(Cpu &c, Arena &m, uint32_t entry) {' % self.e,
               '    uint32_t &eax = c.eax, &ecx = c.ecx, &edx = c.edx, &ebx = c.ebx, &esp = c.esp, '
               '&ebp = c.ebp, &esi = c.esi, &edi = c.edi;',
               '    Flags &f = c.f;',
               '    uint32_t target = entry;',
               '    (void)f; (void)target;',
               '    if (entry) goto dispatch;']
        if self.e in self.p.hooks:
            out.append('    host_hook(c, m, 0x%Xu);' % self.e)
        out += ['    goto L_%X;' % self.e,
               'dispatch:',
               '    switch (target) {']
        cases = sorted(set(entries) | (f['addrs'] if self.indirect_local else set()))
        out += ['    case 0x%Xu: goto L_%X;' % (a, a) for a in cases]
        out += ['    default: jump_address(c, m, target); return;', '    }']
        out += body
        out += ['}', '']
        return '\n'.join(out)


def find_roots(p):
    """main, every function whose address is stored in the data object (VM tables, callbacks)"""
    roots = {e for e, f in p.funcs.items() if f['name'] == 'main'}
    # every function of the game code: some are only reached through computed addresses
    # (e.g. the FLX chunk handlers: table value + 6)
    roots |= {e for e in p.funcs if e < LIB_START}
    # the FLX/ANI chunk handlers are called through tables either at their entry or at entry + 6
    # (behind their 6-byte prologue): make every such address of a data-referenced function callable
    for k in range(0, len(DATA) - 3):
        v = struct.unpack_from('<I', DATA, k)[0]
        if v in p.funcs and v < LIB_START and p.owner(v + 6) == v and v + 6 in p.decode(v)['addrs']:
            p.secondary[v].add(v + 6)
    for a in p.extra_entries:                 # code outside of Ghidra's functions
        if p.owner(a) is None:
            p.discover(a)
        own = p.owner(a)
        if own is not None:
            roots.add(own)
            if own != a:
                p.secondary[own].add(a)
    for k in range(0, len(DATA) - 3):
        v = struct.unpack_from('<I', DATA, k)[0]
        if v in p.funcs and v < LIB_START:
            roots.add(v)
    return sorted(roots)


def stub(p, e, why):
    return ('// %s: not recompiled (%s)\nvoid F_%X(Cpu &c, Arena &m, uint32_t entry) {\n'
            '    c.unsupported(0x%Xu, "%s");\n}\n' % (p.funcs[e]['name'], why, e, e, why.replace('"', "'")))


def main():
    p = Program()
    p.secondary = collections.defaultdict(set)
    roots = find_roots(p)
    done, code, errors = set(), {}, []
    # two passes so secondary entries discovered late are included
    for rnd in range(3):
        before = {k: set(v) for k, v in p.secondary.items()}
        nfun = len(p.funcs)
        code, errors = {}, []
        work, seen = list(roots), set()
        while work:
            e = work.pop()
            if e in seen or e in p.hle or e not in p.funcs:
                continue
            if e >= LIB_START:
                errors.append((p.funcs[e]['name'], 'library function reached but not in HLE set'))
                seen.add(e)
                continue
            seen.add(e)
            fl = FuncLifter(p, e)
            try:
                code[e] = fl.generate()
                work.extend(fl.ext_calls)
                work.extend(fl.refs)
            except LiftError as ex:
                errors.append((p.funcs[e]['name'], str(ex)))
                code[e] = stub(p, e, str(ex))
        if {k: set(v) for k, v in p.secondary.items()} == before and len(p.funcs) == nfun:
            break
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob('gen_*.cpp'):
        old.unlink()
    ok = sorted(code)
    chunks = [ok[k:k + 60] for k in range(0, len(ok), 60)]
    decl = ['#pragma once', '// Generated by scripts/recomp.py - do not edit.', '#include "recomp/runtime.h"', '',
            'namespace blub {', '']
    decl += ['void F_%X(Cpu &c, Arena &m, uint32_t entry);  // %s' % (e, p.funcs[e]['name']) for e in ok]
    decl += ['', '}  // namespace blub', '']
    (OUT / 'functions.h').write_text('\n'.join(decl))
    for n, ch in enumerate(chunks):
        src = ['// Generated by scripts/recomp.py from DID.EXE - do not edit.', '#include "recomp/functions.h"',
               '#include <cmath>', '#include <utility>', '', 'namespace blub {', '']
        src += [code[e] for e in ch]
        src += ['}  // namespace blub', '']
        (OUT / ('gen_%02d.cpp' % n)).write_text('\n'.join(src))
    # dispatcher + names
    tab = ['// Generated by scripts/recomp.py - do not edit.', '#include "recomp/functions.h"', '',
           'namespace blub {', '',
           'bool dispatch_address(Cpu &c, Arena &m, uint32_t a) {', '    switch (a) {']
    for e in ok:
        tab.append('    case 0x%Xu: F_%X(c, m, 0); return true;' % (e, e))
        for s in sorted(p.secondary.get(e, ())):
            tab.append('    case 0x%Xu: F_%X(c, m, 0x%Xu); return true;' % (s, e, s))
    for e in sorted(p.hle):
        tab.append('    case 0x%Xu: host_call(c, m, 0x%Xu); return true;' % (e, e))
    tab += ['    default: return false;', '    }', '}', '',
            'const char *function_name(uint32_t a) {', '    switch (a) {']
    tab += ['    case 0x%Xu: return "%s";' % (e, p.funcs[e]['name'].replace('"', '')) for e in sorted(p.funcs)]
    tab += ['    default: return nullptr;', '    }', '}', '', '}  // namespace blub', '']
    (OUT / 'table.cpp').write_text('\n'.join(tab))
    hle = ['// Generated by scripts/recomp.py - addresses of the functions replaced by the host layer.',
           '#pragma once', '#include <cstdint>', '', 'namespace blub::hle {', '']
    hle += ['constexpr uint32_t k_%s = 0x%Xu;' % (p.funcs[e]['name'].replace('@', '_'), e) for e in sorted(p.hle)]
    hle += ['', '// game entry points used by the host']
    for name in ['main', 'TimerFunc', 'CriticalErrorHandler'] + sorted(p.hook_names):
        e = next(a for a, f in p.funcs.items() if f['name'] == name)
        hle.append('constexpr uint32_t k_%s = 0x%Xu;' % (name, e))
    hle += ['', '// instruction hooks (data/recomp_hooks.txt)']
    for line in (CONFIG / 'recomp_hooks.txt').read_text().splitlines():
        parts = line.split()
        if parts and parts[0].startswith('0x'):
            hle.append('constexpr uint32_t k_hook_%s = %su;' % (parts[1], parts[0]))
    hle += ['', '}  // namespace blub::hle', '']
    (OUT / 'hle_addrs.h').write_text('\n'.join(hle))
    print('functions: %d lifted, %d host, %d with unsupported instructions' % (len(ok), len(p.hle), len(errors)))
    for n, e in errors[:60]:
        print('  %-28s %s' % (n, e))


if __name__ == '__main__':
    main()
