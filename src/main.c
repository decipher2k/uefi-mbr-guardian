/*
Copyright 2026 Dennis Michael Heine

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied
See the License for the specific language governing permissions and
limitations under the License
*/

/*
 * MBR Guardian v2.0 - Graphical UEFI Boot Manager
 *
 * Features:
 *  - Graphical tile-based boot menu with mouse + keyboard
 *  - Automatic MBR change detection and snapshotting
 *  - Customizable icons per entry (BMP files on ESP)
 *  - CSM legacy chainloading of saved MBR states
 */

#include <efi.h>
#include <efilib.h>
#include <efidevp.h>
#include <protocol/legacyboot.h>
#include "gfx.h"
#include "font8x16.h"

/* ------------------------------------------------------------------ */
/*  MBR Constants and Structures                                       */
/* ------------------------------------------------------------------ */

#define MBR_SIZE            512
#ifndef MBR_SIGNATURE
#define MBR_SIGNATURE       0xAA55
#endif
#define SNAPSHOT_DIR        L"\\EFI\\mbr-guardian\\snapshots"
#define ICON_DIR            L"\\EFI\\mbr-guardian\\icons"
#define CONFIG_DIR          L"\\EFI\\mbr-guardian"
#define MAX_SNAPSHOTS       32
#define MAX_DISKS           16
#define MAX_UEFI_ENTRIES    32
#define LABEL_MAX           64
#define PATH_MAX            256

#define SNAP_MAGIC          0x4742524D  /* 'MBRG' */

typedef struct {
    UINT32  magic;
    UINT32  version;
    UINT32  disk_index;
    UINT32  mbr_hash;
    CHAR16  label[LABEL_MAX];
    CHAR16  icon_file[PATH_MAX];    /* Custom icon BMP path */
    EFI_TIME timestamp;
    UINT8   mbr[MBR_SIZE];
} MBR_SNAPSHOT;

typedef struct {
    UINTN           count;
    MBR_SNAPSHOT    entries[MAX_SNAPSHOTS];
    CHAR16          filenames[MAX_SNAPSHOTS][PATH_MAX];
    ICON_IMAGE      *icons[MAX_SNAPSHOTS];
} SNAPSHOT_LIST;

typedef struct {
    EFI_BLOCK_IO_PROTOCOL   *block_io;
    EFI_HANDLE              handle;
    UINT32                  disk_id;
} DISK_ENTRY;

typedef struct {
    UINT16      boot_num;               /* Boot#### number */
    CHAR16      description[LABEL_MAX]; /* Display name */
    CHAR16      path[PATH_MAX];         /* EFI file path (if available) */
    BOOLEAN     active;                 /* LOAD_OPTION_ACTIVE */
    ICON_IMAGE  *icon;
} UEFI_BOOT_ENTRY;

typedef struct {
    UINTN           count;
    UEFI_BOOT_ENTRY entries[MAX_UEFI_ENTRIES];
} UEFI_BOOT_LIST;

/* ------------------------------------------------------------------ */
/*  Globals                                                            */
/* ------------------------------------------------------------------ */

/* gST, gBS are provided by gnu-efi's efilib.h as macros (gST→ST, gBS→BS).
 * gRT is the runtime services macro. We alias gRS→gRT for this project.
 * LibImageHandle is gnu-efi's image handle; alias gImageHandle to it. */
#define gRS                 gRT
#define gImageHandle        LibImageHandle
extern EFI_HANDLE           LibImageHandle;

/* PI-spec EFI_LEGACY_BIOS_PROTOCOL (modern CSM firmware uses this GUID
   instead of the old Intel LEGACY_BOOT_PROTOCOL). */
static EFI_GUID gEfiLegacyBiosProtocolGuid =
    { 0xdb9a1e3d, 0x45cb, 0x4abb,
      { 0x85, 0x3b, 0xe5, 0x38, 0x7f, 0xdb, 0x2e, 0x2d } };

typedef EFI_STATUS (EFIAPI *PI_LEGACY_BIOS_BOOT)(
    IN void                 *This,
    IN BBS_BBS_DEVICE_PATH  *BootOption,
    IN UINT32                LoadOptionsSize,
    IN void                 *LoadOptions
);

typedef EFI_STATUS (EFIAPI *PI_LEGACY_BIOS_INT86)(
    IN void     *This,
    IN UINT8     BiosInt,
    IN OUT void *Regs
);

typedef EFI_STATUS (EFIAPI *PI_LEGACY_BIOS_GET_BBS_INFO)(
    IN  void     *This,
    OUT UINT16   *HddCount,
    OUT void    **HddInfo,
    OUT UINT16   *BbsCount,
    OUT void    **BbsTable
);

typedef EFI_STATUS (EFIAPI *PI_LEGACY_BIOS_PREPARE_TO_BOOT)(
    IN  void     *This,
    OUT UINT16   *BbsCount,
    OUT void    **BbsTable
);

typedef struct {
    PI_LEGACY_BIOS_INT86          Int86;                  /* 0 */
    void                         *FarCall86;              /* 1 */
    void                         *CheckPciRom;            /* 2 */
    void                         *InstallPciRom;          /* 3 */
    PI_LEGACY_BIOS_BOOT           LegacyBoot;             /* 4 */
    void                         *UpdateKeyboardLedStatus;/* 5 */
    PI_LEGACY_BIOS_GET_BBS_INFO   GetBbsInfo;             /* 6 */
    void                         *ShadowAllLegacyOproms;  /* 7 */
    PI_LEGACY_BIOS_PREPARE_TO_BOOT PrepareToBootEfi;      /* 8 */
} PI_LEGACY_BIOS_PROTOCOL;

/* PI BBS table entry (packed per PI spec Vol 5, Appendix A).
   Total size = 67 bytes (0x00..0x42). */
#pragma pack(1)
typedef struct {
    UINT16  BootPriority;       /* 0x00 */
    UINT32  Bus;                /* 0x02 */
    UINT32  Device;             /* 0x06 */
    UINT32  Function;           /* 0x0A */
    UINT8   Class;              /* 0x0E */
    UINT8   SubClass;           /* 0x0F */
    UINT16  MfgStringOffset;    /* 0x10 */
    UINT16  MfgStringSegment;   /* 0x12 */
    UINT16  DeviceType;         /* 0x14 */
    UINT16  StatusFlags;        /* 0x16 */
    UINT16  BootHandlerOffset;  /* 0x18 */
    UINT16  BootHandlerSegment; /* 0x1A */
    UINT16  DescStringOffset;   /* 0x1C */
    UINT16  DescStringSegment;  /* 0x1E */
    UINT32  InitPerReserved;    /* 0x20 */
    UINT16  AdditionalIrq13;    /* 0x24..0x2D */
    UINT8   _pad1[8];
    UINT16  AdditionalIrq18;    /* 0x2E..0x37 */
    UINT8   _pad2[8];
    UINT16  AdditionalIrq19;    /* 0x38..0x41 */
    UINT8   _pad3[8];
    UINT8   IBV1;               /* 0x42 */
} PI_BBS_ENTRY;                 /* 67 bytes */
#pragma pack()
#define PI_BBS_IGNORE   0xFFFF
#define PI_BBS_LOW_PRIO 0xFFFE

/* Minimal register set for PI Int86 (x86 registers, zeroed for INT 19h) */
typedef struct {
    UINT32  EAX, EBX, ECX, EDX, ESI, EDI;
    UINT16  EFlags;
    UINT16  ES, CS, SS, DS, FS, GS;
    UINT32  EBP, ESP;
} PI_IA32_REGISTER_SET;

static DISK_ENTRY           gDisks[MAX_DISKS];
static UINTN                gDiskCount = 0;

static SNAPSHOT_LIST        gSnaps;
static UEFI_BOOT_LIST       gUefiBoot;
static EFI_FILE_PROTOCOL    *gRootDir = NULL;

/* UI state */
static TILE                 gTiles[MAX_TILES];
static UINTN                gTileCount = 0;
static INT32                gSelectedTile = -1;
static INT32                gScrollY = 0;

/* Status message */
static CHAR16               gStatusMsg[128] = L"Ready";

/* Debug marker GUID: 5f0a6f09-7bf0-4b3d-9206-bdc7ca57f8c1 */
static EFI_GUID             gMbrgDebugGuid =
    { 0x5f0a6f09, 0x7bf0, 0x4b3d, { 0x92, 0x06, 0xbd, 0xc7, 0xca, 0x57, 0xf8, 0xc1 } };

#define MBRG_SKIP_VAR       L"MBRGSkipOnce"

static void
mark_boot_seen(void)
{
    UINT32 attrs;
    UINT32 count = 0;
    UINTN size = sizeof(count);

    EFI_STATUS s = gRS->GetVariable(L"MBRGSeen", &gMbrgDebugGuid, &attrs, &size, &count);
    if (EFI_ERROR(s) || size != sizeof(count)) count = 0;
    count++;

    gRS->SetVariable(
        L"MBRGSeen", &gMbrgDebugGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        sizeof(count), &count);
}

static void
set_skip_once(BOOLEAN enabled)
{
    UINT8 v = enabled ? 1 : 0;
    if (!enabled) {
        /* Delete variable */
        gRS->SetVariable(MBRG_SKIP_VAR, &gMbrgDebugGuid, 0, 0, NULL);
        return;
    }

    gRS->SetVariable(
        MBRG_SKIP_VAR, &gMbrgDebugGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        sizeof(v), &v);
}

static BOOLEAN
consume_skip_once(void)
{
    UINT8 v = 0;
    UINT32 attrs;
    UINTN size = sizeof(v);
    EFI_STATUS s = gRS->GetVariable(MBRG_SKIP_VAR, &gMbrgDebugGuid, &attrs, &size, &v);
    if (EFI_ERROR(s) || size != sizeof(v) || v == 0)
        return FALSE;

    /* One-shot: clear now and signal caller to passthrough. */
    set_skip_once(FALSE);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Toolbar button IDs                                                 */
/* ------------------------------------------------------------------ */

enum {
    BTN_BOOT = 1,
    BTN_SNAPSHOT,
    BTN_RENAME,
    BTN_ICON,
    BTN_DELETE,
    BTN_HEXDUMP,
    BTN_RESCAN,
    BTN_REBOOT,
    BTN_SHUTDOWN,
};

#define NUM_BUTTONS 9

static BUTTON gToolbar[NUM_BUTTONS];

/* Forward declarations for internal helpers used across sections. */
static EFI_STATUS read_mbr(UINTN idx, UINT8 *buf);
static BOOLEAN is_valid_mbr(const UINT8 *buf);

/* ------------------------------------------------------------------ */
/*  Utility                                                            */
/* ------------------------------------------------------------------ */

static UINT32
mbr_hash(const UINT8 *data, UINTN size)
{
    UINT32 h = 0x811c9dc5;
    for (UINTN i = 0; i < size; i++) {
        h ^= data[i];
        h *= 0x01000193;
    }
    return h;
}

static void
set_status(const CHAR16 *msg)
{
    UINTN i = 0;
    while (msg[i] && i < 126) { gStatusMsg[i] = msg[i]; i++; }
    gStatusMsg[i] = L'\0';
}

static void
wstrcpy(CHAR16 *dst, const CHAR16 *src, UINTN max)
{
    UINTN i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = L'\0';
}

static UINTN
wstrlen(const CHAR16 *s)
{
    UINTN n = 0;
    while (s[n]) n++;
    return n;
}

static CHAR16
to_lower_ascii(CHAR16 c)
{
    if (c >= L'A' && c <= L'Z') return (CHAR16)(c - L'A' + L'a');
    return c;
}

static BOOLEAN
wcontains_ci(const CHAR16 *hay, const CHAR16 *needle)
{
    if (!hay || !needle || !needle[0]) return FALSE;
    for (UINTN i = 0; hay[i]; i++) {
        UINTN j = 0;
        while (needle[j] && hay[i + j] &&
               to_lower_ascii(hay[i + j]) == to_lower_ascii(needle[j])) {
            j++;
        }
        if (!needle[j]) return TRUE;
    }
    return FALSE;
}

static EFI_STATUS
read_boot_option_desc(UINT16 boot_num, CHAR16 *desc_out, UINTN out_max,
                      BOOLEAN *active, BOOLEAN *is_bbs, BOOLEAN *has_efi_file)
{
    CHAR16 var_name[16];
    const CHAR16 *hex = L"0123456789ABCDEF";
    UINT8 *var_data = NULL;
    UINTN var_size = 0;
    EFI_STATUS status;

    var_name[0] = L'B'; var_name[1] = L'o';
    var_name[2] = L'o'; var_name[3] = L't';
    var_name[4] = hex[(boot_num >> 12) & 0xF];
    var_name[5] = hex[(boot_num >> 8) & 0xF];
    var_name[6] = hex[(boot_num >> 4) & 0xF];
    var_name[7] = hex[boot_num & 0xF];
    var_name[8] = L'\0';

    status = gRS->GetVariable(var_name, &gEfiGlobalVariableGuid, NULL, &var_size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL || var_size < 8)
        return EFI_NOT_FOUND;

    status = gBS->AllocatePool(EfiBootServicesData, var_size, (void **)&var_data);
    if (EFI_ERROR(status)) return status;

    status = gRS->GetVariable(var_name, &gEfiGlobalVariableGuid, NULL, &var_size, var_data);
    if (EFI_ERROR(status)) {
        gBS->FreePool(var_data);
        return status;
    }

    UINT32 attrs = *(UINT32 *)var_data;
    CHAR16 *desc = (CHAR16 *)(var_data + 6);

    if (active) *active = (attrs & 1) ? TRUE : FALSE;
    if (is_bbs) *is_bbs = FALSE;
    if (has_efi_file) *has_efi_file = FALSE;

    UINTN i = 0;
    while (desc[i] && i < out_max - 1) {
        desc_out[i] = desc[i];
        i++;
    }
    desc_out[i] = L'\0';

    /* Boot option layout: attrs(4), fp_len(2), desc(UTF-16 NUL), device path list */
    if (var_size >= 8) {
        UINT16 fp_len = *(UINT16 *)(var_data + 4);
        UINT8 *p = (UINT8 *)(var_data + 6);
        UINT8 *end = var_data + var_size;

        /* Advance over UTF-16 description including terminator. */
        while (p + sizeof(CHAR16) <= end) {
            if (*(CHAR16 *)p == 0) {
                p += sizeof(CHAR16);
                break;
            }
            p += sizeof(CHAR16);
        }

        if (p <= end && fp_len > 0 && (UINTN)(end - p) >= fp_len) {
            EFI_DEVICE_PATH_PROTOCOL *dp = (EFI_DEVICE_PATH_PROTOCOL *)p;
            UINTN rem = fp_len;

            while (rem >= sizeof(EFI_DEVICE_PATH_PROTOCOL)) {
                UINTN node_len = (UINTN)dp->Length[0] | ((UINTN)dp->Length[1] << 8);
                if (node_len < sizeof(EFI_DEVICE_PATH_PROTOCOL) || node_len > rem) break;

                if (dp->Type == BBS_DEVICE_PATH) {
                    if (is_bbs) *is_bbs = TRUE;
                }
                if (dp->Type == MEDIA_DEVICE_PATH && dp->SubType == MEDIA_FILEPATH_DP) {
                    if (has_efi_file) *has_efi_file = TRUE;
                }
                if (dp->Type == END_DEVICE_PATH_TYPE &&
                    dp->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE) {
                    break;
                }

                rem -= node_len;
                dp = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)dp + node_len);
            }
        }
    }

    gBS->FreePool(var_data);
    return EFI_SUCCESS;
}

static EFI_STATUS
find_legacy_bootnext(UINTN disk_idx, UINT16 *boot_num_out, CHAR16 *desc_out, UINTN desc_max)
{
    UINTN data_size = 0;
    UINT8 *data = NULL;
    EFI_STATUS status;
    INTN best_score = -10000;
    UINT16 best_num = 0;
    CHAR16 best_desc[LABEL_MAX] = L"";
    BOOLEAN prefer_usb = FALSE;
    UINT16 boot_current = 0xFFFF;
    UINTN boot_current_size = sizeof(boot_current);

    if (disk_idx < gDiskCount) {
        prefer_usb = gDisks[disk_idx].block_io->Media->RemovableMedia ? TRUE : FALSE;
    }

    gRS->GetVariable(L"BootCurrent", &gEfiGlobalVariableGuid, NULL,
                     &boot_current_size, &boot_current);

    status = gRS->GetVariable(L"BootOrder", &gEfiGlobalVariableGuid, NULL, &data_size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL || data_size == 0)
        return EFI_NOT_FOUND;

    status = gBS->AllocatePool(EfiBootServicesData, data_size, (void **)&data);
    if (EFI_ERROR(status)) return status;

    status = gRS->GetVariable(L"BootOrder", &gEfiGlobalVariableGuid, NULL, &data_size, data);
    if (EFI_ERROR(status)) {
        gBS->FreePool(data);
        return status;
    }

    UINT16 *order = (UINT16 *)data;
    UINTN count = data_size / sizeof(UINT16);

    for (UINTN i = 0; i < count; i++) {
        CHAR16 desc[LABEL_MAX];
        BOOLEAN active = FALSE;
        BOOLEAN is_bbs = FALSE;
        BOOLEAN has_efi_file = FALSE;
        INTN score = 0;

        if (order[i] == boot_current) continue;

        if (EFI_ERROR(read_boot_option_desc(order[i], desc, LABEL_MAX, &active,
                                            &is_bbs, &has_efi_file))) continue;
        if (!active) continue;

        if (wcontains_ci(desc, L"mbr guardian")) continue;

        /* For legacy fallback, only use explicit BBS/legacy-style options.
           If firmware does not expose such options, fail in-app instead of
           rebooting into arbitrary UEFI entries. */
        if (!is_bbs) continue;
        if (has_efi_file) continue;

        if (is_bbs) score += 220;

        if (wcontains_ci(desc, L"legacy") || wcontains_ci(desc, L"csm"))
            score += 90;

        if (wcontains_ci(desc, L"windows boot manager") ||
            wcontains_ci(desc, L"ubuntu") ||
            wcontains_ci(desc, L"grub") ||
            wcontains_ci(desc, L"uefi")) {
            score -= 200;
        }

        if (wcontains_ci(desc, L"lan") || wcontains_ci(desc, L"network") ||
            wcontains_ci(desc, L"pxe")) {
            score -= 80;
        }

        if (prefer_usb) {
            if (wcontains_ci(desc, L"usb")) score += 120;
            if (wcontains_ci(desc, L"hdd")) score += 30;
            if (wcontains_ci(desc, L"cd")) score += 20;
            if (wcontains_ci(desc, L"nvme") || wcontains_ci(desc, L"ata")) score -= 30;
        } else {
            if (wcontains_ci(desc, L"nvme")) score += 100;
            if (wcontains_ci(desc, L"ata")) score += 80;
            if (wcontains_ci(desc, L"hdd")) score += 50;
            if (wcontains_ci(desc, L"usb")) score -= 20;
        }

        if (score > best_score) {
            best_score = score;
            best_num = order[i];
            wstrcpy(best_desc, desc, LABEL_MAX);
        }
    }

    /* Second pass: if no BBS entries matched, try non-BBS entries whose
       description explicitly mentions "legacy" or "csm" and that have no
       EFI file path (i.e. not a UEFI-native loader). */
    if (best_score <= 0) {
        for (UINTN i = 0; i < count; i++) {
            CHAR16 desc[LABEL_MAX];
            BOOLEAN active = FALSE;
            BOOLEAN is_bbs = FALSE;
            BOOLEAN has_efi_file = FALSE;
            INTN score = 0;

            if (order[i] == boot_current) continue;
            if (EFI_ERROR(read_boot_option_desc(order[i], desc, LABEL_MAX,
                                                &active, &is_bbs, &has_efi_file)))
                continue;
            if (!active) continue;
            if (is_bbs) continue;            /* already tried */
            if (has_efi_file) continue;       /* skip UEFI loaders */
            if (wcontains_ci(desc, L"mbr guardian")) continue;

            if (wcontains_ci(desc, L"legacy") || wcontains_ci(desc, L"csm"))
                score += 100;
            else
                continue;   /* without explicit legacy/csm tag, skip */

            if (wcontains_ci(desc, L"windows boot manager") ||
                wcontains_ci(desc, L"ubuntu") ||
                wcontains_ci(desc, L"grub") ||
                wcontains_ci(desc, L"uefi"))
                score -= 200;

            if (score > best_score) {
                best_score = score;
                best_num = order[i];
                wstrcpy(best_desc, desc, LABEL_MAX);
            }
        }
    }

    gBS->FreePool(data);

    if (best_score <= 0) return EFI_NOT_FOUND;

    *boot_num_out = best_num;
    if (desc_out && desc_max > 0) wstrcpy(desc_out, best_desc, desc_max);
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Partition type info                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    UINT8   type;
    CHAR16  *name;
    COLOR   color;
} PTYPE_INFO;

static const PTYPE_INFO part_types[] = {
    { 0x07, L"NTFS/Windows",  RGB(0, 120, 215) },
    { 0x0B, L"FAT32",         RGB(0, 120, 215) },
    { 0x0C, L"FAT32 LBA",     RGB(0, 120, 215) },
    { 0x83, L"Linux",         RGB(230, 120, 0)  },
    { 0x82, L"Linux swap",    RGB(200, 100, 0)  },
    { 0x8E, L"Linux LVM",     RGB(200, 100, 0)  },
    { 0xA5, L"FreeBSD",       RGB(200, 20, 20)  },
    { 0xA6, L"OpenBSD",       RGB(200, 200, 0)  },
    { 0xA9, L"NetBSD",        RGB(240, 140, 0)  },
    { 0xAF, L"macOS HFS+",    RGB(160, 160, 160)},
    { 0xBF, L"Solaris",       RGB(100, 60, 200) },
    { 0xEB, L"BeOS/Haiku",    RGB(0, 180, 0)    },
    { 0xEE, L"GPT",           RGB(100, 100, 100)},
    { 0xEF, L"EFI System",    RGB(100, 100, 100)},
    { 0x00, NULL, 0 }
};

static const PTYPE_INFO *
get_part_info(UINT8 type)
{
    for (int i = 0; part_types[i].name != NULL; i++) {
        if (part_types[i].type == type) return &part_types[i];
    }
    return NULL;
}

static UINT8
get_primary_partition_type(const UINT8 *mbr)
{
    /* Find the first non-empty, active (or largest) partition */
    UINT8 best_type = 0;
    UINT32 best_size = 0;

    for (int i = 0; i < 4; i++) {
        const UINT8 *e = mbr + 446 + (i * 16);
        UINT8 type = e[4];
        UINT32 sectors = e[12] | (e[13]<<8) | (e[14]<<16) | (e[15]<<24);

        if (type == 0) continue;
        if (e[0] == 0x80) return type; /* Active partition takes priority */
        if (sectors > best_size) {
            best_size = sectors;
            best_type = type;
        }
    }
    return best_type;
}

static UINT32
get_mbr_disk_signature(const UINT8 *mbr)
{
    return (UINT32)mbr[440] |
           ((UINT32)mbr[441] << 8) |
           ((UINT32)mbr[442] << 16) |
           ((UINT32)mbr[443] << 24);
}

static UINTN
resolve_snapshot_disk(const MBR_SNAPSHOT *snap)
{
    UINT32 sig = get_mbr_disk_signature(snap->mbr);

    /* First try: matching MBR disk signature across currently visible disks. */
    if (sig != 0) {
        INT32 found = -1;
        UINT8 cur[MBR_SIZE];

        for (UINTN d = 0; d < gDiskCount; d++) {
            if (EFI_ERROR(read_mbr(d, cur))) continue;
            if (!is_valid_mbr(cur)) continue;
            if (get_mbr_disk_signature(cur) == sig) {
                if (found >= 0) {
                    /* Ambiguous signature: keep deterministic fallback. */
                    found = -2;
                    break;
                }
                found = (INT32)d;
            }
        }

        if (found >= 0)
            return (UINTN)found;
    }

    /* Fallback to stored index when still valid. */
    if (snap->disk_index < gDiskCount)
        return (UINTN)snap->disk_index;

    /* Last resort: first disk. */
    return 0;
}

static COLOR
get_os_color(UINT8 ptype)
{
    const PTYPE_INFO *info = get_part_info(ptype);
    if (info) return info->color;
    return COL_ACCENT;
}

/* ------------------------------------------------------------------ */
/*  Disk I/O                                                           */
/* ------------------------------------------------------------------ */

static EFI_STATUS
enumerate_disks(void)
{
    EFI_STATUS status;
    EFI_HANDLE *handles = NULL;
    UINTN count = 0;

    status = gBS->LocateHandleBuffer(ByProtocol, &gEfiBlockIoProtocolGuid,
                                     NULL, &count, &handles);
    if (EFI_ERROR(status)) return status;

    gDiskCount = 0;
    for (UINTN i = 0; i < count && gDiskCount < MAX_DISKS; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio;
        status = gBS->HandleProtocol(handles[i], &gEfiBlockIoProtocolGuid, (void **)&bio);
        if (EFI_ERROR(status)) continue;
        if (bio->Media->LogicalPartition) continue;
        if (bio->Media->BlockSize == 0) continue;
        if (!bio->Media->MediaPresent) continue;

        gDisks[gDiskCount].block_io = bio;
        gDisks[gDiskCount].handle   = handles[i];
        gDisks[gDiskCount].disk_id  = (UINT32)gDiskCount;
        gDiskCount++;
    }

    if (handles) gBS->FreePool(handles);
    return EFI_SUCCESS;
}

static EFI_STATUS read_mbr(UINTN idx, UINT8 *buf)
{
    if (idx >= gDiskCount) return EFI_INVALID_PARAMETER;
    EFI_BLOCK_IO_PROTOCOL *b = gDisks[idx].block_io;
    UINT32 blksz = b->Media->BlockSize;

    /* ReadBlocks requires BufferSize to be a multiple of BlockSize.
       For 4K-sector drives, read a full block and extract the MBR. */
    if (blksz <= MBR_SIZE) {
        return b->ReadBlocks(b, b->Media->MediaId, 0, MBR_SIZE, buf);
    }

    UINT8 *tmp;
    EFI_STATUS s = gBS->AllocatePool(EfiBootServicesData, blksz, (void **)&tmp);
    if (EFI_ERROR(s)) return s;

    s = b->ReadBlocks(b, b->Media->MediaId, 0, blksz, tmp);
    if (!EFI_ERROR(s)) {
        for (UINTN i = 0; i < MBR_SIZE; i++) buf[i] = tmp[i];
    }
    gBS->FreePool(tmp);
    return s;
}

static EFI_STATUS write_mbr(UINTN idx, const UINT8 *buf)
{
    if (idx >= gDiskCount) return EFI_INVALID_PARAMETER;
    EFI_BLOCK_IO_PROTOCOL *b = gDisks[idx].block_io;
    UINT32 blksz = b->Media->BlockSize;
    EFI_STATUS s;

    if (blksz <= MBR_SIZE) {
        s = b->WriteBlocks(b, b->Media->MediaId, 0, MBR_SIZE, (void *)buf);
        if (EFI_ERROR(s)) return s;
        /* Important for removable/USB media before immediate reboot/handoff. */
        return b->FlushBlocks(b);
    }

    /* Read-modify-write: preserve bytes beyond the MBR in the first block */
    UINT8 *tmp;
    s = gBS->AllocatePool(EfiBootServicesData, blksz, (void **)&tmp);
    if (EFI_ERROR(s)) return s;

    s = b->ReadBlocks(b, b->Media->MediaId, 0, blksz, tmp);
    if (!EFI_ERROR(s)) {
        for (UINTN i = 0; i < MBR_SIZE; i++) tmp[i] = buf[i];
        s = b->WriteBlocks(b, b->Media->MediaId, 0, blksz, tmp);
        if (!EFI_ERROR(s))
            s = b->FlushBlocks(b);
    }
    gBS->FreePool(tmp);
    return s;
}

static BOOLEAN is_valid_mbr(const UINT8 *buf)
{
    return (buf[510] | ((UINT16)buf[511] << 8)) == MBR_SIGNATURE;
}

/* ------------------------------------------------------------------ */
/*  ESP file I/O                                                       */
/* ------------------------------------------------------------------ */

static EFI_STATUS
open_root_dir(void)
{
    EFI_LOADED_IMAGE_PROTOCOL *li;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_STATUS s;

    s = gBS->HandleProtocol(gImageHandle, &gEfiLoadedImageProtocolGuid, (void **)&li);
    if (EFI_ERROR(s)) return s;
    s = gBS->HandleProtocol(li->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (void **)&fs);
    if (EFI_ERROR(s)) return s;
    return fs->OpenVolume(fs, &gRootDir);
}

static EFI_STATUS
ensure_directory(const CHAR16 *path)
{
    EFI_FILE_PROTOCOL *d;
    EFI_STATUS s = gRootDir->Open(gRootDir, &d, (CHAR16 *)path,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
        EFI_FILE_DIRECTORY);
    if (!EFI_ERROR(s)) d->Close(d);
    return s;
}

/* ------------------------------------------------------------------ */
/*  Snapshot file name builder                                         */
/* ------------------------------------------------------------------ */

static void
build_snap_path(CHAR16 *buf, UINTN disk, UINT32 hash)
{
    const CHAR16 *hex = L"0123456789ABCDEF";
    CHAR16 *p = buf;
    const CHAR16 *pfx = SNAPSHOT_DIR;
    while (*pfx) *p++ = *pfx++;
    *p++ = L'\\'; *p++ = L'd'; *p++ = L'i'; *p++ = L's'; *p++ = L'k';
    *p++ = L'0' + (CHAR16)(disk % 10);
    *p++ = L'_';
    for (int i = 7; i >= 0; i--) *p++ = hex[(hash >> (i*4)) & 0xF];
    *p++ = L'.'; *p++ = L'm'; *p++ = L'b'; *p++ = L'r';
    *p = L'\0';
}

/* ------------------------------------------------------------------ */
/*  Snapshot persistence                                               */
/* ------------------------------------------------------------------ */

static EFI_STATUS
save_snapshot(const MBR_SNAPSHOT *snap)
{
    CHAR16 path[PATH_MAX];
    build_snap_path(path, snap->disk_index, snap->mbr_hash);

    EFI_FILE_PROTOCOL *f;
    EFI_STATUS s = gRootDir->Open(gRootDir, &f, path,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(s)) return s;

    UINTN sz = sizeof(MBR_SNAPSHOT);
    s = f->Write(f, &sz, (void *)snap);
    f->Close(f);
    return s;
}

static EFI_STATUS
load_snapshots(void)
{
    EFI_FILE_PROTOCOL *dir;
    EFI_STATUS s;

    /* Free old icons */
    for (UINTN i = 0; i < gSnaps.count; i++) {
        if (gSnaps.icons[i]) gfx_free_icon(gSnaps.icons[i], gBS);
        gSnaps.icons[i] = NULL;
    }
    gSnaps.count = 0;

    s = gRootDir->Open(gRootDir, &dir, SNAPSHOT_DIR, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s)) return s;

    UINT8 buf[1024];
    for (;;) {
        UINTN bsz = sizeof(buf);
        s = dir->Read(dir, &bsz, buf);
        if (EFI_ERROR(s) || bsz == 0) break;

        EFI_FILE_INFO *info = (EFI_FILE_INFO *)buf;
        if (info->Attribute & EFI_FILE_DIRECTORY) continue;

        UINTN len = StrLen(info->FileName);
        if (len < 4) continue;
        CHAR16 *fn = info->FileName;
        if (fn[len-4]!=L'.'||fn[len-3]!=L'm'||fn[len-2]!=L'b'||fn[len-1]!=L'r')
            continue;

        /* Build full path */
        CHAR16 fpath[PATH_MAX];
        CHAR16 *p = fpath;
        const CHAR16 *pfx = SNAPSHOT_DIR;
        while (*pfx) *p++ = *pfx++;
        *p++ = L'\\';
        for (UINTN j = 0; j < len; j++) *p++ = fn[j];
        *p = L'\0';

        EFI_FILE_PROTOCOL *sf;
        s = gRootDir->Open(gRootDir, &sf, fpath, EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(s)) continue;

        UINTN ssz = sizeof(MBR_SNAPSHOT);
        UINTN idx = gSnaps.count;
        s = sf->Read(sf, &ssz, &gSnaps.entries[idx]);
        sf->Close(sf);
        if (EFI_ERROR(s) || gSnaps.entries[idx].magic != SNAP_MAGIC) continue;

        wstrcpy(gSnaps.filenames[idx], fpath, PATH_MAX);

        /* Load custom icon if specified */
        if (gSnaps.entries[idx].icon_file[0]) {
            gSnaps.icons[idx] = gfx_load_bmp(gRootDir, gSnaps.entries[idx].icon_file);
        }
        /* Otherwise create default icon based on partition type */
        if (!gSnaps.icons[idx]) {
            UINT8 ptype = get_primary_partition_type(gSnaps.entries[idx].mbr);
            COLOR acol = get_os_color(ptype);
            gSnaps.icons[idx] = gfx_create_default_icon(ptype, acol);
        }

        gSnaps.count++;
        if (gSnaps.count >= MAX_SNAPSHOTS) break;
    }

    dir->Close(dir);
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  UEFI Boot Option Enumeration                                       */
/* ------------------------------------------------------------------ */

/* gEfiGlobalVariableGuid is provided by gnu-efi's efilib.h */

static COLOR
uefi_label_color(const CHAR16 *desc)
{
    /* Try to guess OS from description for color coding */
    for (UINTN i = 0; desc[i]; i++) {
        CHAR16 c0 = desc[i];
        if (c0 == L'W' || c0 == L'w') {
            if (desc[i+1] == L'i' || desc[i+1] == L'I') return RGB(0, 120, 215);
        }
        if (c0 == L'U' || c0 == L'u') {
            if (desc[i+1] == L'b' || desc[i+1] == L'B') return RGB(230, 120, 0);
        }
        if (c0 == L'L' || c0 == L'l') {
            if (desc[i+1] == L'i' || desc[i+1] == L'I') return RGB(230, 120, 0);
        }
        if (c0 == L'F' || c0 == L'f') {
            if (desc[i+1] == L'e' || desc[i+1] == L'E') return RGB(200, 20, 20);
        }
        if (c0 == L'M' || c0 == L'm') {
            if (desc[i+1] == L'a' || desc[i+1] == L'A') return RGB(160, 160, 160);
        }
    }
    return COL_ACCENT;
}

static EFI_STATUS
load_uefi_boot_entries(void)
{
    EFI_STATUS status;
    UINTN data_size;
    UINT8 *data = NULL;

    /* Free old icons */
    for (UINTN i = 0; i < gUefiBoot.count; i++) {
        if (gUefiBoot.entries[i].icon) {
            gfx_free_icon(gUefiBoot.entries[i].icon, gBS);
            gUefiBoot.entries[i].icon = NULL;
        }
    }
    gUefiBoot.count = 0;

    /* Read BootOrder variable */
    data_size = 0;
    status = gRS->GetVariable(L"BootOrder", &gEfiGlobalVariableGuid,
                               NULL, &data_size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL || data_size == 0)
        return EFI_NOT_FOUND;

    status = gBS->AllocatePool(EfiBootServicesData, data_size, (void **)&data);
    if (EFI_ERROR(status)) return status;

    status = gRS->GetVariable(L"BootOrder", &gEfiGlobalVariableGuid,
                               NULL, &data_size, data);
    if (EFI_ERROR(status)) {
        gBS->FreePool(data);
        return status;
    }

    UINTN num_entries = data_size / 2;
    UINT16 *order = (UINT16 *)data;

    for (UINTN i = 0; i < num_entries && gUefiBoot.count < MAX_UEFI_ENTRIES; i++) {
        UINT16 boot_num = order[i];

        /* Build variable name "Boot####" */
        CHAR16 var_name[16];
        const CHAR16 *hex = L"0123456789ABCDEF";
        var_name[0] = L'B'; var_name[1] = L'o';
        var_name[2] = L'o'; var_name[3] = L't';
        var_name[4] = hex[(boot_num >> 12) & 0xF];
        var_name[5] = hex[(boot_num >> 8) & 0xF];
        var_name[6] = hex[(boot_num >> 4) & 0xF];
        var_name[7] = hex[boot_num & 0xF];
        var_name[8] = L'\0';

        /* Read Boot#### variable */
        UINTN var_size = 0;
        status = gRS->GetVariable(var_name, &gEfiGlobalVariableGuid,
                                   NULL, &var_size, NULL);
        if (status != EFI_BUFFER_TOO_SMALL || var_size < 8)
            continue;

        UINT8 *var_data;
        status = gBS->AllocatePool(EfiBootServicesData, var_size, (void **)&var_data);
        if (EFI_ERROR(status)) continue;

        status = gRS->GetVariable(var_name, &gEfiGlobalVariableGuid,
                                   NULL, &var_size, var_data);
        if (EFI_ERROR(status)) {
            gBS->FreePool(var_data);
            continue;
        }

        /* Parse EFI_LOAD_OPTION structure */
        UINT32 attributes = *(UINT32 *)var_data;
        /* UINT16 file_path_list_length = *(UINT16 *)(var_data + 4); */

        /* Extract description (CHAR16 string starting at offset 6) */
        CHAR16 *desc = (CHAR16 *)(var_data + 6);
        UINTN desc_len = 0;
        while (desc[desc_len] != L'\0' && (6 + (desc_len + 1) * 2) < var_size)
            desc_len++;

        /* Skip our own entry */
        BOOLEAN is_self = FALSE;
        if (desc_len >= 12) {
            const CHAR16 *mg = L"MBR Guardian";
            BOOLEAN match = TRUE;
            for (UINTN j = 0; mg[j]; j++) {
                if (desc[j] != mg[j]) { match = FALSE; break; }
            }
            if (match) is_self = TRUE;
        }
        if (is_self) {
            gBS->FreePool(var_data);
            continue;
        }

        UINTN idx = gUefiBoot.count;
        gUefiBoot.entries[idx].boot_num = boot_num;
        gUefiBoot.entries[idx].active = (attributes & 1) ? TRUE : FALSE;
        gUefiBoot.entries[idx].path[0] = L'\0';

        /* Copy description */
        UINTN copy_len = desc_len < LABEL_MAX - 1 ? desc_len : LABEL_MAX - 1;
        for (UINTN j = 0; j < copy_len; j++)
            gUefiBoot.entries[idx].description[j] = desc[j];
        gUefiBoot.entries[idx].description[copy_len] = L'\0';

        /* Create an icon based on detected OS */
        COLOR col = uefi_label_color(gUefiBoot.entries[idx].description);
        gUefiBoot.entries[idx].icon = gfx_create_default_icon(0xEF, col);

        gUefiBoot.count++;
        gBS->FreePool(var_data);
    }

    gBS->FreePool(data);
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  UEFI Boot (chainload via BootNext + reboot)                        */
/* ------------------------------------------------------------------ */

static void
do_uefi_boot(UINTN uefi_idx)
{
    if (uefi_idx >= gUefiBoot.count) return;
    UEFI_BOOT_ENTRY *entry = &gUefiBoot.entries[uefi_idx];

    /* Confirm */
    UINT32 dw = 460, dh = 200;
    INT32 dx = ((INT32)gGfx.screen_w - (INT32)dw) / 2;
    INT32 dy = ((INT32)gGfx.screen_h - (INT32)dh) / 2;

    BUTTON btn_boot = { .bounds = {dx+dw/2-130, dy+dh-50, 120, 36},
        .label = L"Boot Now", .enabled = TRUE, .color = COL_GREEN };
    BUTTON btn_cancel = { .bounds = {dx+dw/2+10, dy+dh-50, 120, 36},
        .label = L"Cancel", .enabled = TRUE, .color = COL_RED };

    for (;;) {
        mouse_poll();

        gfx_fill_rect_alpha(0, 0, gGfx.screen_w, gGfx.screen_h, COL_SHADOW, 160);
        ui_draw_shadow(dx, dy, dw, dh, 12);
        gfx_fill_rounded_rect(dx, dy, dw, dh, 10, COL_PANEL);
        gfx_fill_rounded_rect(dx, dy, dw, 40, 10, COL_ACCENT);
        gfx_fill_rect(dx, dy + 30, dw, 10, COL_ACCENT);
        gfx_text_centered(dx+dw/2, dy+10, L"Confirm UEFI Boot", COL_TEXT_BRIGHT, 1);

        gfx_text(dx+30, dy+60, L"Boot UEFI entry:", COL_TEXT, 1);
        gfx_text(dx+30, dy+82, entry->description, COL_ACCENT_BRIGHT, 1);
        gfx_text(dx+30, dy+110, L"System will reboot to this entry.", COL_TEXT_DIM, 1);

        btn_boot.hover = mouse_in_rect(&btn_boot.bounds);
        btn_cancel.hover = mouse_in_rect(&btn_cancel.bounds);
        ui_draw_button(&btn_boot);
        ui_draw_button(&btn_cancel);

        mouse_draw();
        gfx_flip();

        if (mouse_clicked_rect(&btn_boot.bounds)) {
            /* Set BootNext NVRAM variable to redirect next boot */
            UINT16 boot_num = entry->boot_num;
            EFI_STATUS s = gRS->SetVariable(
                L"BootNext", &gEfiGlobalVariableGuid,
                EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                EFI_VARIABLE_RUNTIME_ACCESS,
                sizeof(UINT16), &boot_num);

            if (EFI_ERROR(s)) {
                ui_dialog_info(L"Error", L"Failed to set BootNext variable!");
                return;
            }

            /* Show boot message */
            gfx_clear(COL_BG_DARK);
            gfx_text_centered(gGfx.screen_w/2, gGfx.screen_h/2 - 40,
                L"BootNext set. Rebooting...", COL_ACCENT_BRIGHT, 2);
            gfx_text_centered(gGfx.screen_w/2, gGfx.screen_h/2 + 20,
                entry->description, COL_TEXT_DIM, 1);
            gfx_flip();

            gBS->Stall(1000000); /* 1 second delay */
            gRS->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
            return;
        }
        if (mouse_clicked_rect(&btn_cancel.bounds)) return;

        EFI_INPUT_KEY key;
        if (!EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) {
            if (key.ScanCode == 0x17) return;
        }
    }
}

static EFI_STATUS
delete_snapshot(UINTN idx)
{
    if (idx >= gSnaps.count) return EFI_INVALID_PARAMETER;
    EFI_FILE_PROTOCOL *f;
    EFI_STATUS s = gRootDir->Open(gRootDir, &f, gSnaps.filenames[idx],
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(s)) return s;
    return f->Delete(f);
}

/* ------------------------------------------------------------------ */
/*  Auto-monitoring                                                    */
/* ------------------------------------------------------------------ */

static UINTN
monitor_disks(void)
{
    UINT8 cur[MBR_SIZE];
    UINTN new_count = 0;

    for (UINTN d = 0; d < gDiskCount; d++) {
        if (EFI_ERROR(read_mbr(d, cur))) continue;
        if (!is_valid_mbr(cur)) continue;

        UINT32 hash = mbr_hash(cur, MBR_SIZE);

        BOOLEAN found = FALSE;
        for (UINTN s = 0; s < gSnaps.count; s++) {
            if (gSnaps.entries[s].disk_index == (UINT32)d &&
                gSnaps.entries[s].mbr_hash == hash) {
                found = TRUE;
                break;
            }
        }

        if (!found && gSnaps.count < MAX_SNAPSHOTS) {
            MBR_SNAPSHOT snap;
            snap.magic = SNAP_MAGIC;
            snap.version = 2;
            snap.disk_index = (UINT32)d;
            snap.mbr_hash = hash;
            snap.icon_file[0] = L'\0';

            /* Auto-label based on detected OS */
            UINT8 ptype = get_primary_partition_type(cur);
            const PTYPE_INFO *pi = get_part_info(ptype);

            CHAR16 *lbl = snap.label;
            const CHAR16 *src = pi ? pi->name : L"Unknown OS";
            UINTN li = 0;
            while (src[li] && li < LABEL_MAX - 8) { lbl[li] = src[li]; li++; }
            lbl[li++] = L' '; lbl[li++] = L'D';
            lbl[li++] = L'0' + (CHAR16)(d % 10);
            lbl[li] = L'\0';

            gRS->GetTime(&snap.timestamp, NULL);
            for (UINTN i = 0; i < MBR_SIZE; i++) snap.mbr[i] = cur[i];

            if (!EFI_ERROR(save_snapshot(&snap))) {
                new_count++;
                load_snapshots();
            }
        }
    }
    return new_count;
}

/* ------------------------------------------------------------------ */
/*  Build tile layout                                                  */
/* ------------------------------------------------------------------ */

static void
build_tiles(void)
{
    gTileCount = gSnaps.count + gUefiBoot.count;
    if (gTileCount > MAX_TILES) gTileCount = MAX_TILES;

    /* Calculate grid layout */
    UINT32 content_w = gGfx.screen_w - 40; /* 20px padding each side */
    UINT32 cols = content_w / (TILE_W + TILE_PAD);
    if (cols == 0) cols = 1;

    UINTN tile_idx = 0;

    /* --- UEFI boot entries first --- */
    for (UINTN i = 0; i < gUefiBoot.count && tile_idx < MAX_TILES; i++, tile_idx++) {
        UINT32 col = (UINT32)(tile_idx % cols);
        UINT32 row = (UINT32)(tile_idx / cols);

        UINT32 grid_w = cols * (TILE_W + TILE_PAD) - TILE_PAD;
        INT32 start_x = ((INT32)gGfx.screen_w - (INT32)grid_w) / 2;
        INT32 start_y = TITLE_BAR_H + 20;

        gTiles[tile_idx].bounds.x = start_x + (INT32)(col * (TILE_W + TILE_PAD));
        gTiles[tile_idx].bounds.y = start_y + (INT32)(row * (TILE_H + TILE_PAD)) + gScrollY;
        gTiles[tile_idx].bounds.w = TILE_W;
        gTiles[tile_idx].bounds.h = TILE_H;

        gTiles[tile_idx].snap_index = (UINT32)i;
        gTiles[tile_idx].tile_type = TILE_TYPE_UEFI;
        gTiles[tile_idx].icon = gUefiBoot.entries[i].icon;
        gTiles[tile_idx].selected = ((INT32)tile_idx == gSelectedTile);

        wstrcpy(gTiles[tile_idx].label, gUefiBoot.entries[i].description, 64);

        /* Sublabel: "UEFI | Boot####" */
        CHAR16 *sl = gTiles[tile_idx].sublabel;
        const CHAR16 *hex = L"0123456789ABCDEF";
        UINT16 bn = gUefiBoot.entries[i].boot_num;
        sl[0] = L'U'; sl[1] = L'E'; sl[2] = L'F'; sl[3] = L'I';
        sl[4] = L' '; sl[5] = L'|'; sl[6] = L' ';
        sl[7] = L'B'; sl[8] = L'o'; sl[9] = L'o'; sl[10] = L't';
        sl[11] = hex[(bn >> 12) & 0xF];
        sl[12] = hex[(bn >> 8) & 0xF];
        sl[13] = hex[(bn >> 4) & 0xF];
        sl[14] = hex[bn & 0xF];
        sl[15] = L'\0';
    }

    /* --- Legacy MBR snapshot entries --- */
    for (UINTN i = 0; i < gSnaps.count && tile_idx < MAX_TILES; i++, tile_idx++) {
        UINT32 col = (UINT32)(tile_idx % cols);
        UINT32 row = (UINT32)(tile_idx / cols);

        UINT32 grid_w = cols * (TILE_W + TILE_PAD) - TILE_PAD;
        INT32 start_x = ((INT32)gGfx.screen_w - (INT32)grid_w) / 2;
        INT32 start_y = TITLE_BAR_H + 20;

        gTiles[tile_idx].bounds.x = start_x + (INT32)(col * (TILE_W + TILE_PAD));
        gTiles[tile_idx].bounds.y = start_y + (INT32)(row * (TILE_H + TILE_PAD)) + gScrollY;
        gTiles[tile_idx].bounds.w = TILE_W;
        gTiles[tile_idx].bounds.h = TILE_H;

        gTiles[tile_idx].snap_index = (UINT32)i;
        gTiles[tile_idx].tile_type = TILE_TYPE_LEGACY;
        gTiles[tile_idx].icon = gSnaps.icons[i];
        gTiles[tile_idx].selected = ((INT32)tile_idx == gSelectedTile);

        wstrcpy(gTiles[tile_idx].label, gSnaps.entries[i].label, 64);

        /* Build sublabel: Legacy | Disk X | YYYY-MM-DD */
        CHAR16 *sl = gTiles[tile_idx].sublabel;
        MBR_SNAPSHOT *snap = &gSnaps.entries[i];
        sl[0] = L'L'; sl[1] = L'e'; sl[2] = L'g'; sl[3] = L'a';
        sl[4] = L'c'; sl[5] = L'y'; sl[6] = L' '; sl[7] = L'|';
        sl[8] = L' '; sl[9] = L'D'; sl[10] = L'k';
        sl[11] = L'0' + (CHAR16)(snap->disk_index % 10);
        sl[12] = L' '; sl[13] = L'|'; sl[14] = L' ';

        /* Date */
        UINTN p = 15;
        UINT16 y = snap->timestamp.Year;
        sl[p++] = L'0' + (CHAR16)(y/1000); y %= 1000;
        sl[p++] = L'0' + (CHAR16)(y/100);  y %= 100;
        sl[p++] = L'0' + (CHAR16)(y/10);   y %= 10;
        sl[p++] = L'0' + (CHAR16)(y);
        sl[p++] = L'-';
        sl[p++] = L'0' + (CHAR16)(snap->timestamp.Month / 10);
        sl[p++] = L'0' + (CHAR16)(snap->timestamp.Month % 10);
        sl[p++] = L'-';
        sl[p++] = L'0' + (CHAR16)(snap->timestamp.Day / 10);
        sl[p++] = L'0' + (CHAR16)(snap->timestamp.Day % 10);
        sl[p] = L'\0';
    }
}

/* ------------------------------------------------------------------ */
/*  Initialize toolbar                                                 */
/* ------------------------------------------------------------------ */

static void
init_toolbar(void)
{
    struct { UINT32 id; const CHAR16 *label; UINT32 w; COLOR color; } defs[] = {
        { BTN_BOOT,      L"Boot",     80,  COL_GREEN },
        { BTN_SNAPSHOT,  L"Snapshot", 100, COL_ACCENT },
        { BTN_RENAME,    L"Rename",   90,  COL_ACCENT },
        { BTN_ICON,      L"Icon",     72,  COL_YELLOW },
        { BTN_DELETE,    L"Delete",   84,  COL_RED },
        { BTN_HEXDUMP,   L"HexDump", 96,  COL_ACCENT },
        { BTN_RESCAN,    L"Rescan",   88,  COL_ACCENT },
        { BTN_REBOOT,    L"Reboot",   88,  COL_ORANGE },
        { BTN_SHUTDOWN,  L"Shutdown", 100, COL_RED },
    };

    for (int i = 0; i < NUM_BUTTONS; i++) {
        gToolbar[i].id      = defs[i].id;
        gToolbar[i].enabled = TRUE;
        gToolbar[i].active  = FALSE;
        gToolbar[i].hover   = FALSE;
        gToolbar[i].color   = defs[i].color;
        gToolbar[i].bounds.w = defs[i].w;
        gToolbar[i].bounds.h = BTN_H;
        wstrcpy(gToolbar[i].label, defs[i].label, 32);
    }
}

/* ------------------------------------------------------------------ */
/*  Hex dump view (modal)                                              */
/* ------------------------------------------------------------------ */

static void
show_hexdump(UINTN snap_idx)
{
    if (snap_idx >= gSnaps.count) return;
    MBR_SNAPSHOT *snap = &gSnaps.entries[snap_idx];

    INT32 scroll = 0;
    INT32 max_scroll = (INT32)(MBR_SIZE / 16) * 18 - (INT32)(gGfx.screen_h - 100);
    if (max_scroll < 0) max_scroll = 0;

    BUTTON btn_close = { .bounds = {0, 0, 100, 36},
        .label = L"Close", .id = 0, .hover = FALSE,
        .active = FALSE, .enabled = TRUE, .color = COL_ACCENT };

    for (;;) {
        mouse_poll();

        gfx_clear(COL_BG_DARK);

        /* Header */
        gfx_fill_rect(0, 0, gGfx.screen_w, 48, COL_TITLE_BAR);
        gfx_text(20, 14, L"MBR Hex Dump: ", COL_TEXT_DIM, 1);
        gfx_text(20 + 14 * 8, 14, snap->label, COL_ACCENT_BRIGHT, 1);

        btn_close.bounds.x = (INT32)gGfx.screen_w - 120;
        btn_close.bounds.y = 6;
        btn_close.hover = mouse_in_rect(&btn_close.bounds);
        ui_draw_button(&btn_close);

        /* Hex content */
        INT32 base_y = 60 - scroll;
        const CHAR16 *hex = L"0123456789ABCDEF";

        for (UINTN row = 0; row < MBR_SIZE / 16; row++) {
            INT32 y = base_y + (INT32)(row * 18);
            if (y < 48 || y > (INT32)gGfx.screen_h) continue;

            UINTN off = row * 16;

            /* Offset */
            CHAR16 off_str[6];
            off_str[0] = hex[(off >> 12) & 0xF];
            off_str[1] = hex[(off >> 8) & 0xF];
            off_str[2] = hex[(off >> 4) & 0xF];
            off_str[3] = hex[off & 0xF];
            off_str[4] = L':';
            off_str[5] = L'\0';
            gfx_text(20, y, off_str, COL_ACCENT_DIM, 1);

            /* Hex bytes */
            for (UINTN col = 0; col < 16; col++) {
                UINT8 byte = snap->mbr[off + col];
                CHAR16 hx[4];
                hx[0] = hex[byte >> 4];
                hx[1] = hex[byte & 0xF];
                hx[2] = L' ';
                hx[3] = L'\0';

                COLOR bc = COL_TEXT;
                if (off + col >= 446 && off + col < 510) bc = COL_YELLOW; /* Partition table */
                if (off + col >= 510) bc = COL_GREEN; /* Signature */
                if (byte == 0) bc = COL_TEXT_DIM;

                gfx_text(80 + (INT32)(col * 24), y, hx, bc, 1);

                if (col == 7) /* Extra gap */
                    gfx_text(80 + 7 * 24 + 16, y, L" ", COL_TEXT_DIM, 1);
            }

            /* ASCII */
            INT32 ascii_x = 80 + 16 * 24 + 20;
            for (UINTN col = 0; col < 16; col++) {
                UINT8 byte = snap->mbr[off + col];
                CHAR16 ch[2] = { (byte >= 0x20 && byte < 0x7F) ? (CHAR16)byte : L'.', L'\0' };
                gfx_text(ascii_x + (INT32)(col * 8), y, ch, COL_TEXT_DIM, 1);
            }
        }

        /* Legend */
        INT32 ly = (INT32)gGfx.screen_h - 30;
        gfx_fill_rect(0, ly - 5, gGfx.screen_w, 35, COL_TOOLBAR);
        gfx_fill_rect(20, ly + 2, 12, 12, COL_YELLOW);
        gfx_text(36, ly, L"Partition Table", COL_TEXT_DIM, 1);
        gfx_fill_rect(180, ly + 2, 12, 12, COL_GREEN);
        gfx_text(196, ly, L"Boot Signature", COL_TEXT_DIM, 1);

        mouse_draw();
        gfx_flip();

        if (mouse_clicked_rect(&btn_close.bounds)) return;

        /* Keyboard */
        EFI_INPUT_KEY key;
        EFI_STATUS ks = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
        if (!EFI_ERROR(ks)) {
            if (key.ScanCode == 0x17) return; /* ESC */
            if (key.ScanCode == 0x01) { scroll -= 40; if (scroll < 0) scroll = 0; }  /* Up */
            if (key.ScanCode == 0x02) { scroll += 40; if (scroll > max_scroll) scroll = max_scroll; }  /* Down */
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Icon change dialog                                                 */
/* ------------------------------------------------------------------ */

static void
change_icon(UINTN snap_idx)
{
    if (snap_idx >= gSnaps.count) return;

    /* List .bmp files in icon directory */
    EFI_FILE_PROTOCOL *dir;
    EFI_STATUS s = gRootDir->Open(gRootDir, &dir, ICON_DIR, EFI_FILE_MODE_READ, 0);

    if (EFI_ERROR(s)) {
        ui_dialog_info(L"No Icons Found",
            L"Place .bmp files in \\EFI\\mbr-guardian\\icons\\");
        return;
    }

    CHAR16 bmp_files[16][PATH_MAX];
    UINTN bmp_count = 0;

    UINT8 buf[1024];
    for (;;) {
        UINTN bsz = sizeof(buf);
        s = dir->Read(dir, &bsz, buf);
        if (EFI_ERROR(s) || bsz == 0) break;

        EFI_FILE_INFO *info = (EFI_FILE_INFO *)buf;
        if (info->Attribute & EFI_FILE_DIRECTORY) continue;

        UINTN len = StrLen(info->FileName);
        if (len < 5) continue;
        CHAR16 *fn = info->FileName;
        if ((fn[len-4]==L'.'|| fn[len-4]==L'.') &&
            (fn[len-3]==L'b'||fn[len-3]==L'B') &&
            (fn[len-2]==L'm'||fn[len-2]==L'M') &&
            (fn[len-1]==L'p'||fn[len-1]==L'P') && bmp_count < 16) {

            /* Build full path */
            CHAR16 *p = bmp_files[bmp_count];
            const CHAR16 *pfx = ICON_DIR;
            while (*pfx) *p++ = *pfx++;
            *p++ = L'\\';
            for (UINTN j = 0; j < len; j++) *p++ = fn[j];
            *p = L'\0';
            bmp_count++;
        }
    }
    dir->Close(dir);

    if (bmp_count == 0) {
        ui_dialog_info(L"No Icons Found",
            L"Place .bmp files in \\EFI\\mbr-guardian\\icons\\");
        return;
    }

    /* Show selection dialog with icon previews */
    INT32 sel = 0;
    ICON_IMAGE *previews[16] = {0};
    for (UINTN i = 0; i < bmp_count; i++) {
        previews[i] = gfx_load_bmp(gRootDir, bmp_files[i]);
    }

    BUTTON btn_ok = { .bounds = {0,0,100,36}, .label = L"Select",
        .enabled = TRUE, .color = COL_GREEN };
    BUTTON btn_cancel = { .bounds = {0,0,100,36}, .label = L"Cancel",
        .enabled = TRUE, .color = COL_RED };
    BUTTON btn_reset = { .bounds = {0,0,120,36}, .label = L"Default",
        .enabled = TRUE, .color = COL_ACCENT };

    UINT32 dw = 520, dh = 400;
    INT32 dx = ((INT32)gGfx.screen_w - (INT32)dw) / 2;
    INT32 dy = ((INT32)gGfx.screen_h - (INT32)dh) / 2;

    for (;;) {
        mouse_poll();

        /* Dim background */
        gfx_fill_rect_alpha(0, 0, gGfx.screen_w, gGfx.screen_h, COL_SHADOW, 160);

        /* Dialog */
        ui_draw_shadow(dx, dy, dw, dh, 12);
        gfx_fill_rounded_rect(dx, dy, dw, dh, 10, COL_PANEL);
        gfx_fill_rounded_rect(dx, dy, dw, 40, 10, COL_ACCENT_DIM);
        gfx_fill_rect(dx, dy + 30, dw, 10, COL_ACCENT_DIM);
        gfx_text_centered(dx + dw/2, dy + 10, L"Choose Icon", COL_TEXT_BRIGHT, 1);

        /* Icon grid */
        INT32 gx = dx + 20;
        INT32 gy = dy + 55;
        UINT32 icon_sz = 64;
        UINT32 gap = 12;

        for (UINTN i = 0; i < bmp_count; i++) {
            UINT32 col = (UINT32)(i % 6);
            UINT32 row = (UINT32)(i / 6);
            INT32 ix = gx + (INT32)(col * (icon_sz + gap));
            INT32 iy = gy + (INT32)(row * (icon_sz + gap + 20));

            RECT ir = { ix, iy, icon_sz, icon_sz };

            /* Highlight */
            if ((INT32)i == sel) {
                gfx_fill_rounded_rect(ix - 4, iy - 4, icon_sz + 8, icon_sz + 8, 6, COL_ACCENT);
            }

            gfx_fill_rounded_rect(ix, iy, icon_sz, icon_sz, 4, COL_BG_DARK);
            if (previews[i]) {
                gfx_draw_icon_scaled(ix, iy, icon_sz, icon_sz, previews[i]);
            }

            /* Filename below icon */
            /* Extract just filename */
            const CHAR16 *fname = bmp_files[i];
            UINTN last_slash = 0;
            for (UINTN j = 0; fname[j]; j++)
                if (fname[j] == L'\\') last_slash = j + 1;
            gfx_text(ix, iy + icon_sz + 2, &fname[last_slash], COL_TEXT_DIM, 1);

            if (mouse_clicked_rect(&ir)) sel = (INT32)i;
        }

        /* Buttons */
        btn_ok.bounds.x = dx + dw/2 - 180;
        btn_ok.bounds.y = dy + dh - 50;
        btn_cancel.bounds.x = dx + dw/2 + 80;
        btn_cancel.bounds.y = dy + dh - 50;
        btn_reset.bounds.x = dx + dw/2 - 60;
        btn_reset.bounds.y = dy + dh - 50;

        btn_ok.hover = mouse_in_rect(&btn_ok.bounds);
        btn_cancel.hover = mouse_in_rect(&btn_cancel.bounds);
        btn_reset.hover = mouse_in_rect(&btn_reset.bounds);

        ui_draw_button(&btn_ok);
        ui_draw_button(&btn_reset);
        ui_draw_button(&btn_cancel);

        mouse_draw();
        gfx_flip();

        if (mouse_clicked_rect(&btn_ok.bounds) && sel >= 0 && sel < (INT32)bmp_count) {
            /* Apply icon */
            wstrcpy(gSnaps.entries[snap_idx].icon_file, bmp_files[sel], PATH_MAX);
            save_snapshot(&gSnaps.entries[snap_idx]);
            if (gSnaps.icons[snap_idx]) gfx_free_icon(gSnaps.icons[snap_idx], gBS);
            gSnaps.icons[snap_idx] = previews[sel];
            previews[sel] = NULL; /* Don't free this one */
            set_status(L"Icon updated.");
            break;
        }
        if (mouse_clicked_rect(&btn_reset.bounds)) {
            /* Reset to default icon */
            gSnaps.entries[snap_idx].icon_file[0] = L'\0';
            save_snapshot(&gSnaps.entries[snap_idx]);
            if (gSnaps.icons[snap_idx]) gfx_free_icon(gSnaps.icons[snap_idx], gBS);
            UINT8 ptype = get_primary_partition_type(gSnaps.entries[snap_idx].mbr);
            gSnaps.icons[snap_idx] = gfx_create_default_icon(ptype, get_os_color(ptype));
            set_status(L"Icon reset to default.");
            break;
        }
        if (mouse_clicked_rect(&btn_cancel.bounds)) break;

        EFI_INPUT_KEY key;
        if (!EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) {
            if (key.ScanCode == 0x17) break;
        }
    }

    /* Free previews */
    for (UINTN i = 0; i < bmp_count; i++) {
        if (previews[i]) gfx_free_icon(previews[i], gBS);
    }
}

/* ------------------------------------------------------------------ */
/*  Rename dialog (keyboard-based in GUI)                              */
/* ------------------------------------------------------------------ */

static void
rename_snapshot(UINTN snap_idx)
{
    if (snap_idx >= gSnaps.count) return;

    MBR_SNAPSHOT *snap = &gSnaps.entries[snap_idx];

    CHAR16 new_label[LABEL_MAX];
    wstrcpy(new_label, snap->label, LABEL_MAX);
    UINTN pos = wstrlen(new_label);

    UINT32 dw = 500, dh = 180;
    INT32 dx = ((INT32)gGfx.screen_w - (INT32)dw) / 2;
    INT32 dy = ((INT32)gGfx.screen_h - (INT32)dh) / 2;

    BUTTON btn_ok = { .bounds = {dx+dw/2-130, dy+dh-50, 120, 36},
        .label = L"OK", .enabled = TRUE, .color = COL_GREEN };
    BUTTON btn_cancel = { .bounds = {dx+dw/2+10, dy+dh-50, 120, 36},
        .label = L"Cancel", .enabled = TRUE, .color = COL_RED };

    for (;;) {
        mouse_poll();

        gfx_fill_rect_alpha(0, 0, gGfx.screen_w, gGfx.screen_h, COL_SHADOW, 160);
        ui_draw_shadow(dx, dy, dw, dh, 12);
        gfx_fill_rounded_rect(dx, dy, dw, dh, 10, COL_PANEL);
        gfx_fill_rounded_rect(dx, dy, dw, 40, 10, COL_ACCENT_DIM);
        gfx_fill_rect(dx, dy + 30, dw, 10, COL_ACCENT_DIM);
        gfx_text_centered(dx + dw/2, dy + 10, L"Rename Snapshot", COL_TEXT_BRIGHT, 1);

        /* Input field */
        INT32 fx = dx + 20, fy = dy + 65;
        UINT32 fw = dw - 40, fh = 32;
        gfx_fill_rounded_rect(fx, fy, fw, fh, 4, COL_BG_DARK);
        gfx_fill_rounded_rect(fx, fy, fw, fh, 4, COL_BG_DARK);
        gfx_text(fx + 8, fy + 8, new_label, COL_TEXT_BRIGHT, 1);

        /* Cursor */
        INT32 cur_x = fx + 8 + (INT32)(pos * FONT_W);
        gfx_fill_rect(cur_x, fy + 6, 2, 20, COL_ACCENT_BRIGHT);

        btn_ok.hover = mouse_in_rect(&btn_ok.bounds);
        btn_cancel.hover = mouse_in_rect(&btn_cancel.bounds);
        ui_draw_button(&btn_ok);
        ui_draw_button(&btn_cancel);

        mouse_draw();
        gfx_flip();

        if (mouse_clicked_rect(&btn_ok.bounds)) {
            /* Save new label */
            wstrcpy(snap->label, new_label, LABEL_MAX);
            delete_snapshot(snap_idx);
            save_snapshot(snap);
            load_snapshots();
            set_status(L"Snapshot renamed.");
            return;
        }
        if (mouse_clicked_rect(&btn_cancel.bounds)) return;

        EFI_INPUT_KEY key;
        if (!EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) {
            if (key.ScanCode == 0x17) return;
            if (key.UnicodeChar == L'\r') {
                wstrcpy(snap->label, new_label, LABEL_MAX);
                delete_snapshot(snap_idx);
                save_snapshot(snap);
                load_snapshots();
                set_status(L"Snapshot renamed.");
                return;
            }
            if (key.UnicodeChar == L'\b' && pos > 0) {
                new_label[--pos] = L'\0';
            } else if (key.UnicodeChar >= L' ' && pos < LABEL_MAX - 1) {
                new_label[pos++] = key.UnicodeChar;
                new_label[pos] = L'\0';
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Manual snapshot dialog                                             */
/* ------------------------------------------------------------------ */

static void
manual_snapshot(void)
{
    if (gDiskCount == 0) {
        ui_dialog_info(L"No Disks", L"No disks detected.");
        return;
    }

    /* Simple disk selection: show dialog with disk list */
    UINT32 dw = 400, dh = 200;
    INT32 dx = ((INT32)gGfx.screen_w - (INT32)dw) / 2;
    INT32 dy = ((INT32)gGfx.screen_h - (INT32)dh) / 2;
    INT32 sel_disk = 0;

    BUTTON btn_ok = { .bounds = {dx+dw/2-130, dy+dh-50, 120, 36},
        .label = L"Snapshot", .enabled = TRUE, .color = COL_GREEN };
    BUTTON btn_cancel = { .bounds = {dx+dw/2+10, dy+dh-50, 120, 36},
        .label = L"Cancel", .enabled = TRUE, .color = COL_RED };

    for (;;) {
        mouse_poll();

        gfx_fill_rect_alpha(0, 0, gGfx.screen_w, gGfx.screen_h, COL_SHADOW, 160);
        ui_draw_shadow(dx, dy, dw, dh, 12);
        gfx_fill_rounded_rect(dx, dy, dw, dh, 10, COL_PANEL);
        gfx_fill_rounded_rect(dx, dy, dw, 40, 10, COL_ACCENT_DIM);
        gfx_fill_rect(dx, dy + 30, dw, 10, COL_ACCENT_DIM);
        gfx_text_centered(dx + dw/2, dy + 10, L"Create Snapshot", COL_TEXT_BRIGHT, 1);

        /* Disk list */
        for (UINTN i = 0; i < gDiskCount; i++) {
            INT32 ey = dy + 55 + (INT32)(i * 26);
            RECT er = { dx + 20, ey, dw - 40, 24 };

            COLOR bg = (INT32)i == sel_disk ? COL_ACCENT_DIM : COL_BG_LIGHT;
            if (mouse_in_rect(&er)) bg = COL_PANEL_HOVER;
            gfx_fill_rounded_rect(er.x, er.y, er.w, er.h, 4, bg);

            CHAR16 dlbl[64];
            dlbl[0] = L'D'; dlbl[1] = L'i'; dlbl[2] = L's'; dlbl[3] = L'k';
            dlbl[4] = L' '; dlbl[5] = L'0' + (CHAR16)(i % 10);
            dlbl[6] = L'\0';
            gfx_text(er.x + 10, er.y + 4, dlbl, COL_TEXT, 1);

            if (mouse_clicked_rect(&er)) sel_disk = (INT32)i;
        }

        btn_ok.hover = mouse_in_rect(&btn_ok.bounds);
        btn_cancel.hover = mouse_in_rect(&btn_cancel.bounds);
        ui_draw_button(&btn_ok);
        ui_draw_button(&btn_cancel);

        mouse_draw();
        gfx_flip();

        if (mouse_clicked_rect(&btn_ok.bounds)) {
            UINT8 mbr_buf[MBR_SIZE];
            if (!EFI_ERROR(read_mbr(sel_disk, mbr_buf))) {
                MBR_SNAPSHOT snap;
                snap.magic = SNAP_MAGIC;
                snap.version = 2;
                snap.disk_index = (UINT32)sel_disk;
                snap.mbr_hash = mbr_hash(mbr_buf, MBR_SIZE);
                snap.icon_file[0] = L'\0';
                wstrcpy(snap.label, L"Manual Snapshot", LABEL_MAX);
                gRS->GetTime(&snap.timestamp, NULL);
                for (UINTN i = 0; i < MBR_SIZE; i++) snap.mbr[i] = mbr_buf[i];

                save_snapshot(&snap);
                load_snapshots();
                build_tiles();
                set_status(L"Manual snapshot created.");
            }
            return;
        }
        if (mouse_clicked_rect(&btn_cancel.bounds)) return;

        EFI_INPUT_KEY key;
        if (!EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) {
            if (key.ScanCode == 0x17) return;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Legacy boot                                                        */
/* ------------------------------------------------------------------ */

static void
do_legacy_boot(UINTN snap_idx)
{
    if (snap_idx >= gSnaps.count) return;
    MBR_SNAPSHOT *snap = &gSnaps.entries[snap_idx];

    UINTN target_disk = resolve_snapshot_disk(snap);

    if (gDiskCount == 0 || target_disk >= gDiskCount) {
        ui_dialog_info(L"Error", L"Target disk not found!");
        return;
    }

    /* Confirm */
    UINT32 dw = 460, dh = 200;
    INT32 dx = ((INT32)gGfx.screen_w - (INT32)dw) / 2;
    INT32 dy = ((INT32)gGfx.screen_h - (INT32)dh) / 2;

    BUTTON btn_boot = { .bounds = {dx+dw/2-130, dy+dh-50, 120, 36},
        .label = L"Boot Now", .enabled = TRUE, .color = COL_GREEN };
    BUTTON btn_cancel = { .bounds = {dx+dw/2+10, dy+dh-50, 120, 36},
        .label = L"Cancel", .enabled = TRUE, .color = COL_RED };

    for (;;) {
        mouse_poll();

        gfx_fill_rect_alpha(0, 0, gGfx.screen_w, gGfx.screen_h, COL_SHADOW, 160);
        ui_draw_shadow(dx, dy, dw, dh, 12);
        gfx_fill_rounded_rect(dx, dy, dw, dh, 10, COL_PANEL);
        gfx_fill_rounded_rect(dx, dy, dw, 40, 10, COL_GREEN);
        gfx_fill_rect(dx, dy + 30, dw, 10, COL_GREEN);
        gfx_text_centered(dx+dw/2, dy+10, L"Confirm Legacy Boot", COL_TEXT_BRIGHT, 1);

        gfx_text(dx+30, dy+60, L"Restore MBR and boot:", COL_TEXT, 1);
        gfx_text(dx+30, dy+82, snap->label, COL_ACCENT_BRIGHT, 1);
        gfx_text(dx+30, dy+104, L"MBR will be written to disk before boot.", COL_TEXT_DIM, 1);

        btn_boot.hover = mouse_in_rect(&btn_boot.bounds);
        btn_cancel.hover = mouse_in_rect(&btn_cancel.bounds);
        ui_draw_button(&btn_boot);
        ui_draw_button(&btn_cancel);

        mouse_draw();
        gfx_flip();

        if (mouse_clicked_rect(&btn_boot.bounds)) {
            /* Restore MBR */
            EFI_STATUS s = write_mbr(target_disk, snap->mbr);
            if (EFI_ERROR(s)) {
                ui_dialog_info(L"Error", L"Failed to write MBR to disk!");
                return;
            }

            /* Read back sector 0 to catch write/cache issues (common on USB). */
            UINT8 verify[MBR_SIZE];
            s = read_mbr(target_disk, verify);
            if (EFI_ERROR(s)) {
                ui_dialog_info(L"Error", L"MBR verify read failed after write.");
                set_status(L"Legacy boot aborted: cannot verify written MBR.");
                return;
            }
            if (mbr_hash(verify, MBR_SIZE) != mbr_hash(snap->mbr, MBR_SIZE)) {
                ui_dialog_info(L"Error", L"Written MBR differs from snapshot!");
                set_status(L"Legacy boot aborted: MBR write verification mismatch.");
                return;
            }

            /* Show boot message */
            gfx_clear(COL_BG_DARK);
            gfx_text_centered(gGfx.screen_w/2, gGfx.screen_h/2 - 40,
                L"MBR Restored. Initiating Legacy Boot...", COL_GREEN, 2);
            gfx_text_centered(gGfx.screen_w/2, gGfx.screen_h/2 + 20,
                L"Rebooting to legacy MBR on disk...", COL_TEXT_DIM, 1);
            gfx_flip();

            /* Try true legacy handoff via LegacyBoot protocol when available. */
            LEGACY_BOOT_INTERFACE *legacy = NULL;
            EFI_STATUS ls = gBS->LocateProtocol(&LegacyBootProtocol, NULL, (void **)&legacy);
            BOOLEAN have_legacy_protocol = (!EFI_ERROR(ls) && legacy && legacy->BootIt);

            /* If firmware falls back to this app on reboot, skip once and let
               firmware continue with next boot option instead of looping here. */
            set_skip_once(TRUE);

            if (have_legacy_protocol) {
                /* Prefer the selected disk's device path for handoff. */
                EFI_DEVICE_PATH *dp = NULL;
                ls = gBS->HandleProtocol(gDisks[target_disk].handle,
                                         &gEfiDevicePathProtocolGuid,
                                         (void **)&dp);
                if (!EFI_ERROR(ls) && dp) {
                    ls = legacy->BootIt(dp);
                    if (!EFI_ERROR(ls)) return;
                }

                /* Fallback: generic BBS device paths. USB is important for
                   external drives/sticks that expose legacy boot via CSM. */
                struct {
                    BBS_BBS_DEVICE_PATH bbs;
                    EFI_DEVICE_PATH_PROTOCOL end;
                } bbs_hd;

                bbs_hd.bbs.Header.Type = BBS_DEVICE_PATH;
                bbs_hd.bbs.Header.SubType = BBS_BBS_DP;
                bbs_hd.bbs.Header.Length[0] = sizeof(BBS_BBS_DEVICE_PATH);
                bbs_hd.bbs.Header.Length[1] = 0;
                bbs_hd.bbs.StatusFlag = 0;
                bbs_hd.bbs.String[0] = '\0';

                bbs_hd.end.Type = END_DEVICE_PATH_TYPE;
                bbs_hd.end.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
                bbs_hd.end.Length[0] = sizeof(EFI_DEVICE_PATH_PROTOCOL);
                bbs_hd.end.Length[1] = 0;

                UINT16 bbs_types[] = {
                    BBS_TYPE_HARDDRIVE,
                    BBS_TYPE_USB,
                    BBS_TYPE_CDROM,
                    BBS_TYPE_DEV,
                    BBS_TYPE_UNKNOWN
                };
                for (UINTN i = 0; i < sizeof(bbs_types)/sizeof(bbs_types[0]); i++) {
                    bbs_hd.bbs.DeviceType = bbs_types[i];
                    ls = legacy->BootIt((EFI_DEVICE_PATH *)&bbs_hd);
                    if (!EFI_ERROR(ls)) return;
                }
            }

            /* Fallback: PI-spec EFI_LEGACY_BIOS_PROTOCOL (AMI/Phoenix/Insyde
               CSM uses this GUID instead of the old Intel protocol). */
            PI_LEGACY_BIOS_PROTOCOL *pi_csm = NULL;
            BOOLEAN have_pi_csm = FALSE;
            if (!have_legacy_protocol) {
                EFI_STATUS pi_s = gBS->LocateProtocol(
                    &gEfiLegacyBiosProtocolGuid, NULL, (void **)&pi_csm);
                have_pi_csm = (!EFI_ERROR(pi_s) && pi_csm && pi_csm->LegacyBoot);
            }
            if (have_pi_csm) {
                /* Step 1: enumerate BBS table and prioritize target device. */
                UINT16 hdd_cnt = 0, bbs_cnt = 0;
                void *hdd_info = NULL;
                PI_BBS_ENTRY *bbs_tbl = NULL;
                BOOLEAN prefer_usb_bbs = FALSE;
                if (target_disk < gDiskCount)
                    prefer_usb_bbs = gDisks[target_disk].block_io->Media->RemovableMedia ? TRUE : FALSE;

                if (pi_csm->GetBbsInfo) {
                    pi_csm->GetBbsInfo(pi_csm, &hdd_cnt, &hdd_info,
                                       &bbs_cnt, (void **)&bbs_tbl);
                }

                /* Set BootPriority so the matching device type boots first. */
                UINT16 want_type = prefer_usb_bbs ? BBS_TYPE_USB : BBS_TYPE_HARDDRIVE;
                if (bbs_tbl && bbs_cnt > 0) {
                    UINT16 next_prio = 1;
                    for (UINT16 bi = 0; bi < bbs_cnt; bi++) {
                        if (bbs_tbl[bi].BootPriority == PI_BBS_IGNORE) continue;
                        if (bbs_tbl[bi].DeviceType == want_type)
                            bbs_tbl[bi].BootPriority = 0;
                        else
                            bbs_tbl[bi].BootPriority = next_prio++;
                    }
                }

                /* Step 2a: try handing off the target disk's own device path.
                   Some firmware (AMI) accepts the block-IO handle's native
                   device path for LegacyBoot instead of synthetic BBS nodes. */
                if (target_disk < gDiskCount) {
                    EFI_DEVICE_PATH *disk_dp = NULL;
                    EFI_STATUS dp_s = gBS->HandleProtocol(
                        gDisks[target_disk].handle,
                        &gEfiDevicePathProtocolGuid, (void **)&disk_dp);
                    if (!EFI_ERROR(dp_s) && disk_dp) {
                        ls = pi_csm->LegacyBoot(pi_csm,
                            (BBS_BBS_DEVICE_PATH *)disk_dp, 0, NULL);
                        if (!EFI_ERROR(ls)) return;
                    }
                }

                /* Step 2b: try BBS device path nodes for each plausible type.
                   LegacyBoot internally calls PrepareToBoot (the legacy
                   variant); do NOT call PrepareToBootEfi here — that one
                   tears down CSM for EFI boot and corrupts the legacy
                   environment, causing POST failures and black-screen hangs. */
                struct {
                    BBS_BBS_DEVICE_PATH      bbs;
                    EFI_DEVICE_PATH_PROTOCOL end;
                } pi_dp;

                pi_dp.bbs.Header.Type = BBS_DEVICE_PATH;
                pi_dp.bbs.Header.SubType = BBS_BBS_DP;
                pi_dp.bbs.Header.Length[0] = sizeof(BBS_BBS_DEVICE_PATH);
                pi_dp.bbs.Header.Length[1] = 0;
                pi_dp.bbs.StatusFlag = 0;
                pi_dp.bbs.String[0] = '\0';

                pi_dp.end.Type = END_DEVICE_PATH_TYPE;
                pi_dp.end.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
                pi_dp.end.Length[0] = sizeof(EFI_DEVICE_PATH_PROTOCOL);
                pi_dp.end.Length[1] = 0;

                UINT16 pi_types[] = {
                    want_type,
                    BBS_TYPE_HARDDRIVE,
                    BBS_TYPE_USB,
                    BBS_TYPE_CDROM,
                    BBS_TYPE_DEV,
                    BBS_TYPE_UNKNOWN
                };
                for (UINTN pi_i = 0;
                     pi_i < sizeof(pi_types)/sizeof(pi_types[0]); pi_i++) {
                    pi_dp.bbs.DeviceType = pi_types[pi_i];
                    ls = pi_csm->LegacyBoot(pi_csm, &pi_dp.bbs, 0, NULL);
                    if (!EFI_ERROR(ls)) return;
                }

                /* Step 2c: try with NULL device path — some CSM implementations
                   boot the highest-priority BBS entry when passed NULL. */
                ls = pi_csm->LegacyBoot(pi_csm, NULL, 0, NULL);
                if (!EFI_ERROR(ls)) return;
            }

            /* Firmware-specific fallback: use BootNext to jump to a likely
               legacy boot option (often more reliable than BootIt on laptops). */
            UINT16 legacy_bn = 0;
            CHAR16 legacy_desc[LABEL_MAX];
            EFI_STATUS bn_find = find_legacy_bootnext(target_disk, &legacy_bn, legacy_desc, LABEL_MAX);
            if (!EFI_ERROR(bn_find)) {
                EFI_STATUS bs = gRS->SetVariable(
                    L"BootNext", &gEfiGlobalVariableGuid,
                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                    EFI_VARIABLE_RUNTIME_ACCESS,
                    sizeof(UINT16), &legacy_bn);

                if (!EFI_ERROR(bs)) {
                    gfx_clear(COL_BG_DARK);
                    gfx_text_centered(gGfx.screen_w/2, gGfx.screen_h/2 - 30,
                        L"Legacy protocol failed. Using BootNext fallback...", COL_YELLOW, 1);
                    gfx_text_centered(gGfx.screen_w/2, gGfx.screen_h/2 + 4,
                        legacy_desc, COL_TEXT_DIM, 1);
                    gfx_flip();
                    gBS->Stall(1200000);
                    /* Try warm reset first — some firmware only processes
                       BootNext on warm reset.  Fall through to cold if
                       warm returns (should never happen). */
                    gRS->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
                    gRS->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
                    return;
                }
            }

            /* Last resort: create a temporary Boot#### variable with a BBS
               Hard Drive device path, set it as BootNext, and reboot.  This
               forces the firmware to attempt a legacy/CSM boot from the hard
               drive on next boot — even if there is no existing legacy entry
               in the firmware's BootOrder. */
            {
                /* Build a minimal EFI_LOAD_OPTION with BBS HDD device path.
                   Layout: Attributes(4) + FilePathListLength(2) +
                           Description(UTF-16 NUL) + DevicePath */
                CHAR16 *tmp_desc = L"MBR Guardian Legacy";
                UINTN desc_bytes = (21) * sizeof(CHAR16); /* includes NUL */

                struct {
                    BBS_BBS_DEVICE_PATH      bbs;
                    EFI_DEVICE_PATH_PROTOCOL end;
                } __attribute__((packed)) tmp_dp;

                tmp_dp.bbs.Header.Type     = BBS_DEVICE_PATH;
                tmp_dp.bbs.Header.SubType  = BBS_BBS_DP;
                tmp_dp.bbs.Header.Length[0] = (UINT8)sizeof(BBS_BBS_DEVICE_PATH);
                tmp_dp.bbs.Header.Length[1] = 0;
                tmp_dp.bbs.DeviceType      = BBS_TYPE_HARDDRIVE;
                tmp_dp.bbs.StatusFlag      = 0;
                tmp_dp.bbs.String[0]       = '\0';

                tmp_dp.end.Type     = END_DEVICE_PATH_TYPE;
                tmp_dp.end.SubType  = END_ENTIRE_DEVICE_PATH_SUBTYPE;
                tmp_dp.end.Length[0] = (UINT8)sizeof(EFI_DEVICE_PATH_PROTOCOL);
                tmp_dp.end.Length[1] = 0;

                UINT16 fp_len = (UINT16)sizeof(tmp_dp);
                UINTN var_len = 4 + 2 + desc_bytes + sizeof(tmp_dp);
                UINT8 *var_buf = NULL;
                EFI_STATUS alloc_s = gBS->AllocatePool(EfiBootServicesData, var_len, (void **)&var_buf);
                if (!EFI_ERROR(alloc_s) && var_buf) {
                    UINT8 *p = var_buf;
                    /* Attributes: LOAD_OPTION_ACTIVE */
                    UINT32 lo_attr = 1;
                    gBS->CopyMem(p, &lo_attr, 4);     p += 4;
                    gBS->CopyMem(p, &fp_len, 2);      p += 2;
                    gBS->CopyMem(p, tmp_desc, desc_bytes); p += desc_bytes;
                    gBS->CopyMem(p, &tmp_dp, sizeof(tmp_dp));

                    /* Use Boot9999 as a scratch entry unlikely to collide. */
                    UINT16 tmp_boot_num = 0x9999;
                    EFI_STATUS vs = gRS->SetVariable(
                        L"Boot9999", &gEfiGlobalVariableGuid,
                        EFI_VARIABLE_NON_VOLATILE |
                        EFI_VARIABLE_BOOTSERVICE_ACCESS |
                        EFI_VARIABLE_RUNTIME_ACCESS,
                        var_len, var_buf);

                    if (!EFI_ERROR(vs)) {
                        vs = gRS->SetVariable(
                            L"BootNext", &gEfiGlobalVariableGuid,
                            EFI_VARIABLE_NON_VOLATILE |
                            EFI_VARIABLE_BOOTSERVICE_ACCESS |
                            EFI_VARIABLE_RUNTIME_ACCESS,
                            sizeof(UINT16), &tmp_boot_num);
                    }

                    gBS->FreePool(var_buf);

                    if (!EFI_ERROR(vs)) {
                        gfx_clear(COL_BG_DARK);
                        gfx_text_centered(gGfx.screen_w/2, gGfx.screen_h/2 - 30,
                            L"MBR written. Rebooting via legacy Boot entry...",
                            COL_YELLOW, 1);
                        gfx_text_centered(gGfx.screen_w/2, gGfx.screen_h/2 + 4,
                            L"BootNext → Boot9999 (BBS HDD)", COL_TEXT_DIM, 1);
                        gfx_flip();
                        gBS->Stall(1500000);
                        gRS->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
                        gRS->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
                        return; /* should not reach */
                    }
                }
            }

            /* Should never reach here unless NVRAM write failed. */
            set_skip_once(FALSE);

            ui_dialog_info(L"Legacy Handoff Failed",
                L"No legacy handoff path succeeded. Check CSM + boot order.");
            if (!have_legacy_protocol && !have_pi_csm) {
                set_status(L"No CSM protocol found; could not create legacy boot entry.");
            } else {
                set_status(L"CSM present but all handoff methods failed.");
            }
            return;
        }
        if (mouse_clicked_rect(&btn_cancel.bounds)) return;

        EFI_INPUT_KEY key;
        if (!EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) {
            if (key.ScanCode == 0x17) return;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Handle toolbar actions                                             */
/* ------------------------------------------------------------------ */

static void
handle_button(UINT32 btn_id)
{
    /* Helper: get selected tile type and sub-index */
    UINT32 sel_type = 0;
    UINT32 sel_sub = 0;
    BOOLEAN has_sel = (gSelectedTile >= 0 && gSelectedTile < (INT32)gTileCount);
    if (has_sel) {
        sel_type = gTiles[gSelectedTile].tile_type;
        sel_sub  = gTiles[gSelectedTile].snap_index;
    }

    switch (btn_id) {
        case BTN_BOOT:
            if (!has_sel) {
                set_status(L"Select an entry to boot.");
                break;
            }
            if (sel_type == TILE_TYPE_UEFI) {
                do_uefi_boot(sel_sub);
            } else {
                do_legacy_boot(sel_sub);
            }
            break;

        case BTN_SNAPSHOT:
            manual_snapshot();
            break;

        case BTN_RENAME:
            if (has_sel && sel_type == TILE_TYPE_LEGACY) {
                rename_snapshot(sel_sub);
                build_tiles();
            } else if (has_sel && sel_type == TILE_TYPE_UEFI) {
                set_status(L"Cannot rename UEFI boot entries.");
            } else {
                set_status(L"Select a snapshot to rename.");
            }
            break;

        case BTN_ICON:
            if (has_sel && sel_type == TILE_TYPE_LEGACY) {
                change_icon(sel_sub);
                build_tiles();
            } else if (has_sel && sel_type == TILE_TYPE_UEFI) {
                set_status(L"Cannot change icon for UEFI entries.");
            } else {
                set_status(L"Select a snapshot to change its icon.");
            }
            break;

        case BTN_DELETE:
            if (has_sel && sel_type == TILE_TYPE_LEGACY) {
                delete_snapshot(sel_sub);
                load_snapshots();
                gSelectedTile = -1;
                build_tiles();
                set_status(L"Snapshot deleted.");
            } else if (has_sel && sel_type == TILE_TYPE_UEFI) {
                set_status(L"Cannot delete UEFI boot entries.");
            } else {
                set_status(L"Select a snapshot to delete.");
            }
            break;

        case BTN_HEXDUMP:
            if (has_sel && sel_type == TILE_TYPE_LEGACY) {
                show_hexdump(sel_sub);
            } else if (has_sel && sel_type == TILE_TYPE_UEFI) {
                set_status(L"Hex dump not available for UEFI entries.");
            } else {
                set_status(L"Select a snapshot for hex dump.");
            }
            break;

        case BTN_RESCAN:
            enumerate_disks();
            load_snapshots();
            load_uefi_boot_entries();
            {
                UINTN new_snaps = monitor_disks();
                if (new_snaps > 0) set_status(L"New MBR changes detected!");
                else set_status(L"No new changes found.");
            }
            build_tiles();
            break;

        case BTN_REBOOT:
            gRS->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
            break;

        case BTN_SHUTDOWN:
            gRS->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
            break;
    }
}

/* ------------------------------------------------------------------ */
/*  Main render loop                                                   */
/* ------------------------------------------------------------------ */

static BOOLEAN bg_cached = FALSE;

static void
render_frame(void)
{
    /* Restore cached background gradient instead of recomputing it */
    gfx_begin_frame();
    if (bg_cached) {
        gfx_restore_bg();
    } else {
        gfx_gradient_v(0, 0, gGfx.screen_w, gGfx.screen_h, COL_BG_DARK, COL_BG_MID);
        gfx_cache_bg();
        bg_cached = TRUE;
    }

    /* Title bar */
    ui_draw_titlebar(L"MBR Guardian", (UINT32)gTileCount, (UINT32)gDiskCount);

    /* Tiles */
    for (UINTN i = 0; i < gTileCount; i++) {
        /* Check visibility (clip to content area) */
        if (gTiles[i].bounds.y + (INT32)gTiles[i].bounds.h < TITLE_BAR_H) continue;
        if (gTiles[i].bounds.y > (INT32)(gGfx.screen_h - TOOLBAR_H - 30)) continue;

        /* Update hover */
        gTiles[i].hover = mouse_in_rect(&gTiles[i].bounds);
        gTiles[i].selected = ((INT32)i == gSelectedTile);

        ui_draw_tile(&gTiles[i]);
    }

    /* Empty state */
    if (gTileCount == 0) {
        gfx_text_centered(gGfx.screen_w / 2, gGfx.screen_h / 2 - 20,
            L"No Boot Entries Found", COL_TEXT_DIM, 2);
        gfx_text_centered(gGfx.screen_w / 2, gGfx.screen_h / 2 + 20,
            L"Press F5 to rescan, or create a manual snapshot.",
            COL_TEXT_DIM, 1);
    }

    /* Status bar */
    ui_draw_statusbar(gStatusMsg);

    /* Toolbar */
    ui_draw_toolbar(gToolbar, NUM_BUTTONS);

    /* Cursor is drawn separately after gfx_flip for efficient updates */
}

/* ------------------------------------------------------------------ */
/*  EFI Entry Point                                                    */
/* ------------------------------------------------------------------ */

EFI_STATUS
EFIAPI
efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *systab)
{
    EFI_STATUS status;

    gST = systab;
    gBS = systab->BootServices;
    gRS = systab->RuntimeServices;
    gImageHandle = image_handle;

    if (consume_skip_once()) {
        /* One-shot passthrough requested by previous legacy boot attempt. */
        return EFI_SUCCESS;
    }

#ifdef MBRG_ENTRY_RESET_TEST
    gRS->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
    for (;;) { }
#endif

    mark_boot_seen();

    InitializeLib(image_handle, systab);

    /* Initialize graphics */
    status = gfx_init(systab, gBS);
    if (EFI_ERROR(status)) {
        /* Fallback to text mode */
        systab->ConOut->Reset(systab->ConOut, TRUE);
        systab->ConOut->SetMode(systab->ConOut, 0);
        systab->ConOut->ClearScreen(systab->ConOut);
        systab->ConOut->OutputString(systab->ConOut,
            L"ERROR: Graphics initialization failed.\r\n"
            L"GOP not available. Falling back to text mode.\r\n");
        systab->ConOut->OutputString(systab->ConOut,
            L"Graphics init failed.\r\n");
        /* Could launch text-mode version here */
        systab->ConOut->OutputString(systab->ConOut,
            L"Press any key to continue...\r\n");
        UINTN idx;
        gBS->WaitForEvent(1, &systab->ConIn->WaitForKey, &idx);
        return EFI_UNSUPPORTED;
    }

    /* Initialize mouse */
    mouse_init(gBS);

    /* Show splash */
    gfx_clear(COL_BG_DARK);
    gfx_text_centered(gGfx.screen_w/2, gGfx.screen_h/2 - 40,
        L"MBR GUARDIAN", COL_ACCENT_BRIGHT, 3);
    gfx_text_centered(gGfx.screen_w/2, gGfx.screen_h/2 + 20,
        L"Initializing...", COL_TEXT_DIM, 1);
    gfx_flip();

    /* Open ESP */
    status = open_root_dir();
    if (EFI_ERROR(status)) {
        gfx_text_centered(gGfx.screen_w/2, gGfx.screen_h/2 + 50,
            L"FATAL: Cannot access ESP!", COL_RED, 1);
        gfx_flip();
        gBS->Stall(3000000);
        gfx_shutdown();
        return status;
    }

    ensure_directory(CONFIG_DIR);
    ensure_directory(SNAPSHOT_DIR);
    ensure_directory(ICON_DIR);

    /* Enumerate and scan */
    enumerate_disks();
    load_snapshots();
    load_uefi_boot_entries();

    UINTN new_snaps = monitor_disks();
    if (new_snaps > 0)
        set_status(L"New MBR changes detected and saved!");
    else
        set_status(L"Ready. Select an entry to boot.");

    /* Build UI */
    init_toolbar();
    build_tiles();

    /* Main event loop */
    BOOLEAN need_render = TRUE; /* first frame always renders */
    for (;;) {
        /* Poll mouse — save previous position to detect movement */
        INT32 prev_mx = gMouse.x, prev_my = gMouse.y;
        mouse_poll();
        BOOLEAN mouse_moved = (gMouse.x != prev_mx || gMouse.y != prev_my);
        if (gMouse.left_click || gMouse.right_click)
            need_render = TRUE;

        /* Check hover changes when mouse moved (avoid full redraw for cursor-only) */
        BOOLEAN hover_changed = FALSE;
        if (mouse_moved && !need_render) {
            for (UINTN i = 0; i < gTileCount; i++) {
                BOOLEAN h = mouse_in_rect(&gTiles[i].bounds);
                if (h != gTiles[i].hover) { hover_changed = TRUE; break; }
            }
            if (!hover_changed) {
                for (int b = 0; b < NUM_BUTTONS; b++) {
                    BOOLEAN h = mouse_in_rect(&gToolbar[b].bounds);
                    if (h != gToolbar[b].hover) { hover_changed = TRUE; break; }
                }
            }
            if (hover_changed)
                need_render = TRUE;
        }

        /* Handle tile clicks */
        for (UINTN i = 0; i < gTileCount; i++) {
            if (mouse_clicked_rect(&gTiles[i].bounds)) {
                if (gSelectedTile == (INT32)i) {
                    /* Double-click: boot */
                    if (gTiles[i].tile_type == TILE_TYPE_UEFI)
                        do_uefi_boot(gTiles[i].snap_index);
                    else
                        do_legacy_boot(gTiles[i].snap_index);
                } else {
                    gSelectedTile = (INT32)i;
                    set_status(gTiles[i].label);
                }
                need_render = TRUE;
            }
            if (mouse_rclicked_rect(&gTiles[i].bounds)) {
                /* Right-click: change icon (legacy only) */
                if (gTiles[i].tile_type == TILE_TYPE_LEGACY) {
                    gSelectedTile = (INT32)i;
                    change_icon(gTiles[i].snap_index);
                    build_tiles();
                }
                need_render = TRUE;
            }
        }

        /* Handle toolbar clicks */
        for (int b = 0; b < NUM_BUTTONS; b++) {
            if (mouse_clicked_rect(&gToolbar[b].bounds)) {
                handle_button(gToolbar[b].id);
                build_tiles(); /* Refresh */
                need_render = TRUE;
            }
        }

        /* Keyboard shortcuts */
        EFI_INPUT_KEY key;
        status = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
        if (!EFI_ERROR(status)) {
            need_render = TRUE;
            switch (key.UnicodeChar) {
                case L'b': case L'B': /* Boot */
                case L'\r':
                    if (gSelectedTile >= 0 && gSelectedTile < (INT32)gTileCount) {
                        handle_button(BTN_BOOT);
                    }
                    break;
                case L's': case L'S': /* Snapshot */
                    manual_snapshot();
                    build_tiles();
                    break;
                case L'r': case L'R': /* Rename */
                    handle_button(BTN_RENAME);
                    break;
                case L'i': case L'I': /* Icon */
                    handle_button(BTN_ICON);
                    break;
                case L'd': case L'D': /* Delete */
                    handle_button(BTN_DELETE);
                    break;
                case L'h': case L'H': /* Hex */
                    handle_button(BTN_HEXDUMP);
                    break;
                case L'q': case L'Q': /* Quit to UEFI */
                    gfx_shutdown();
                    return EFI_SUCCESS;
            }

            /* Arrow keys for tile navigation */
            if (key.ScanCode == 0x03) { /* Right */
                gSelectedTile++;
                if (gSelectedTile >= (INT32)gTileCount) gSelectedTile = 0;
            }
            if (key.ScanCode == 0x04) { /* Left */
                gSelectedTile--;
                if (gSelectedTile < 0) gSelectedTile = (INT32)gTileCount - 1;
            }
            if (key.ScanCode == 0x01) { /* Up - scroll */
                gScrollY += 30;
                if (gScrollY > 0) gScrollY = 0;
                build_tiles();
            }
            if (key.ScanCode == 0x02) { /* Down - scroll */
                gScrollY -= 30;
                build_tiles();
            }
            if (key.ScanCode == 0x17) { /* ESC - deselect */
                gSelectedTile = -1;
            }

            /* F5 = rescan */
            if (key.ScanCode == 0x0F) handle_button(BTN_RESCAN);
        }

        /* Render only when something changed */
        if (need_render) {
            mouse_restore_under();
            render_frame();
            gfx_flip();
            mouse_save_under();
            mouse_draw();
            gfx_flip_rect(gMouse.x, gMouse.y, CURSOR_SPRITE_W, CURSOR_SPRITE_H);
            need_render = FALSE;
        } else if (mouse_moved) {
            mouse_update_cursor();
        }

        /* Small delay to avoid spinning CPU while keeping cursor updates smooth. */
        gBS->Stall(2000);
    }

    gfx_shutdown();
    return EFI_SUCCESS;
}
