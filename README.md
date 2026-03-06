# MBR Guardian v2.0

**Graphical UEFI Boot Manager with Automatic MBR Versioning**

MBR Guardian resides safely on the EFI System Partition, monitors the MBR of all hard drives on every boot, automatically creates snapshots when changes are detected, and allows booting any saved MBR state via CSM chainloading — all through a graphical interface with mouse and keyboard control.

## Screenshots (conceptual)

```
╔══════════════════════════════════════════════════════════════╗
║  MBR GUARDIAN                          Disks: 2  Snapshots: 3║
╠══════════════════════════════════════════════════════════════╣
║                                                              ║
║  ┌──────────┐  ┌──────────┐  ┌──────────┐                   ║
║  │  ┌────┐  │  │  ┌────┐  │  │  ┌────┐  │                   ║
║  │  │ W  │  │  │  │ 🐧 │  │  │  │ BD │  │                   ║
║  │  └────┘  │  │  └────┘  │  │  └────┘  │                   ║
║  │Windows 11│  │Arch Linux│  │ FreeBSD  │                   ║
║  │Disk 0    │  │Disk 0    │  │Disk 1    │                   ║
║  │2026-03-01│  │2026-03-03│  │2026-03-04│                   ║
║  └──────────┘  └──────────┘  └──────────┘                   ║
║                                                              ║
║  Ready. Select a snapshot to boot.                           ║
╠══════════════════════════════════════════════════════════════╣
║  [Boot] [Snapshot] [Rename] [Icon] [Delete] [Hex] [Rescan]  ║
║  [UEFI Boot] [Reboot] [Shutdown]                             ║
╚══════════════════════════════════════════════════════════════╝
```

## Features

### Graphical Interface
- **Tile-based layout**: Each MBR snapshot displayed as a visual tile with icon, label, and info
- **Mouse control**: Hover effects, left-click to select, double-click to boot, right-click to change icon
- **Keyboard control**: Arrow keys for navigation, Enter to boot, letter shortcuts (B=Boot, S=Snapshot, R=Rename, I=Icon, D=Delete, H=Hex, Q=Quit)
- **Dark theme**: Eye-friendly dark color palette with accent colors
- **Dialogs**: Modal dialogs for confirmations, input, and file selection

### MBR Management
- **Auto-detection**: Automatically detects MBR changes on every boot (FNV-1a hash)
- **Auto-labeling**: Identifies the OS type (NTFS→Windows, ext4→Linux, etc.) and labels snapshots automatically
- **Up to 32 snapshots**: Multiple MBR states per system
- **Multi-disk**: Monitors all connected hard drives
- **Hex dump**: Color-coded MBR hex dump (partition table in yellow, signature in green)

### Icons
- **Automatic icons**: Generates color-coded icons based on partition type
- **Custom icons**: Assignable custom BMP files (64×64, 24/32-bit)
- **Icon browser**: Graphical dialog for icon selection from the `icons` directory
- **Right-click**: Quick access to icon change

## Architecture

```
src/
├── main.c          Main program: MBR logic, GUI menu, event loop
├── gfx.c           Graphics subsystem: framebuffer, drawing, BMP, mouse, widgets
├── gfx.h           Graphics API: types, colors, constants, function declarations
└── font8x16.h      Embedded bitmap font (CP437, 8×16px)

ESP Layout:
\EFI\mbr-guardian\
├── mbr-guardian.efi    The UEFI application
├── snapshots\          Saved MBR dumps
│   ├── disk0_A1B2C3D4.mbr
│   └── disk0_E5F6A7B8.mbr
└── icons\              Custom BMP icons
    ├── windows.bmp
    ├── linux.bmp
    └── freebsd.bmp
```

## Graphics Subsystem

Rendering uses the UEFI Graphics Output Protocol (GOP) directly:

- **Double buffering** via `Blt()` for flicker-free display
- **Framebuffer rendering**: Pixel-based drawing in a back buffer
- **Rounded rectangles**: For tiles, buttons, and dialogs
- **Alpha blending**: For shadows and modal dim overlays
- **Vertical/horizontal gradients**: For background and title bar
- **BMP loader**: Loads 24-bit and 32-bit uncompressed BMP files
- **Mouse cursor**: Pixel-accurate sprite with black/white outline
- **Text rendering**: Embedded 8×16 bitmap font, scalable (1×, 2×, 3×)

### Mouse Support

The mouse uses `EFI_SIMPLE_POINTER_PROTOCOL`:
- Relative movements are converted to screen coordinates
- Left-click: Select / press buttons
- Right-click: Context action (change icon)
- If no mouse is detected, everything also works via keyboard only

## Building

```bash
# Dependencies
sudo apt install gnu-efi gcc make    # Debian/Ubuntu
sudo dnf install gnu-efi-devel       # Fedora
sudo pacman -S gnu-efi-libs          # Arch

# Compile
make

# Optional: verify required sections are present in the final EFI image
objdump -h build/mbr-guardian.efi | grep -E '\\.(text|data|rodata|reloc|rela)'

# Install (creates boot entry automatically)
sudo make install ESP=/boot/efi
```

If you still see a black screen at launch, rebuild from a clean tree and re-check sections:

```bash
make clean && make
objdump -h build/mbr-guardian.efi | grep -E '\\.(rodata|reloc|rela)'
```

Try the alternate ABI build once (some toolchain/firmware combinations need it):

```bash
make clean && make USE_MS_ABI=0
sudo make install ESP=/boot/efi
```

If it is still black, run a text-only diagnostic build (disables GOP completely):

```bash
make clean && make NO_GFX_BOOT=1
sudo make install ESP=/boot/efi
```

Expected result: you should see startup text and a "graphics disabled" message.

If it is still black even in text-only mode, test without `InitializeLib`:

```bash
make clean && make NO_GFX_BOOT=1 SKIP_INITIALIZELIB=1
sudo make install ESP=/boot/efi
```

Expected result: you should see startup text and an "InitializeLib skipped" message.

If screen output stays black in every mode, run a pure entry-point reset test:

```bash
make clean && make ENTRY_RESET_TEST=1
sudo make install ESP=/boot/efi
```

Expected result: selecting `MBR Guardian` should immediately reboot the machine.
If it does not reboot, firmware is not executing this binary path.

Also verify you are booting the updated binary (not an older copy):

```bash
sudo ls -l /boot/efi/EFI/mbr-guardian/mbr-guardian.efi
sudo ls -l /boot/efi/EFI/BOOT/BOOTX64.EFI
sudo efibootmgr -v | grep -i 'MBR Guardian\|mbr-guardian.efi\|BOOTX64.EFI'
```

If multiple `MBR Guardian` entries exist on different partitions, delete stale ones and recreate one:

```bash
sudo efibootmgr -v | grep -i 'MBR Guardian'
# Example cleanup (replace #### with entry numbers shown above)
sudo efibootmgr -b #### -B
sudo efibootmgr -c -d /dev/nvme0n1 -p 3 -l '\EFI\mbr-guardian\mbr-guardian.efi' -L 'MBR Guardian'
```

To verify whether the EFI app executes at all (even with a black screen), check the boot marker variable:

```bash
ls /sys/firmware/efi/efivars | grep -i MBRGSeen
```

Boot once into `MBR Guardian`, reboot back to Linux, and run the command again. If `MBRGSeen-*` appears (or keeps changing), the app reached `efi_main` and the failure is later in startup.

### Windows Installation

```powershell
# Run as Administrator:
powershell -ExecutionPolicy Bypass -File install.ps1
```

The script:
1. Automatically mounts the EFI System Partition
2. Copies `mbr-guardian.efi` to `\EFI\mbr-guardian\`
3. Creates a UEFI boot entry via `bcdedit`

### Manual UEFI Boot Entry (Linux)

If `efibootmgr` does not work automatically during installation:

```bash
sudo efibootmgr -c -d /dev/sda -p 1 \
    -l '\EFI\mbr-guardian\mbr-guardian.efi' \
    -L 'MBR Guardian'
```

### Manual UEFI Boot Entry (UEFI Setup)

If no OS-level tool works:
1. Reboot into UEFI Setup (F2/DEL at startup, or on Windows: Shift+Restart → Troubleshoot → UEFI Firmware Settings)
2. In the boot menu, select "Add Boot Option"
3. Choose file: `\EFI\mbr-guardian\mbr-guardian.efi`
4. Set name: `MBR Guardian`
5. Save and adjust boot order

## Creating Custom Icons

Icons must have the following format:
- **BMP format** (Windows Bitmap)
- **24-bit or 32-bit** color depth
- **Uncompressed** (no RLE)
- **Recommended size**: 64×64 pixels (automatically scaled)

```bash
# Example with ImageMagick:
convert -size 64x64 xc:navy \
    -fill white -font DejaVu-Sans-Bold -pointsize 36 \
    -gravity center -annotate +0+0 "W" \
    BMP3:icons/windows.bmp

# Copy to ESP:
sudo cp icons/*.bmp /boot/efi/EFI/mbr-guardian/icons/
```

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| ← → | Select snapshot |
| ↑ ↓ | Scroll |
| Enter / B | Boot selected snapshot |
| S | Manual snapshot |
| R | Rename |
| I | Change icon |
| D | Delete |
| H | Show hex dump |
| F5 | Rescan disks |
| ESC | Deselect |
| Q | Continue to normal UEFI boot |

## Requirements

- **UEFI firmware** with GOP (Graphics Output Protocol)
- **CSM** enabled for legacy boot (most boards 2012–2022)
- **Secure Boot** disabled (or MOK enrolled)
- **gnu-efi** for compiling

## Limitations

- CSM required for legacy boot. Newer systems (Win11, Mac) may have removed CSM.
- BMP icons must be uncompressed (no RLE, no PNG).
- No Secure Boot without custom signing.
- Legacy boot handoff is firmware-dependent; tested with AMI/Phoenix/Insyde.

## License

MIT License
