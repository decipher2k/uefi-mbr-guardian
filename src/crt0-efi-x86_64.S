/*
 * crt0-efi-x86_64.S - Custom CRT0 for gnu-efi with MS ABI support
 *
 * The stock Ubuntu gnu-efi package provides crt0-efi-x86_64.o compiled
 * WITHOUT GNU_EFI_USE_MS_ABI.  That CRT0 converts the firmware's
 * MS-ABI arguments (rcx/rdx) to System V (rdi/rsi) before calling
 * efi_main.  When application code is compiled WITH GNU_EFI_USE_MS_ABI,
 * efi_main expects MS-ABI (rcx/rdx) — causing a mismatch that leads
 * to an immediate crash (garbage SystemTable pointer → page fault).
 *
 * This file is the standard gnu-efi CRT0 with the MS-ABI path,
 * matching our USE_MS_ABI=1 build setting.
 *
 * Entry:  UEFI firmware calls _start(rcx=ImageHandle, rdx=SystemTable)
 *         using the Microsoft x64 calling convention.
 * Flow:   _start → _relocate(ImageBase, &_DYNAMIC) → efi_main(IH, ST)
 * Exit:   efi_main's return value (rax) is passed back to the firmware.
 */

	.text
	.balign 4
	.globl _start
_start:
	subq	$8, %rsp		/* Align stack to 16 bytes */

	/* Save the firmware-provided arguments (MS ABI: rcx, rdx).  */
	pushq	%rcx			/* ImageHandle  */
	pushq	%rdx			/* SystemTable  */

	/*
	 * Call _relocate(ImageBase, &_DYNAMIC) using System V ABI
	 * (the _relocate function in libgnuefi.a is always SysV).
	 *   rdi = ImageBase  (runtime address of PE image base, RVA 0)
	 *   rsi = &_DYNAMIC  (start of the .dynamic section)
	 *
	 * ImageBase is defined at RVA 0 by the linker script
	 * (elf_x86_64_efi.lds).  Using _start here would be wrong
	 * because _start is at the beginning of .text (RVA 0x3000),
	 * not at the image base.
	 */
	lea	ImageBase(%rip), %rdi
	lea	_DYNAMIC(%rip), %rsi

	call	_relocate		/* EFI_STATUS _relocate(...) */

	/* Restore ImageHandle / SystemTable into MS-ABI registers.  */
	popq	%rdx			/* rdx = SystemTable  */
	popq	%rcx			/* rcx = ImageHandle  */

	/*
	 * If _relocate returned non-zero, skip efi_main and return
	 * the error status directly.
	 */
	testq	%rax, %rax
	jnz	.exit

	/*
	 * Call efi_main(ImageHandle, SystemTable) — MS ABI.
	 * rcx and rdx are already set from the pops above.
	 */
	call	efi_main

.exit:
	addq	$8, %rsp		/* Remove alignment padding */
	ret

/*
 * Minimal PE .reloc section — required by the UEFI firmware PE loader.
 *
 * Without this section the firmware cannot relocate the image and
 * silently refuses to execute it.  A single IMAGE_BASE_RELOCATION
 * block with one IMAGE_REL_BASED_ABSOLUTE entry (type 0, no-op)
 * satisfies the loader while keeping relocations handled by
 * _relocate() via the ELF .rela section above.
 *
 * Structure (12 bytes):
 *   DWORD  VirtualAddress  = 0
 *   DWORD  SizeOfBlock     = 12 (header + one 2-byte entry + 2 pad)
 *   WORD   TypeOffset[0]   = 0  (IMAGE_REL_BASED_ABSOLUTE)
 *   WORD   (padding)       = 0
 */
	.section .reloc, "a"
	.long	0			/* VirtualAddress  */
	.long	12			/* SizeOfBlock     */
	.long	0			/* TypeOffset + pad */
