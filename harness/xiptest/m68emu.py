"""xip.x の関数を Mac の上の 68000 (Unicorn) で動かす小さな土台。

xip.x のテキストとデータ (再配置前、先頭 = 番地 0) を番地 0 に置き、関数をスタック渡しの引数で呼ぶ。
DOS _CHANGE_PR (0xFFFF) と IOCS (trap #15) は実行せず、その手前で止めて呼び出し側に知らせる
(スレッドを切り替えたことにして、あとで続きから再開できる)。
"""
from unicorn import Uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, UC_HOOK_CODE, UcError
from unicorn.m68k_const import *
import struct

MEM = 0x80000
DREGS = [UC_M68K_REG_D0 + i for i in range(8)]
AREGS = [UC_M68K_REG_A0 + i for i in range(8)]


class Thread:
    """1 本のスレッド: 自分のスタックと、呼びかけの関数の続き (pc とレジスタ)"""

    def __init__(self, emu, stack_top):
        self.emu = emu
        self.stack_top = stack_top
        self.ctx = None          # 途中で止まっているときのレジスタ
        self.ret = stack_top - 0x20  # 戻り番地 (ここに来たら関数が終わった)

    def call(self, entry, *args, regs=None):
        """関数を最初から呼ぶ準備をして、走らせる。戻り値は run() と同じ"""
        u = self.emu.uc
        sp = self.stack_top - 0x40
        for a in reversed(args):
            sp -= 4
            u.mem_write(sp, struct.pack('>I', a & 0xffffffff))
        sp -= 4
        u.mem_write(sp, struct.pack('>I', self.ret))
        vals = regs or {}
        for i, r in enumerate(DREGS):
            u.reg_write(r, vals.get('d%d' % i, 0xd0d0d000 + i))
        for i, r in enumerate(AREGS[:7]):
            u.reg_write(r, vals.get('a%d' % i, 0xa0a0a000 + i))
        u.reg_write(UC_M68K_REG_SR, 0x2000)   # 先にスーパーバイザにする (後だと A7 が SSP に切り替わる)
        u.reg_write(UC_M68K_REG_A7, sp)
        self.ctx = None
        return self.run(entry)

    def run(self, pc=None):
        """続きから走らせる。('done', d0) / ('yield', pc) / ('iocs', pc, d0) / ('steps', pc) を返す"""
        u = self.emu.uc
        if pc is None:
            u.context_restore(self.ctx)
            pc = u.reg_read(UC_M68K_REG_PC)
        self.emu.event = None
        try:
            u.emu_start(pc, self.ret, count=self.emu.max_steps)
        except UcError as e:
            return ('error', u.reg_read(UC_M68K_REG_PC), str(e))
        ev = self.emu.event
        self.ctx = u.context_save()
        if ev is None:
            pc = u.reg_read(UC_M68K_REG_PC)
            if pc == self.ret:
                return ('done', u.reg_read(UC_M68K_REG_D0))
            return ('steps', pc)
        return ev

    def skip(self, n=2):
        """止まった命令 (_CHANGE_PR / trap #15) を飛ばして続ける準備"""
        u = self.emu.uc
        u.context_restore(self.ctx)
        u.reg_write(UC_M68K_REG_PC, u.reg_read(UC_M68K_REG_PC) + n)
        self.ctx = u.context_save()

    def regs(self):
        u = self.emu.uc
        u.context_restore(self.ctx)
        return {('d%d' % i): u.reg_read(r) for i, r in enumerate(DREGS)} | \
               {('a%d' % i): u.reg_read(r) for i, r in enumerate(AREGS)}


class Emu:
    def __init__(self, image, max_steps=200000):
        self.uc = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
        self.uc.ctl_set_cpu_model(UC_CPU_M68K_M68000)
        self.uc.mem_map(0, MEM)
        self.uc.mem_write(0, image)
        self.max_steps = max_steps
        self.event = None
        self.steps_left = None
        self.uc.hook_add(UC_HOOK_CODE, self._code)

    def _code(self, uc, addr, size, user):
        op = struct.unpack('>H', uc.mem_read(addr, 2))[0]
        if op == 0xffff:                     # DOS _CHANGE_PR: 譲る (ここで止める)
            self.event = ('yield', addr)
            uc.emu_stop()
        elif op == 0x4e4f:                   # trap #15 (IOCS)
            self.event = ('iocs', addr, uc.reg_read(UC_M68K_REG_D0))
            uc.emu_stop()
        elif self.steps_left is not None:
            self.steps_left -= 1
            if self.steps_left <= 0:         # 時間切れ (割り込みでスレッドが切り替わったことにする)
                self.event = ('preempt', addr)
                uc.emu_stop()

    def rd(self, addr, n):
        return bytes(self.uc.mem_read(addr, n))

    def wr(self, addr, data):
        self.uc.mem_write(addr, data)
