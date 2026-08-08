#!/bin/bash
set -euo pipefail
#
# run.sh — Boot Samsung Android kernel 5.10 in QEMU
#
# Copy this file to a working directory containing:
#   - kernel                       (extracted from boot.img)
#   - rootfs-defex.cpio.gz         (Buildroot + DEFEX dpolicy)
#
# Then run:  ./run.sh [debug|freeze|nodebug]
#

# ── User-configurable paths ─────────────────────────────────────────────
QEMU="PATH_TO/qemu-system-aarch64-static-pc"   # Change this to your QEMU binary

ROOTFS_CPIO_DEFEX="rootfs-defex.cpio.gz"       # rootfs with DEFEX dpolicy

# ── Sanity checks ───────────────────────────────────────────────────────
if [ ! -f "kernel" ]; then
  echo "ERROR: kernel not found" >&2
  exit 1
fi

if [ ! -f "$ROOTFS_CPIO_DEFEX" ]; then
  echo "ERROR: $ROOTFS_CPIO_DEFEX not found" >&2
  exit 1
fi

echo "Using initrd: $ROOTFS_CPIO_DEFEX (DEFEX rules included)"

# ── Build QEMU args ─────────────────────────────────────────────────────
ARGS=()

# Core machine setup
ARGS+=("-cpu" "cortex-a710,sve=off")
ARGS+=("-machine" "virt")
ARGS+=("-nographic")
ARGS+=("-smp" "8")
ARGS+=("-m" "12288")

# Kernel + initrd
ARGS+=("-kernel" "kernel")
ARGS+=("-initrd" "$ROOTFS_CPIO_DEFEX")

# Networking: USB CDC ethernet gadget via xHCI (production kernel has no virtio-net)
ARGS+=("-netdev" "user,id=net0,hostfwd=tcp::13337-:22")
ARGS+=("-device" "qemu-xhci,id=xhci")
ARGS+=("-device" "usb-net,netdev=net0,bus=xhci.0")

ARGS+=("-append" "console=ttyAMA0 earlycon root=/dev/ram0 log_buf_len=10M nokaslr loglevel=4")

# ── Debug flags ─────────────────────────────────────────────────────────
DEBUG=true
FREEZE=false

for opt in "$@"; do
  case "$opt" in
    debug)   DEBUG=true  ;;
    freeze)  FREEZE=true ;;
    nodebug) DEBUG=false ;;
    *)       echo "Warning: unknown option '$opt' (supported: debug, freeze, nodebug)" >&2 ;;
  esac
done

if [ "$DEBUG" = true ]; then
  ARGS+=("-s")
fi
if [ "$FREEZE" = true ]; then
  ARGS+=("-S")
fi

# ── Run ─────────────────────────────────────────────────────────────────
echo "Running: $QEMU ${ARGS[*]}"
exec "$QEMU" "${ARGS[@]}"
