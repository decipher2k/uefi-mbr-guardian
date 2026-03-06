# MBR Guardian v2.0 - Graphical UEFI Boot Manager
# Build with gnu-efi

ARCH		= x86_64
TARGET		= mbr-guardian.efi

# gnu-efi paths (adjust to your system)
GNUEFI_DIR	= /usr/lib
GNUEFI_INC	= /usr/include/efi
GNUEFI_LIB	= $(GNUEFI_DIR)
GNUEFI_LDS	= $(GNUEFI_LIB)/elf_$(ARCH)_efi.lds

# Use our custom CRT0 that matches the MS ABI build setting.
# The stock system crt0-efi-x86_64.o is compiled WITHOUT MS ABI,
# causing a calling-convention mismatch that crashes efi_main.
GNUEFI_CRT	= $(BUILDDIR)/crt0-efi-$(ARCH).o

CC		= gcc
LD		= ld
OBJCOPY		= objcopy

# Some toolchains/firmware combinations are sensitive to ABI setup.
# Default keeps current behavior; override with `make USE_MS_ABI=0`.
USE_MS_ABI	?= 1
NO_GFX_BOOT	?= 0
SKIP_INITIALIZELIB ?= 0
ENTRY_RESET_TEST ?= 0

ABI_CFLAGS	=
ifeq ($(USE_MS_ABI),1)
ABI_CFLAGS	+= -DGNU_EFI_USE_MS_ABI -maccumulate-outgoing-args
endif

DIAG_CFLAGS	=
ifeq ($(NO_GFX_BOOT),1)
DIAG_CFLAGS	+= -DMBRG_NO_GFX_BOOT=1
endif
ifeq ($(SKIP_INITIALIZELIB),1)
DIAG_CFLAGS	+= -DMBRG_SKIP_INITIALIZELIB=1
endif
ifeq ($(ENTRY_RESET_TEST),1)
DIAG_CFLAGS	+= -DMBRG_ENTRY_RESET_TEST=1
endif

CFLAGS		= -I$(GNUEFI_INC) \
		  -I$(GNUEFI_INC)/$(ARCH) \
		  -I$(GNUEFI_INC)/protocol \
		  -Isrc \
		  -DEFI_FUNCTION_WRAPPER \
		  -fno-stack-protector \
		  -fpic \
		  -fshort-wchar \
		  -mno-red-zone \
		  -O2 \
		  -Wall \
		  -Wextra \
		  $(ABI_CFLAGS) \
		  $(DIAG_CFLAGS) \
		  -c

LDFLAGS		= -nostdlib \
		  -znocombreloc \
		  -T $(GNUEFI_LDS) \
		  -shared \
		  -Bsymbolic \
		  -L $(GNUEFI_LIB) \
		  $(GNUEFI_CRT)

LIBS		= -lefi -lgnuefi

OBJCOPY_FLAGS	= -j .text \
		  -j .sdata \
		  -j .data \
		  -j .rodata \
		  -j .rodata.* \
		  -j .gnu.linkonce.r.* \
		  -j .dynamic \
		  -j .dynsym \
		  -j .rel \
		  -j .rel.* \
		  -j .rela \
		  -j .rela.* \
		  -j .reloc \
		  --target=efi-app-$(ARCH)

SRCDIR		= src
BUILDDIR	= build

SOURCES		= $(SRCDIR)/main.c $(SRCDIR)/gfx.c
OBJECTS		= $(BUILDDIR)/main.o $(BUILDDIR)/gfx.o
SO_TARGET	= $(BUILDDIR)/mbr-guardian.so

.PHONY: all clean install icons

all: $(BUILDDIR)/$(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/crt0-efi-$(ARCH).o: $(SRCDIR)/crt0-efi-$(ARCH).S | $(BUILDDIR)
	$(CC) -I$(GNUEFI_INC) -I$(GNUEFI_INC)/$(ARCH) $(ABI_CFLAGS) -c -o $@ $<

$(BUILDDIR)/main.o: $(SRCDIR)/main.c $(SRCDIR)/gfx.h $(SRCDIR)/font8x16.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILDDIR)/gfx.o: $(SRCDIR)/gfx.c $(SRCDIR)/gfx.h $(SRCDIR)/font8x16.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $<

$(SO_TARGET): $(GNUEFI_CRT) $(OBJECTS)
	$(LD) $(LDFLAGS) $(OBJECTS) -o $@ $(LIBS)

$(BUILDDIR)/$(TARGET): $(SO_TARGET)
	$(OBJCOPY) $(OBJCOPY_FLAGS) $< $@
	@echo ""
	@echo "╔═══════════════════════════════════════════╗"
	@echo "║   MBR Guardian v2.0 - Build Complete      ║"
	@echo "╚═══════════════════════════════════════════╝"
	@echo "  Output: $(BUILDDIR)/$(TARGET)"
	@echo ""

clean:
	rm -rf $(BUILDDIR)

# Install to EFI System Partition
ESP ?= /boot/efi

install: $(BUILDDIR)/$(TARGET)
	@echo "Installing to $(ESP)/EFI/mbr-guardian/"
	mkdir -p $(ESP)/EFI/mbr-guardian/snapshots
	mkdir -p $(ESP)/EFI/mbr-guardian/icons
	mkdir -p $(ESP)/EFI/BOOT
	cp $(BUILDDIR)/$(TARGET) $(ESP)/EFI/mbr-guardian/
	cp $(BUILDDIR)/$(TARGET) $(ESP)/EFI/BOOT/BOOTX64.EFI
	@if [ -d icons ] && [ "$$(ls -A icons/*.bmp 2>/dev/null)" ]; then \
		cp icons/*.bmp $(ESP)/EFI/mbr-guardian/icons/; \
		echo "  Copied icon files."; \
	fi
	@# Automatically create UEFI boot entry
	@ESP_DEV=$$(findmnt -no SOURCE $(ESP) 2>/dev/null); \
	ESP_DISK=""; \
	ESP_PART=""; \
	if [ -n "$$ESP_DEV" ]; then \
		ESP_PKNAME=$$(lsblk -no PKNAME "$$ESP_DEV" 2>/dev/null); \
		ESP_PART=$$(lsblk -no PARTN "$$ESP_DEV" 2>/dev/null); \
		if [ -n "$$ESP_PKNAME" ]; then ESP_DISK="/dev/$$ESP_PKNAME"; fi; \
	fi; \
	if command -v efibootmgr >/dev/null 2>&1 && [ -n "$$ESP_DISK" ] && [ -n "$$ESP_PART" ]; then \
		echo "Creating UEFI boot entry..."; \
		for b in $$(efibootmgr 2>/dev/null | awk '/MBR Guardian/ {print substr($$1,5,4)}' | tr -d '*'); do \
			efibootmgr -b $$b -B >/dev/null 2>&1 || true; \
		done; \
		efibootmgr -c -d $$ESP_DISK -p $$ESP_PART \
			-l '\EFI\mbr-guardian\mbr-guardian.efi' \
			-L 'MBR Guardian' 2>/dev/null && \
		echo "  UEFI boot entry 'MBR Guardian' created." || \
		echo "  Warning: Could not create boot entry. Run manually:"; \
		echo "    sudo efibootmgr -c -d $$ESP_DISK -p $$ESP_PART \\"; \
		echo "      -l '\\EFI\\mbr-guardian\\mbr-guardian.efi' -L 'MBR Guardian'"; \
	else \
		echo ""; \
		echo "efibootmgr not found or ESP disk not detected."; \
		echo "Create UEFI boot entry manually:"; \
		echo "  sudo efibootmgr -c -d /dev/sdX -p 1 \\"; \
		echo "    -l '\\EFI\\mbr-guardian\\mbr-guardian.efi' -L 'MBR Guardian'"; \
	fi
	@echo ""
	@echo "Done."

# ---- Minimal CRT0-free diagnostic binary ----
# Builds a tiny EFI app that immediately reboots.
# Uses NO gnu-efi CRT0, NO _relocate, NO libraries.
# If this reboots the machine, the problem is in CRT0/_relocate.
# If it does NOT reboot, the firmware refuses to load ANY binary
# we produce, or the boot entry path is wrong.

$(BUILDDIR)/minimal_test.o: $(SRCDIR)/minimal_test.c | $(BUILDDIR)
	$(CC) -I$(GNUEFI_INC) -I$(GNUEFI_INC)/$(ARCH) \
	      -fno-stack-protector -fpic -fshort-wchar -mno-red-zone \
	      -DGNU_EFI_USE_MS_ABI -maccumulate-outgoing-args \
	      -ffreestanding -Wall -O2 -c -o $@ $<

$(BUILDDIR)/minimal_test.so: $(BUILDDIR)/minimal_test.o
	$(LD) -nostdlib -znocombreloc -T $(GNUEFI_LDS) -shared -Bsymbolic \
	      -L $(GNUEFI_LIB) $(BUILDDIR)/minimal_test.o -o $@ \
	      --entry=efi_main --undefined=efi_main

$(BUILDDIR)/minimal-test.efi: $(BUILDDIR)/minimal_test.so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .rodata.* \
	           -j .reloc --target=efi-app-$(ARCH) $< $@
	@echo ""
	@echo "=== Minimal CRT0-free test binary built ==="
	@echo "  Output: $@"
	@echo "  Copy to ESP and boot to test firmware loading."
	@echo ""

minimal-test: $(BUILDDIR)/minimal-test.efi

install-minimal-test: $(BUILDDIR)/minimal-test.efi
	@echo "Installing minimal test to $(ESP)..."
	mkdir -p $(ESP)/EFI/mbr-guardian
	cp $(BUILDDIR)/minimal-test.efi $(ESP)/EFI/mbr-guardian/mbr-guardian.efi
	cp $(BUILDDIR)/minimal-test.efi $(ESP)/EFI/BOOT/BOOTX64.EFI
	@echo "Done. Reboot and select MBR Guardian. Machine should reboot immediately."

# Hint for custom icons
icons:
	@echo "To add custom icons:"
	@echo "  1. Create 64x64 BMP files (24-bit or 32-bit, uncompressed)"
	@echo "  2. Place them in icons/ or directly on ESP:"
	@echo "     \\EFI\\mbr-guardian\\icons\\windows.bmp"
	@echo "     \\EFI\\mbr-guardian\\icons\\linux.bmp"
	@echo "  3. Select icons via the GUI (right-click a tile or press I)"
