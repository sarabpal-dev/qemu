# Samsung Kernel in QEMU — DEFEX rootfs

Samsung production kernels require a signed **DEFEX** (Defender Extended) policy
to boot. Without it the kernel panics during filesystem mounting.

The DEFEX policy is a binary blob called `dpolicy` stored inside
`first_stage_ramdisk/` on the device's `vendor_boot` partition. It is
**cryptographically bound to the kernel** — you must use the dpolicy that
matches your exact kernel build.

## Files you must provide

| File | Source |
|---|---|
| `kernel` | Extract from your device's `boot.img` |
| `vendor_boot.img` | Your device's stock vendor_boot partition (contains dpolicy) |

## Files provided here

| File | Purpose |
|---|---|
| `rootfs-defex.cpio.gz` | Base template — Buildroot + SELinux + device nodes |
| `run.sh` | Boot script (edit `QEMU=` path at the top) |
| `debug.sh` | GDB attach script (uses kallsyms ELF if no vmlinux) |
| `build-rootfs-defex.sh` | Swaps dpolicy from your `vendor_boot.img` into the rootfs |
| `kallsyms2elf.py` | Converts `kallsyms.txt` output into a minimal symbol ELF for GDB |
| `kallsyms.c` | Standalone C program to extract symbol tables offline from raw kernel binary |

## Kernel Symbol Extraction & Debugging

### Why Extract Symbols?
Usually, debugging a Linux kernel in QEMU using GDB requires an unstripped `vmlinux` ELF binary with full symbol tables. However, Samsung production devices only provide compiled, stripped raw kernel images (`kernel` extracted from `boot.img`).

To enable function name resolution, breakpoints, symbolic backtraces, and `info symbol` in GDB without having the original `vmlinux`, we extract kernel symbols and convert them into a synthetic ELF file (`vmlinux.kallsyms.elf`).

### Extracting Symbols (`kallsyms.c`)

`kallsyms.c` parses the raw binary kernel image offline by locating and decompressing the kernel's embedded `kallsyms` tables (token table, token index, relative base, offsets).

```bash
# 1. Compile the C extractor tool
gcc kallsyms.c -o kallsyms

# 2. Make it executable
chmod +x kallsyms

# 3. Extract symbols from your raw kernel image into kallsyms.txt
./kallsyms kernel > kallsyms.txt
```

### Generating Symbol ELF for GDB (`kallsyms2elf.py`)

Once `kallsyms.txt` is generated, convert it into a minimal ELF symbol file:

```bash
python3 kallsyms2elf.py kallsyms.txt vmlinux.kallsyms.elf
```

When you launch `./debug.sh`, GDB automatically loads `vmlinux.kallsyms.elf` (or generates it automatically if `kallsyms.txt` is present), enabling easy kernel debugging in QEMU.

## Usage Workflow

```bash
# 1. Compile kallsyms extractor and generate kallsyms.txt from raw kernel binary
gcc kallsyms.c -o kallsyms
chmod +x kallsyms
./kallsyms kernel > kallsyms.txt

# 2. Convert symbol table to ELF binary (for GDB symbol resolution)
python3 kallsyms2elf.py kallsyms.txt vmlinux.kallsyms.elf

# 3. Swap in the dpolicy from your vendor_boot.img into rootfs
./build-rootfs-defex.sh \
  --rootfs rootfs-defex.cpio.gz \
  --vendor-boot vendor_boot.img \
  --output rootfs-defex.cpio.gz

# 4. Edit run.sh → set QEMU="PATH_TO/qemu-system-aarch64-static-pc"

# 5. Boot QEMU instance
./run.sh

# 6. Debug with GDB (in another terminal)
./debug.sh
```

