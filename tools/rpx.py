"""Inspect Wii U RPX/RPL modules.

  rpx.py sections <rpx>
  rpx.py imports  <rpx> [lib]
  rpx.py calls    <rpx> <symbol>...
  rpx.py dis      <rpx> <addr> [before] [after]
  rpx.py find     <rpx> <capstone-op-regex>
"""
import re
import struct
import sys
import zlib

SHF_RPL_ZLIB = 0x08000000
SHT_SYMTAB = 2
SHT_RELA = 4
R_PPC_REL24 = 10


class Rpx:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        assert raw[:4] == b'\x7fELF' and raw[5] == 2, 'not a big-endian ELF'
        shoff, = struct.unpack_from('>I', raw, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from('>HHH', raw, 0x2E)
        self.sections = []
        for i in range(shnum):
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from('>10I', raw, shoff + i * shentsize)
            data = raw[off:off + size] if typ != 8 else b''
            if flags & SHF_RPL_ZLIB and data:
                data = zlib.decompress(data[4:])
            self.sections.append(dict(name=name, type=typ, flags=flags, addr=addr, data=data, link=link, info=info, entsize=entsize))
        strtab = self.sections[shstrndx]['data']
        for s in self.sections:
            s['name'] = self._str(strtab, s['name'])
        self.symbols = []
        for s in self.sections:
            if s['type'] == SHT_SYMTAB:
                names = self.sections[s['link']]['data']
                for j in range(0, len(s['data']), 16):
                    nm, value, size, info, other, shndx = struct.unpack_from('>IIIBBH', s['data'], j)
                    self.symbols.append(dict(name=self._str(names, nm), value=value, size=size, shndx=shndx))
        self.relocs = {}
        for s in self.sections:
            if s['type'] == SHT_RELA:
                for j in range(0, len(s['data']), 12):
                    offset, info, addend = struct.unpack_from('>IIi', s['data'], j)
                    self.relocs[offset] = (info & 0xFF, self.symbols[info >> 8], addend)

    @staticmethod
    def _str(table, off):
        return table[off:table.index(b'\0', off)].decode('ascii', 'replace')

    def text(self):
        return next(s for s in self.sections if s['name'] == '.text')

    def symbol_name(self, addr):
        best = None
        for sym in self.symbols:
            if sym['name'] and sym['value'] <= addr < sym['value'] + max(sym['size'], 1):
                best = sym
        return best['name'] if best else None

    def disasm(self, start, count):
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
        text = self.text()
        data = text['data']
        addr, end = start, start + count * 4
        while addr < end:
            # capstone stops at the first word it cannot decode (paired singles etc.), resume after it
            for insn in md.disasm(data[addr - text['addr']:end - text['addr']], addr):
                note = ''
                if insn.address in self.relocs:
                    typ, sym, addend = self.relocs[insn.address]
                    note = f'  ; reloc {typ} -> {sym["name"] or hex(sym["value"])}+{addend:#x}'
                yield insn, note
                addr = insn.address + 4
            else:
                addr += 4


def main():
    cmd, path, *args = sys.argv[1:]
    rpx = Rpx(path)
    if cmd == 'sections':
        for i, s in enumerate(rpx.sections):
            print(f'{i:3} {s["name"]:28} type={s["type"]:#010x} addr={s["addr"]:#010x} size={len(s["data"]):#x}')
    elif cmd == 'imports':
        for sym in rpx.symbols:
            sec = rpx.sections[sym['shndx']] if sym['shndx'] < len(rpx.sections) else None
            if sec and sec['name'].startswith('.fimport_') and (not args or sec['name'] == f'.fimport_{args[0]}'):
                print(f'{sym["value"]:#010x} {sec["name"][9:]}::{sym["name"]}')
    elif cmd == 'calls':
        for offset, (typ, sym, addend) in sorted(rpx.relocs.items()):
            if typ == R_PPC_REL24 and sym['name'] in args:
                print(f'{offset:#010x} {sym["name"]:28} in {rpx.symbol_name(offset)}')
    elif cmd == 'dis':
        addr = int(args[0], 16)
        before = int(args[1]) if len(args) > 1 else 16
        after = int(args[2]) if len(args) > 2 else 16
        for insn, note in rpx.disasm(addr - before * 4, before + after + 1):
            mark = '>' if insn.address == addr else ' '
            print(f'{mark}{insn.address:#010x}  {insn.mnemonic:8} {insn.op_str}{note}')
    elif cmd == 'xref':
        targets = {int(a, 16) for a in args}
        lis = re.compile(r'^(r\d+), (-?0x[0-9a-f]+|-?\d+)$')
        mem = re.compile(r'^(?:[rf]\d+), (-?0x[0-9a-f]+|-?\d+)\((r\d+)\)$')
        imm = re.compile(r'^(r\d+), (r\d+), (-?0x[0-9a-f]+|-?\d+)$')
        hi = {}
        text = rpx.text()
        for insn, note in rpx.disasm(text['addr'], len(text['data']) // 4):
            ops = insn.op_str
            if insn.mnemonic == 'lis' and (m := lis.match(ops)):
                hi[m.group(1)] = (int(m.group(2), 0) & 0xFFFF) << 16
                continue
            ea = None
            if (m := mem.match(ops)) and m.group(2) in hi:
                ea = (hi[m.group(2)] + int(m.group(1), 0)) & 0xFFFFFFFF
            elif insn.mnemonic in ('addi', 'addic') and (m := imm.match(ops)) and m.group(2) in hi:
                ea = (hi[m.group(2)] + int(m.group(3), 0)) & 0xFFFFFFFF
            if ea in targets:
                print(f'{insn.address:#010x}  {ea:#010x}  {insn.mnemonic:8} {ops}')
            if insn.mnemonic in ('bl', 'blr', 'bctrl', 'b'):
                hi.clear()
            elif (m := re.match(r'^(r\d+)', ops)) and insn.mnemonic not in ('stw', 'stwu', 'stb', 'sth', 'stfs', 'stfd', 'cmpwi', 'cmplwi', 'cmpw', 'cmplw'):
                hi.pop(m.group(1), None)
    elif cmd == 'find':
        pattern = re.compile(args[0])
        text = rpx.text()
        for insn, note in rpx.disasm(text['addr'], len(text['data']) // 4):
            if pattern.search(f'{insn.mnemonic} {insn.op_str}'):
                print(f'{insn.address:#010x}  {insn.mnemonic:8} {insn.op_str}{note}')


if __name__ == '__main__':
    main()
