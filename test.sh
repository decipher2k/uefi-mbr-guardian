#!/bin/bash
# ================================================================
# MBR Guardian - QEMU Test Script
#
# Tests the UEFI application in a virtual machine with OVMF.
# Creates a test disk with a fake MBR for testing snapshot logic.
# ================================================================

set -e

BUILDDIR="build"
EFI_FILE="$BUILDDIR/mbr-guardian.efi"
TEST_DIR="test"
ESP_IMG="$TEST_DIR/esp.img"
DISK_IMG="$TEST_DIR/testdisk.img"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE.fd"
OVMF_VARS_ORIG="/usr/share/OVMF/OVMF_VARS.fd"
OVMF_VARS="$TEST_DIR/OVMF_VARS.fd"

# Check prerequisites
check_deps() {
    local missing=()
    command -v qemu-system-x86_64 >/dev/null || missing+=("qemu-system-x86_64")
    [ -f "$OVMF_CODE" ] || missing+=("OVMF (apt install ovmf)")
    [ -f "$EFI_FILE" ] || missing+=("$EFI_FILE (run 'make' first)")

    if [ ${#missing[@]} -gt 0 ]; then
        echo "ERROR: Missing dependencies:"
        for dep in "${missing[@]}"; do
            echo "  - $dep"
        done
        exit 1
    fi
}

# Create a FAT32 ESP image with our EFI application
create_esp() {
    echo "Creating ESP image..."
    mkdir -p "$TEST_DIR"

    # 64MB ESP
    dd if=/dev/zero of="$ESP_IMG" bs=1M count=64 2>/dev/null
    mkfs.vfat -F 32 "$ESP_IMG"

    # Mount and copy EFI file
    local mnt=$(mktemp -d)
    sudo mount -o loop "$ESP_IMG" "$mnt"
    sudo mkdir -p "$mnt/EFI/BOOT"
    sudo mkdir -p "$mnt/EFI/mbr-guardian/snapshots"
    sudo cp "$EFI_FILE" "$mnt/EFI/BOOT/BOOTX64.EFI"
    sudo cp "$EFI_FILE" "$mnt/EFI/mbr-guardian/"
    sudo umount "$mnt"
    rmdir "$mnt"

    echo "ESP image created: $ESP_IMG"
}

# Create a test disk with a populated MBR
create_test_disk() {
    echo "Creating test disk with MBR..."

    # 1GB test disk
    dd if=/dev/zero of="$DISK_IMG" bs=1M count=1024 2>/dev/null

    # Write a fake MBR with partition table
    python3 -c "
import struct

mbr = bytearray(512)

# Boot code area (fake)
boot_msg = b'MBR Guardian Test Disk - Not bootable'
mbr[0:len(boot_msg)] = boot_msg

# Partition 1: NTFS, starts at sector 2048, 500MB
entry1_offset = 446
mbr[entry1_offset + 0] = 0x80  # Active/bootable
mbr[entry1_offset + 4] = 0x07  # NTFS
struct.pack_into('<I', mbr, entry1_offset + 8, 2048)          # LBA start
struct.pack_into('<I', mbr, entry1_offset + 12, 1024000)      # Sectors

# Partition 2: Linux, starts after P1, 400MB
entry2_offset = 446 + 16
mbr[entry2_offset + 0] = 0x00  # Not active
mbr[entry2_offset + 4] = 0x83  # Linux
struct.pack_into('<I', mbr, entry2_offset + 8, 1026048)       # LBA start
struct.pack_into('<I', mbr, entry2_offset + 12, 819200)       # Sectors

# MBR signature
mbr[510] = 0x55
mbr[511] = 0xAA

with open('$DISK_IMG', 'r+b') as f:
    f.write(bytes(mbr))

print('Test MBR written with NTFS + Linux partitions')
"

    echo "Test disk created: $DISK_IMG"
}

# Run QEMU with OVMF
run_qemu() {
    echo ""
    echo "========================================="
    echo "  Starting QEMU with MBR Guardian"
    echo "========================================="
    echo ""
    echo "  ESP:  $ESP_IMG"
    echo "  Disk: $DISK_IMG"
    echo "  OVMF: $OVMF_CODE"
    echo ""
    echo "  TIP: Enable CSM in the UEFI setup if"
    echo "       legacy boot doesn't work."
    echo ""

    # Copy OVMF_VARS so we can modify it
    cp "$OVMF_VARS_ORIG" "$OVMF_VARS"

    qemu-system-x86_64 \
        -machine q35 \
        -m 512M \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
        -drive if=pflash,format=raw,file="$OVMF_VARS" \
        -drive format=raw,file="$ESP_IMG" \
        -drive format=raw,file="$DISK_IMG" \
        -net none \
        -serial stdio \
        -vga std
}

# Main
case "${1:-run}" in
    setup)
        check_deps
        create_esp
        create_test_disk
        echo "Setup complete. Run: $0 run"
        ;;
    run)
        check_deps
        [ -f "$ESP_IMG" ] || create_esp
        [ -f "$DISK_IMG" ] || create_test_disk
        run_qemu
        ;;
    clean)
        rm -rf "$TEST_DIR"
        echo "Test files cleaned."
        ;;
    *)
        echo "Usage: $0 [setup|run|clean]"
        ;;
esac
