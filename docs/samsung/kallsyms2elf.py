#!/usr/bin/env python3
"""Convert /proc/kallsyms output into a minimal ELF64 (AArch64) symbol file.

The Samsung production kernel ships no vmlinux - only kallsyms.txt with
"ADDR TYPE NAME" lines.  This builds an ELF whose symbols live in .text /
.data sections (SHT_NOBITS, so the file stays small) so GDB can resolve
names <-> addresses in both directions: breakpoints by function name,
symbolicated backtraces, 'info symbol', 'x/i sym', etc.

Boot with 'nokaslr' so the addresses match the running kernel.
"""

import struct
import sys

EM_AARCH64 = 183
ET_EXEC = 2
SHT_NOBITS = 8
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
STB_GLOBAL = 1
STT_NOTYPE = 0
STT_OBJECT = 1
STT_FUNC = 2

SEC_TEXT = 1
SEC_DATA = 2

FUNC_TYPES = set("TtWw")
DATA_TYPES = set("DdRrBbGg")


def read_kallsyms(path):
    raw = []
    einittext = None
    etext = None
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:
                continue
            addr_s, typ, name = parts[0], parts[1], parts[2]
            if len(typ) != 1 or typ in "Uu":
                continue
            if name.startswith("$") or name.startswith("."):
                continue
            try:
                addr = int(addr_s, 16)
            except ValueError:
                continue
            if addr == 0:
                continue
            raw.append((name, addr, typ))
            if name == "_einittext":
                einittext = addr
            elif name == "_etext":
                etext = addr
    if einittext is None:
        einittext = etext
    if einittext is None:
        einittext = max(a for _, a, t in raw if t in FUNC_TYPES) + 1

    syms = []
    for name, addr, typ in raw:
        if typ in DATA_TYPES and addr >= einittext:
            syms.append((name, addr, STT_OBJECT, SEC_DATA))
        elif typ in FUNC_TYPES or addr < einittext:
            # text range: covers linker boundary symbols like
            # __inittext_begin that kallsyms tags as data
            stt = STT_FUNC if typ in FUNC_TYPES else STT_NOTYPE
            syms.append((name, addr, stt, SEC_TEXT))
        else:
            syms.append((name, addr, STT_NOTYPE, SEC_DATA))
    return syms


def build_elf(syms, out_path):
    strtab = bytearray(b"\0")
    symtab = bytearray()
    for name, addr, stt, sec in syms:
        name_off = len(strtab)
        strtab += name.encode() + b"\0"
        st_info = (STB_GLOBAL << 4) | stt
        symtab += struct.pack("<IBBHQQ", name_off, st_info, 0, sec, addr, 0)

    def sec_range(secid):
        vals = [a for _, a, _, s in syms if s == secid]
        return (min(vals), max(vals)) if vals else (0, 0)

    text_lo, text_hi = sec_range(SEC_TEXT)
    data_lo, data_hi = sec_range(SEC_DATA)

    shstrtab = b"\0.text\0.data\0.symtab\0.strtab\0.shstrtab\0"
    no_text = shstrtab.index(b".text")
    no_data = shstrtab.index(b".data")
    no_symtab = shstrtab.index(b".symtab")
    no_strtab = shstrtab.index(b".strtab")
    no_shstrtab = shstrtab.index(b".shstrtab")

    ehdr_size = 64
    shent_size = 64
    symtab_off = ehdr_size
    strtab_off = symtab_off + len(symtab)
    shstrtab_off = strtab_off + len(strtab)
    shoff = (shstrtab_off + len(shstrtab) + 7) & ~7
    shnum = 6

    ehdr = struct.pack(
        "<16sHHIQQQIHHHHHH",
        b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8,  # 64-bit LE SYSV
        ET_EXEC,
        EM_AARCH64,
        1,          # e_version
        0,          # e_entry
        0,          # e_phoff
        shoff,      # e_shoff
        0,          # e_flags
        ehdr_size,  # e_ehsize
        0,          # e_phentsize
        0,          # e_phnum
        shent_size,
        shnum,
        5,          # e_shstrndx
    )
    assert len(ehdr) == ehdr_size

    def shent(name, shtype, flags, addr, off, size, link, info, align, entsize):
        return struct.pack("<IIQQQQIIQQ", name, shtype, flags, addr, off,
                           size, link, info, align, entsize)

    shdrs = b"\0" * shent_size
    shdrs += shent(no_text, SHT_NOBITS, SHF_ALLOC | SHF_EXECINSTR,
                   text_lo, 0, text_hi - text_lo, 0, 0, 4, 0)
    shdrs += shent(no_data, SHT_NOBITS, SHF_ALLOC | SHF_WRITE,
                   data_lo, 0, data_hi - data_lo, 0, 0, 8, 0)
    shdrs += shent(no_symtab, SHT_SYMTAB, 0, 0, symtab_off, len(symtab),
                   4, 0, 8, 24)   # sh_link = .strtab (index 4)
    shdrs += shent(no_strtab, SHT_STRTAB, 0, 0, strtab_off, len(strtab),
                   0, 0, 1, 0)
    shdrs += shent(no_shstrtab, SHT_STRTAB, 0, 0, shstrtab_off,
                   len(shstrtab), 0, 0, 1, 0)

    with open(out_path, "wb") as f:
        f.write(ehdr)
        f.write(symtab)
        f.write(strtab)
        f.write(shstrtab)
        f.write(b"\0" * (shoff - f.tell()))
        f.write(shdrs)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "kallsyms.txt"
    dst = sys.argv[2] if len(sys.argv) > 2 else "vmlinux.kallsyms.elf"
    syms = read_kallsyms(src)
    if not syms:
        sys.exit(f"no symbols found in {src}")
    build_elf(syms, dst)
    print(f"wrote {dst}: {len(syms)} symbols")


if __name__ == "__main__":
    main()
