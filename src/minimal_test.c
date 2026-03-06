/*
 * Minimal EFI test - bypasses gnu-efi CRT0 entirely.
 * Entry point is efi_main directly (no _start / _relocate).
 * Uses only RIP-relative addressing (x86_64 default) so no
 * relocations are needed.
 *
 * If this binary causes a reboot when selected from the boot menu,
 * the firmware loaded and executed it successfully — proving the
 * issue is in the CRT0 / _relocate path.
 *
 * Build: make minimal-test
 */

/* Minimal UEFI types - no headers needed */
typedef unsigned long long UINT64;
typedef unsigned long long UINTN;
typedef void *EFI_HANDLE;
typedef UINT64 EFI_STATUS;

typedef struct {
    char _hdr[24];                  /* EFI_TABLE_HEADER */
    void *_pad[4];                  /* GetTime .. SetWakeupTime */
    void *_pad2[5];                 /* SetVirtualAddressMap .. ConvertPointer */
    void *_pad3[3];                 /* GetVariable .. GetNextVariableName */
    void *SetVariable;
    void *_pad4;                    /* GetNextHighMonotonicCount */
    /* ResetSystem is the 14th function pointer (offset 0x68 on 64-bit) */
    void (*ResetSystem)(int ResetType, UINT64 Status, UINTN DataSize, void *Data);
} MINIMAL_RT;

typedef struct {
    char _hdr[24];                  /* EFI_TABLE_HEADER */
    void *_pad[6];                  /* FirmwareVendor .. ConsoleOutHandle */
    void *ConOut;                   /* EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL * */
    void *_pad2[3];                 /* StdErrHandle .. BootServices */
    MINIMAL_RT *RuntimeServices;    /* offset 0x58 on 64-bit */
} MINIMAL_ST;

/*
 * Direct entry point — no CRT0.
 * The firmware calls this with MS ABI: rcx=ImageHandle, rdx=SystemTable.
 */
__attribute__((ms_abi))
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, MINIMAL_ST *SystemTable)
{
    (void)ImageHandle;
    /* Immediate cold reboot.  If this executes, the machine reboots. */
    SystemTable->RuntimeServices->ResetSystem(0, 0, 0, (void *)0);
    /* Never reached */
    return 0;
}
