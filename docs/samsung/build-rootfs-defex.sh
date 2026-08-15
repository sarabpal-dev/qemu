#!/bin/bash
set -euo pipefail
#
# build-rootfs-defex.sh -- Swap DEFEX dpolicy in a rootfs-defex.cpio.gz
#
# Takes an existing rootfs-defex.cpio.gz (which already contains the full
# Buildroot userspace, SELinux policy, and device nodes) and swaps the
# DEFEX dpolicy with one extracted from a new vendor_boot.img.
#
# This lets you pair a kernel with its matching dpolicy — required because
# DEFEX policies are cryptographically bound to the kernel build.
#
# Usage:
#   ./build-rootfs-defex.sh \
#       --rootfs rootfs-defex.cpio.gz \
#       --vendor-boot vendor_boot.img \
#       [--output rootfs-defex.cpio.gz]
#
# Requirements:
#   unpack_bootimg (pip install mkbootimg_tools  or  apt install mkbootimg)
#   lz4            (apt install lz4)
#   cpio, gzip, file
#

OUTPUT="rootfs-defex.cpio.gz"
ROOTFS=""
VENDOR_BOOT=""

usage() {
    echo "Usage: $0 --rootfs <rootfs-defex.cpio.gz> --vendor-boot <vendor_boot.img> [--output <out.cpio.gz>]"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rootfs)      ROOTFS="$2"; shift 2;;
        --vendor-boot) VENDOR_BOOT="$2"; shift 2;;
        --output)      OUTPUT="$2"; shift 2;;
        *)             usage;;
    esac
done

if [ -z "$ROOTFS" ] || [ -z "$VENDOR_BOOT" ]; then
    usage
fi

for f in "$ROOTFS" "$VENDOR_BOOT"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: File not found: $f" >&2
        exit 1
    fi
done

# Tool checks
for tool in unpack_bootimg lz4 cpio gzip file; do
    if ! command -v "$tool" &>/dev/null; then
        echo "ERROR: Required tool '$tool' not found." >&2
        case "$tool" in
            unpack_bootimg) echo "  Install: pip install mkbootimg_tools  or  apt install mkbootimg" >&2 ;;
            lz4)            echo "  Install: apt install lz4" >&2 ;;
        esac
        exit 1
    fi
done

# -- Temporary workspace --------------------------------------------------
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

# ── Step 1: Extract dpolicy from vendor_boot.img ────────────────────────
echo "[1/3] Extracting dpolicy from vendor_boot.img ..."

VBOOTDIR="$WORKDIR/vendor_boot_extract"
mkdir -p "$VBOOTDIR"

unpack_bootimg --boot_img "$VENDOR_BOOT" --out "$VBOOTDIR" 2>/dev/null

EXTRACT_DIR="$VBOOTDIR/ramdisk_extract"
mkdir -p "$EXTRACT_DIR"

FOUND_RAMDISK=false
for RAMDISK_FILE in $(find "$VBOOTDIR" -maxdepth 1 -name 'vendor_ramdisk*' -type f 2>/dev/null | sort); do
    FOUND_RAMDISK=true
    RAMDISK_TYPE=$(file -b "$RAMDISK_FILE" 2>/dev/null)
    if echo "$RAMDISK_TYPE" | grep -qi 'LZ4'; then
        echo "  Vendor ramdisk ($RAMDISK_FILE): LZ4 compressed"
        lz4 -d "$RAMDISK_FILE" - 2>/dev/null | (cd "$EXTRACT_DIR" && cpio -idmu 2>/dev/null) || true
    elif echo "$RAMDISK_TYPE" | grep -qi 'gzip'; then
        echo "  Vendor ramdisk ($RAMDISK_FILE): gzip compressed"
        gunzip -c "$RAMDISK_FILE" 2>/dev/null | (cd "$EXTRACT_DIR" && cpio -idmu 2>/dev/null) || true
    else
        echo "  Vendor ramdisk ($RAMDISK_FILE): trying raw cpio ..."
        (cd "$EXTRACT_DIR" && cpio -idmu < "$RAMDISK_FILE" 2>/dev/null) || true
    fi
    DPOLICY=$(find "$EXTRACT_DIR" -name dpolicy -type f 2>/dev/null | head -1)
    if [ -n "$DPOLICY" ]; then
        break
    fi
done

if [ "$FOUND_RAMDISK" = false ]; then
    echo "ERROR: No vendor_ramdisk file found in vendor_boot.img" >&2
    ls -la "$VBOOTDIR" >&2
    exit 1
fi

DPOLICY=$(find "$EXTRACT_DIR" -name dpolicy -type f 2>/dev/null | head -1)
if [ -z "$DPOLICY" ]; then
    echo "ERROR: dpolicy not found in vendor ramdisk." >&2
    echo "  Contents of vendor ramdisk:" >&2
    find "$EXTRACT_DIR" -type f 2>/dev/null | head -20 >&2
    exit 1
fi

echo "  dpolicy: $DPOLICY ($(stat -c%s "$DPOLICY") bytes)"

# Verify DEFEX magic
if head -c8 "$DPOLICY" 2>/dev/null | grep -q 'DEFEX'; then
    echo "  Magic: $(head -c8 "$DPOLICY")"
else
    echo "  WARNING: dpolicy doesn't start with 'DEFEX' magic." >&2
fi

# ── Step 2: Extract base rootfs-defex.cpio.gz ────────────────────────────
echo "[2/4] Extracting base rootfs-defex.cpio.gz ..."

ROOTFSDIR="$WORKDIR/rootfs"
mkdir -p "$ROOTFSDIR"
# cpio -idmu will fail on device nodes (mknod needs root). We ignore those
# errors and regenerate device nodes in step 3.
gunzip -c "$ROOTFS" | (cd "$ROOTFSDIR" && cpio -idmu 2>/dev/null) || true

# Remove broken device-node placeholders (cpio created them as empty files on failure)
for f in console null zero kmsg tty ttyAMA0 urandom random full mem ptmx; do
    rm -f "$ROOTFSDIR/dev/$f" 2>/dev/null || true
done

# Fix /var/empty permissions
mkdir -p "$ROOTFSDIR/var/empty"
chmod 755 "$ROOTFSDIR/var/empty"
chmod 755 "$ROOTFSDIR/var"

echo "  Base extracted (device nodes will be regenerated)"

# ── Step 3: Generate device nodes (cpio binary, no root needed) ─────────
echo "[3/4] Generating device nodes ..."

DEV_CPIO="$WORKDIR/devnodes.cpio"

python3 - "$DEV_CPIO" << 'PYHELPER'
import sys, struct, time

def pad4(n): return (n + 3) & ~3

def cpio_newc_header(filename, mode, uid, gid, nlink, filesize,
                     devmajor, devminor, rdevmajor, rdevminor):
    namesize = len(filename) + 1
    hdr = struct.pack('6s8s8s8s8s8s8s8s8s8s8s8s8s8s',
        b'070701',
        f'{0:08x}'.encode(), f'{mode:08x}'.encode(),
        f'{uid:08x}'.encode(), f'{gid:08x}'.encode(),
        f'{nlink:08x}'.encode(), f'{int(time.time()):08x}'.encode(),
        f'{filesize:08x}'.encode(), f'{devmajor:08x}'.encode(),
        f'{devminor:08x}'.encode(), f'{rdevmajor:08x}'.encode(),
        f'{rdevminor:08x}'.encode(), f'{namesize:08x}'.encode(),
        f'{0:08x}'.encode(),
    )
    out = hdr + filename.encode() + b'\0'
    out += b'\0' * (pad4(110 + namesize) - (110 + namesize))
    return out

def cpio_trailer():
    return cpio_newc_header('TRAILER!!!', 0, 0, 0, 1, 0, 0, 0, 0, 0)

S_IFCHR = 0o20000
devnodes = [
    ('dev/console',  0o622, 0, 0, 1, 5,   1),
    ('dev/null',     0o666, 0, 0, 1, 1,   3),
    ('dev/zero',     0o666, 0, 0, 1, 1,   5),
    ('dev/kmsg',     0o600, 0, 0, 1, 1,  11),
    ('dev/tty',      0o666, 0, 0, 1, 5,   0),
    ('dev/ttyAMA0',  0o666, 0, 0, 1, 204, 64),
    ('dev/urandom',  0o666, 0, 0, 1, 1,   9),
    ('dev/random',   0o666, 0, 0, 1, 1,   8),
    ('dev/full',     0o666, 0, 0, 1, 1,   7),
    ('dev/mem',      0o600, 0, 0, 1, 1,   1),
    ('dev/ptmx',     0o666, 0, 0, 1, 5,   2),
]

entries = []
for name, perm, uid, gid, nlink, major, minor in devnodes:
    mode = S_IFCHR | perm
    entries.append(cpio_newc_header(name, mode, uid, gid, nlink, 0, 0, 0, major, minor))
entries.append(cpio_trailer())

with open(sys.argv[1], 'wb') as f:
    f.write(b''.join(entries))
print(f"Generated {len(entries)-1} device nodes", file=sys.stderr)
PYHELPER

echo "  Device nodes: $(stat -c%s "$DEV_CPIO") bytes"

# ── Step 4: Swap dpolicy ─────────────────────────────────────────────────
echo "[4/4] Swapping dpolicy ..."

OLD_DP="$ROOTFSDIR/first_stage_ramdisk/dpolicy"
if [ -f "$OLD_DP" ]; then
    echo "  Old dpolicy: $(stat -c%s "$OLD_DP") bytes → replaced"
    rm -f "$OLD_DP"
fi

mkdir -p "$ROOTFSDIR/first_stage_ramdisk"
cp "$DPOLICY" "$ROOTFSDIR/first_stage_ramdisk/dpolicy"
chmod 644 "$ROOTFSDIR/first_stage_ramdisk/dpolicy"
echo "  New dpolicy: $(stat -c%s "$ROOTFSDIR/first_stage_ramdisk/dpolicy") bytes ($(head -c8 "$DPOLICY"))"

# ── Step 5: Repack — regular files + device nodes + gzip ────────────────
echo "[5/5] Repacking as $OUTPUT ..."

REG_CPIO="$WORKDIR/regular.cpio"
(cd "$ROOTFSDIR" && find . | cpio -o -H newc 2>/dev/null) > "$REG_CPIO"

# Fix UIDs/GIDs to 0 (root:root) and /var/empty permissions in regular.cpio
FIXED_CPIO="$WORKDIR/regular_fixed.cpio"
python3 - "$REG_CPIO" "$FIXED_CPIO" << 'PYFIX'
import sys, struct

def pad4(n): return (n + 3) & ~3

infile = sys.argv[1]
outfile = sys.argv[2]

with open(infile, 'rb') as f:
    data = f.read()

pos = 0
out = bytearray()

while pos < len(data):
    if pos + 110 > len(data):
        out.extend(data[pos:])
        break
    
    magic = data[pos:pos+6]
    if magic != b'070701':
        out.extend(data[pos:])
        break

    hdr = data[pos:pos+110]
    mode = int(hdr[14:22], 16)
    filesize = int(hdr[54:62], 16)
    namesize = int(hdr[94:102], 16)

    header_and_name_len = pad4(110 + namesize)
    data_len = pad4(filesize)
    total_entry_len = header_and_name_len + data_len

    if pos + total_entry_len > len(data):
        out.extend(data[pos:])
        break

    name_raw = data[pos+110 : pos+110+namesize]
    name = name_raw.rstrip(b'\0').decode('utf-8', errors='ignore')

    # Force UID=0 and GID=0 (root:root)
    new_uid = 0
    new_gid = 0

    # Ensure var/empty is 0755 (S_IFDIR 0o40000 | 0o755)
    new_mode = mode
    if name in ('./var/empty', 'var/empty', './var/empty/'):
        new_mode = (mode & ~0o777) | 0o755

    new_hdr = (
        hdr[:14] +
        f'{new_mode:08x}'.encode() +
        f'{new_uid:08x}'.encode() +
        f'{new_gid:08x}'.encode() +
        hdr[38:]
    )

    out.extend(new_hdr)
    out.extend(data[pos+110 : pos+total_entry_len])

    pos += total_entry_len

    if name == 'TRAILER!!!':
        break

with open(outfile, 'wb') as f:
    f.write(out)
PYFIX

REG_CPIO="$FIXED_CPIO"

# Strip TRAILER!!!
REG_CPIO_TRUNC="$WORKDIR/regular_no_trailer.cpio"
REG_SIZE=$(stat -c%s "$REG_CPIO")
TRAILER_POS=$(python3 -c "
data = open('$REG_CPIO', 'rb').read()
idx = data.rfind(b'TRAILER!!!')
print(idx - 110 if idx >= 110 else 0)
")
if [ "$TRAILER_POS" -gt 0 ]; then
    head -c "$TRAILER_POS" "$REG_CPIO" > "$REG_CPIO_TRUNC"
else
    cp "$REG_CPIO" "$REG_CPIO_TRUNC"
fi

cat "$REG_CPIO_TRUNC" "$DEV_CPIO" | gzip > "$OUTPUT"

echo ""
echo "Done: $OUTPUT ($(stat -c%s "$OUTPUT") bytes)"
gunzip -c "$OUTPUT" | cpio -t 2>/dev/null | grep dpolicy | head -1
echo ""
echo "now run cp $OUTPUT . to copy to current dir"

