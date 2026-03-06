#Requires -RunAsAdministrator
<#
.SYNOPSIS
    MBR Guardian - Windows UEFI Boot Entry Installer
.DESCRIPTION
    Copies mbr-guardian.efi to the EFI System Partition and creates
    a UEFI boot entry so it appears in the firmware boot menu.
.NOTES
    Must be run as Administrator.
#>

param(
    [string]$EfiFile = "build\mbr-guardian.efi"
)

$ErrorActionPreference = "Stop"

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  MBR Guardian - UEFI Boot Installer"    -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# --- Verify EFI file exists ---
if (-not (Test-Path $EfiFile)) {
    Write-Host "ERROR: $EfiFile not found. Build the project first." -ForegroundColor Red
    exit 1
}

# --- Mount EFI System Partition ---
# Find a free drive letter
$usedLetters = (Get-Volume | Where-Object DriveLetter).DriveLetter
$freeLetter = [char[]](90..65) | Where-Object { $_ -notin $usedLetters } | Select-Object -First 1
$espDrive = "${freeLetter}:"

Write-Host "Mounting EFI System Partition to ${espDrive}..." -ForegroundColor Yellow

# Find the EFI System Partition
$espPartition = Get-Partition | Where-Object { $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' } | Select-Object -First 1

if (-not $espPartition) {
    Write-Host "ERROR: No EFI System Partition found." -ForegroundColor Red
    exit 1
}

$espDiskNumber = $espPartition.DiskNumber
$espPartNumber = $espPartition.PartitionNumber

# Check if ESP already has a drive letter
$existingLetter = $espPartition.DriveLetter
$mountedByUs = $false

if ($existingLetter) {
    $espDrive = "${existingLetter}:"
    Write-Host "  ESP already mounted at ${espDrive}" -ForegroundColor Green
} else {
    # Mount ESP
    $espPartition | Add-PartitionAccessPath -AccessPath "${espDrive}\"
    $mountedByUs = $true
    Write-Host "  Mounted at ${espDrive}" -ForegroundColor Green
}

try {
    # --- Create directories ---
    $targetDir = "${espDrive}\EFI\mbr-guardian"
    $snapshotDir = "${espDrive}\EFI\mbr-guardian\snapshots"
    $iconDir = "${espDrive}\EFI\mbr-guardian\icons"

    foreach ($dir in @($targetDir, $snapshotDir, $iconDir)) {
        if (-not (Test-Path $dir)) {
            New-Item -ItemType Directory -Path $dir -Force | Out-Null
        }
    }

    # --- Copy EFI file ---
    Write-Host "Copying mbr-guardian.efi..." -ForegroundColor Yellow
    Copy-Item $EfiFile "${targetDir}\mbr-guardian.efi" -Force
    Write-Host "  -> ${targetDir}\mbr-guardian.efi" -ForegroundColor Green

    # --- Copy icons ---
    if (Test-Path "icons\*.bmp") {
        Write-Host "Copying icons..." -ForegroundColor Yellow
        Copy-Item "icons\*.bmp" $iconDir -Force
        Write-Host "  -> $iconDir" -ForegroundColor Green
    }

    # --- Clean up old bcdedit entries (from previous broken installs) ---
    Write-Host ""
    Write-Host "Cleaning up old boot entries..." -ForegroundColor Yellow
    try {
        $fwEntries = bcdedit /enum firmware 2>&1 | Out-String
        $guidPattern = '\{[0-9a-fA-F\-]+\}'
        $currentId = $null
        foreach ($line in ($fwEntries -split "`n")) {
            if ($line -match "^identifier\s+($guidPattern)") {
                $currentId = $matches[1]
            }
            if ($line -match "description\s+MBR Guardian" -and $currentId) {
                Write-Host "  Removing old BCD entry $currentId..." -ForegroundColor Yellow
                bcdedit /set "{fwbootmgr}" displayorder $currentId /remove 2>&1 | Out-Null
                bcdedit /delete $currentId /f 2>&1 | Out-Null
                $currentId = $null
            }
        }
    } catch { <# ignore errors - old entries may not exist #> }

    # --- Create UEFI firmware boot entry via NVRAM ---
    # bcdedit /create /application osloader creates a Windows BCD entry which
    # does NOT appear in the UEFI firmware boot menu.  The firmware reads boot
    # entries from NVRAM variables (Boot####).  We write those directly using
    # the SetFirmwareEnvironmentVariable Win32 API.
    Write-Host ""
    Write-Host "Creating UEFI firmware boot entry (NVRAM)..." -ForegroundColor Yellow

    # --- P/Invoke helpers for UEFI NVRAM access ---
    if (-not ([System.Management.Automation.PSTypeName]'UefiBoot').Type) {
        Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public class UefiBoot
{
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern uint GetFirmwareEnvironmentVariable(
        string name, string guid, [Out] byte[] buf, uint size);

    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool SetFirmwareEnvironmentVariable(
        string name, string guid, byte[] val, uint size);

    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode,
               EntryPoint="SetFirmwareEnvironmentVariable")]
    public static extern bool DeleteFirmwareVariable(
        string name, string guid, IntPtr val, uint size);

    [DllImport("advapi32.dll", SetLastError=true)]
    static extern bool OpenProcessToken(IntPtr proc, uint access, out IntPtr token);

    [DllImport("advapi32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern bool LookupPrivilegeValue(string sys, string name, ref long luid);

    [DllImport("advapi32.dll", SetLastError=true)]
    static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll,
        ref TokPriv tp, int len, IntPtr prev, IntPtr ret);

    [DllImport("kernel32.dll")]
    static extern IntPtr GetCurrentProcess();

    [StructLayout(LayoutKind.Sequential, Pack=1)]
    struct TokPriv { public int Count; public long Luid; public int Attr; }

    public static void EnablePrivilege()
    {
        IntPtr tok;
        if (!OpenProcessToken(GetCurrentProcess(), 0x0028, out tok))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        var tp = new TokPriv { Count = 1, Attr = 2 };
        if (!LookupPrivilegeValue(null, "SeSystemEnvironmentPrivilege", ref tp.Luid))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        if (!AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error());
    }
}
"@
    }

    try {
        [UefiBoot]::EnablePrivilege()
    } catch {
        Write-Host "  WARNING: Could not acquire firmware variable privilege." -ForegroundColor Red
        Write-Host "  Ensure you are running as Administrator on a UEFI system." -ForegroundColor Gray
    }

    $efiGuid = "{8BE4DF61-93CA-11d2-AA0D-00E098032B8C}"

    # --- Read current BootOrder ---
    $orderBuf = New-Object byte[] 4096
    $orderSize = [UefiBoot]::GetFirmwareEnvironmentVariable("BootOrder", $efiGuid, $orderBuf, 4096)

    if ($orderSize -eq 0) {
        $lastErr = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        if ($lastErr -eq 1) {
            Write-Host "ERROR: This system does not appear to be booted in UEFI mode." -ForegroundColor Red
            Write-Host "  NVRAM variables are only available on UEFI systems." -ForegroundColor Gray
            exit 1
        }
    }

    $bootNums = [System.Collections.Generic.List[uint16]]::new()
    if ($orderSize -gt 0) {
        for ($i = 0; $i -lt $orderSize; $i += 2) {
            $bootNums.Add([BitConverter]::ToUInt16($orderBuf, $i))
        }
    }

    # --- Remove existing "MBR Guardian" NVRAM entries ---
    $keptNums = [System.Collections.Generic.List[uint16]]::new()
    foreach ($num in $bootNums) {
        $varName = "Boot{0:X4}" -f $num
        $buf = New-Object byte[] 4096
        $sz = [UefiBoot]::GetFirmwareEnvironmentVariable($varName, $efiGuid, $buf, 4096)

        $isMG = $false
        if ($sz -gt 6) {
            # EFI_LOAD_OPTION: 4 bytes attrs + 2 bytes pathlen, then CHAR16 description
            $desc = ""
            for ($j = 6; $j -lt ($sz - 1); $j += 2) {
                $ch = [BitConverter]::ToUInt16($buf, $j)
                if ($ch -eq 0) { break }
                $desc += [char]$ch
            }
            if ($desc -eq "MBR Guardian") {
                Write-Host "  Removing old NVRAM entry $varName..." -ForegroundColor Yellow
                [UefiBoot]::DeleteFirmwareVariable($varName, $efiGuid, [IntPtr]::Zero, 0) | Out-Null
                $isMG = $true
            }
        }
        if (-not $isMG) { $keptNums.Add($num) }
    }

    # --- Find a free Boot#### slot ---
    $newNum = -1
    for ($n = 1; $n -lt 0x10000; $n++) {
        $varName = "Boot{0:X4}" -f $n
        $probe = New-Object byte[] 4
        $psz = [UefiBoot]::GetFirmwareEnvironmentVariable($varName, $efiGuid, $probe, 4)
        if ($psz -eq 0) { $newNum = $n; break }
    }
    if ($newNum -lt 0) {
        Write-Host "ERROR: No free UEFI boot slot found." -ForegroundColor Red
        exit 1
    }

    # --- Gather ESP partition geometry for EFI device path ---
    $disk      = Get-Disk -Number $espDiskNumber
    $blockSize = [uint64]$disk.LogicalSectorSize
    $partStart = [uint64]($espPartition.Offset / $blockSize)
    $partSize  = [uint64]($espPartition.Size   / $blockSize)
    $partGuid  = [Guid]::new(($espPartition.Guid -replace '[{}]',''))

    # --- Build EFI_LOAD_OPTION binary (UEFI Spec 2.10 §3.1.3) ---
    $ms = [System.IO.MemoryStream]::new()

    # UINT32 Attributes = LOAD_OPTION_ACTIVE
    $ms.Write([BitConverter]::GetBytes([uint32]1), 0, 4)

    # UINT16 FilePathListLength (placeholder - patched below)
    $fpLenOff = $ms.Position
    $ms.Write([byte[]]@(0,0), 0, 2)

    # CHAR16[] Description = "MBR Guardian\0"
    $descBytes = [System.Text.Encoding]::Unicode.GetBytes("MBR Guardian")
    $ms.Write($descBytes, 0, $descBytes.Length)
    $ms.Write([byte[]]@(0,0), 0, 2)

    # --- EFI_DEVICE_PATH nodes ---
    $fpStart = $ms.Position

    # HD() node  (Type=0x04 Media, SubType=0x01 HardDrive, Length=42)
    $ms.WriteByte(0x04); $ms.WriteByte(0x01)
    $ms.Write([BitConverter]::GetBytes([uint16]42), 0, 2)
    $ms.Write([BitConverter]::GetBytes([uint32]$espPartNumber), 0, 4)
    $ms.Write([BitConverter]::GetBytes([uint64]$partStart), 0, 8)
    $ms.Write([BitConverter]::GetBytes([uint64]$partSize),  0, 8)
    $ms.Write($partGuid.ToByteArray(), 0, 16)
    $ms.WriteByte(0x02)   # MBR type  = GPT
    $ms.WriteByte(0x02)   # Sig type  = GUID

    # File() node (Type=0x04 Media, SubType=0x04 FilePath)
    $efiPath   = "\EFI\mbr-guardian\mbr-guardian.efi"
    $pathBytes = [System.Text.Encoding]::Unicode.GetBytes($efiPath)
    $fileLen   = [uint16](4 + $pathBytes.Length + 2)     # header + path + null
    $ms.WriteByte(0x04); $ms.WriteByte(0x04)
    $ms.Write([BitConverter]::GetBytes($fileLen), 0, 2)
    $ms.Write($pathBytes, 0, $pathBytes.Length)
    $ms.Write([byte[]]@(0,0), 0, 2)                     # CHAR16 null terminator

    # End node   (Type=0x7F, SubType=0xFF, Length=4)
    $ms.WriteByte(0x7F); $ms.WriteByte(0xFF)
    $ms.Write([BitConverter]::GetBytes([uint16]4), 0, 2)

    $fpEnd = $ms.Position

    # Patch FilePathListLength
    $fpLen = [uint16]($fpEnd - $fpStart)
    $ms.Position = $fpLenOff
    $ms.Write([BitConverter]::GetBytes($fpLen), 0, 2)

    $loadOption = $ms.ToArray()
    $ms.Dispose()

    # --- Write Boot#### NVRAM variable ---
    $newVarName = "Boot{0:X4}" -f $newNum
    $ok = [UefiBoot]::SetFirmwareEnvironmentVariable(
        $newVarName, $efiGuid, $loadOption, [uint32]$loadOption.Length)

    if (-not $ok) {
        $lastErr = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        Write-Host "ERROR: Failed to write NVRAM ($newVarName), Win32 error $lastErr." -ForegroundColor Red
        Write-Host ""
        Write-Host "Fallback: add the entry manually in UEFI/BIOS Setup:" -ForegroundColor Yellow
        Write-Host "  File: \EFI\mbr-guardian\mbr-guardian.efi" -ForegroundColor Gray
    } else {
        # --- Update BootOrder (append new entry) ---
        $orderMs = [System.IO.MemoryStream]::new()
        foreach ($n in $keptNums) {
            $orderMs.Write([BitConverter]::GetBytes([uint16]$n), 0, 2)
        }
        $orderMs.Write([BitConverter]::GetBytes([uint16]$newNum), 0, 2)
        $newOrder = $orderMs.ToArray()
        $orderMs.Dispose()

        $ok2 = [UefiBoot]::SetFirmwareEnvironmentVariable(
            "BootOrder", $efiGuid, $newOrder, [uint32]$newOrder.Length)

        if ($ok2) {
            Write-Host "  NVRAM entry '$newVarName' created successfully." -ForegroundColor Green
            Write-Host "  'MBR Guardian' added to firmware boot order." -ForegroundColor Green
        } else {
            Write-Host "  WARNING: Entry written but BootOrder update failed." -ForegroundColor Yellow
            Write-Host "  You may need to enable the entry in UEFI Setup." -ForegroundColor Gray
        }
    }

    Write-Host ""
    Write-Host "Note:" -ForegroundColor Yellow
    Write-Host "  'MBR Guardian' should now appear in the UEFI firmware boot menu." -ForegroundColor Gray
    Write-Host "  Access it via the boot device menu (F12/F8/ESC at POST)," -ForegroundColor Gray
    Write-Host "  or set it as first boot option in UEFI Setup." -ForegroundColor Gray

} finally {
    # --- Unmount ESP if we mounted it ---
    if ($mountedByUs) {
        Write-Host ""
        Write-Host "Unmounting ESP from ${espDrive}..." -ForegroundColor Yellow
        Remove-PartitionAccessPath -DiskNumber $espDiskNumber -PartitionNumber $espPartNumber -AccessPath "${espDrive}\"
        Write-Host "  Done." -ForegroundColor Green
    }
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Installation Complete!" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "To boot MBR Guardian:" -ForegroundColor White
Write-Host "  - Reboot and open the boot device menu (F12 / F8 / ESC)" -ForegroundColor Gray
Write-Host "  - Select 'MBR Guardian' from the list" -ForegroundColor Gray
Write-Host ""
Write-Host "If the entry is missing, open UEFI Setup and add a boot" -ForegroundColor Gray
Write-Host "option for:  \EFI\mbr-guardian\mbr-guardian.efi" -ForegroundColor Gray
Write-Host ""
