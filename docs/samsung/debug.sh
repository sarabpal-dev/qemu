#!/bin/sh
#
# debug.sh — Attach GDB to a running QEMU instance
#
# Usage: ./debug.sh [optional-vmlinux-path]
#
# The Samsung production kernel has no vmlinux — only kallsyms.txt.
# This script auto-generates a symbol ELF from kallsyms.txt if needed.
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── User-configurable ───────────────────────────────────────────────────
GDB="gdb-multiarch"         # gdb-multiarch for x86 PC

# ── Find vmlinux or generate from kallsyms ──────────────────────────────
if [ -z "$1" ]; then
    VMLINUX="$SCRIPT_DIR/vmlinux"
else
    VMLINUX="$1/vmlinux"
fi

if [ ! -f "$VMLINUX" ]; then
    KALLSYMS="$SCRIPT_DIR/kallsyms.txt"
    KALLSYMS_ELF="$SCRIPT_DIR/vmlinux.kallsyms.elf"
    if [ -f "$KALLSYMS" ]; then
        if [ ! -f "$KALLSYMS_ELF" ] || [ "$KALLSYMS" -nt "$KALLSYMS_ELF" ]; then
            echo "Generating $KALLSYMS_ELF from kallsyms.txt ..."
            python3 "$SCRIPT_DIR/kallsyms2elf.py" "$KALLSYMS" "$KALLSYMS_ELF" || exit 1
        fi
        VMLINUX="$KALLSYMS_ELF"
        echo "No vmlinux - using symbols from $VMLINUX"
    fi
fi

if [ ! -f "$VMLINUX" ]; then
    echo "Error: vmlinux not found at $VMLINUX (and no kallsyms.txt to convert)" >&2
    exit 1
fi

exec "$GDB" "$VMLINUX" \
    -ex "set confirm off" \
    -ex "set detach-on-quit off" \
    -ex "set architecture aarch64" \
    -ex "set pagination off" \
    -ex "target remote :1234" \
    -ex "break start_kernel"
