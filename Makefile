# MBR Guardian v2.0 - Graphical UEFI Boot Manager
# Build with gnu-efi

ARCH		= x86_64
TARGET		= mbr-guardian.efi

# gnu-efi paths (adjust to your system)
GNUEFI_DIR	= /usr/lib
GNUEFI_INC	= /usr/include/efi
GNUEFI_LIB	= $(GNUEFI_DIR)
GNUEFI_CRT	= $(GNUEFI_LIB)/crt0-efi-$(ARCH).o
GNUEFI_LDS	= $(GNUEFI_LIB)/elf_$(ARCH)_efi.lds

CC		= gcc
LD		= ld
OBJCOPY		= objcopy

CFLAGS		= -I$(GNUEFI_INC) \
		  -I$(GNUEFI_INC)/$(ARCH) \
		  -I$(GNUEFI_INC)/protocol \
		  -Isrc \
		  -DEFI_FUNCTION_WRAPPER \
		  -fno-stack-protector \
		  -fpic \
		  -fshort-wchar \
		  -mno-red-zone \
		  -Wall \
		  -Wextra \
		  -DGNU_EFI_USE_MS_ABI \
		  -maccumulate-outgoing-args \
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
		  -j .dynamic \
		  -j .dynsym \
		  -j .rel \
		  -j .rela \
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

$(BUILDDIR)/main.o: $(SRCDIR)/main.c $(SRCDIR)/gfx.h $(SRCDIR)/font8x16.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILDDIR)/gfx.o: $(SRCDIR)/gfx.c $(SRCDIR)/gfx.h $(SRCDIR)/font8x16.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $<

$(SO_TARGET): $(OBJECTS)
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
	cp $(BUILDDIR)/$(TARGET) $(ESP)/EFI/mbr-guardian/
	cp $(BUILDDIR)/$(TARGET) $(ESP)/EFI/BOOT/BOOTX64.EFI 2>/dev/null || true
	@if [ -d icons ] && [ "$$(ls -A icons/*.bmp 2>/dev/null)" ]; then \
		cp icons/*.bmp $(ESP)/EFI/mbr-guardian/icons/; \
		echo "  Copied icon files."; \
	fi
	@# Automatically create UEFI boot entry
	@ESP_DISK=$$(df $(ESP) 2>/dev/null | tail -1 | awk '{print $$1}' | sed 's/[0-9]*$$//'); \
	ESP_PART=$$(df $(ESP) 2>/dev/null | tail -1 | awk '{print $$1}' | grep -o '[0-9]*$$'); \
	if command -v efibootmgr >/dev/null 2>&1 && [ -n "$$ESP_DISK" ] && [ -n "$$ESP_PART" ]; then \
		echo "Creating UEFI boot entry..."; \
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

# Hint for custom icons
icons:
	@echo "To add custom icons:"
	@echo "  1. Create 64x64 BMP files (24-bit or 32-bit, uncompressed)"
	@echo "  2. Place them in icons/ or directly on ESP:"
	@echo "     \\EFI\\mbr-guardian\\icons\\windows.bmp"
	@echo "     \\EFI\\mbr-guardian\\icons\\linux.bmp"
	@echo "  3. Select icons via the GUI (right-click a tile or press I)"
