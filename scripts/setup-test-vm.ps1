#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Create a Hyper-V VM for testing the GhostCOM kernel driver.

.DESCRIPTION
    Creates a Gen2 Hyper-V VM from a Windows 11 ISO, with test signing
    enabled and the driver package shared via a mapped folder.

.PARAMETER IsoPath
    Path to the Windows 11 Enterprise Evaluation ISO.

.PARAMETER VMName
    Name for the VM. Default: "GhostCOM-Test"

.PARAMETER VHDXSizeGB
    Size of the virtual disk in GB. Default: 60
#>

param(
    [Parameter(Mandatory)]
    [string]$IsoPath,

    [string]$VMName = "GhostCOM-Test",
    [int]$VHDXSizeGB = 60
)

$ErrorActionPreference = "Stop"

Write-Host "GhostCOM Test VM Setup" -ForegroundColor Cyan
Write-Host "======================" -ForegroundColor Cyan
Write-Host ""

if (-not (Test-Path $IsoPath)) {
    Write-Error "ISO not found: $IsoPath"
    exit 1
}

# ── Paths ──────────────────────────────────────────────────────
$VMDir = "C:\HyperV\$VMName"
$VHDXPath = "$VMDir\$VMName.vhdx"

# ── Create VM directory ────────────────────────────────────────
New-Item -ItemType Directory -Path $VMDir -Force | Out-Null

# ── Create VHDX ───────────────────────────────────────────────
if (-not (Test-Path $VHDXPath)) {
    Write-Host "Creating ${VHDXSizeGB}GB VHDX at $VHDXPath..."
    New-VHD -Path $VHDXPath -SizeBytes ($VHDXSizeGB * 1GB) -Dynamic | Out-Null
}

# ── Create VM ─────────────────────────────────────────────────
$existingVM = Get-VM -Name $VMName -ErrorAction SilentlyContinue
if ($existingVM) {
    Write-Host "VM '$VMName' already exists. Removing..."
    Stop-VM -Name $VMName -Force -TurnOff -ErrorAction SilentlyContinue
    Remove-VM -Name $VMName -Force
}

Write-Host "Creating VM: $VMName"
New-VM -Name $VMName `
    -MemoryStartupBytes 4GB `
    -Generation 2 `
    -VHDPath $VHDXPath `
    -Path $VMDir `
    -SwitchName "Default Switch" | Out-Null

# ── Configure VM ──────────────────────────────────────────────
Write-Host "Configuring VM..."

# CPU and memory
Set-VM -Name $VMName `
    -ProcessorCount 4 `
    -DynamicMemory `
    -MemoryMinimumBytes 2GB `
    -MemoryMaximumBytes 8GB `
    -CheckpointType Standard

# Disable Secure Boot (required for test signing)
Set-VMFirmware -VMName $VMName -EnableSecureBoot Off

# Attach ISO
Write-Host "Attaching ISO: $IsoPath"
Add-VMDvdDrive -VMName $VMName -Path $IsoPath

# Set boot order: DVD first, then HD
$dvd = Get-VMDvdDrive -VMName $VMName
$hd = Get-VMHardDiskDrive -VMName $VMName
Set-VMFirmware -VMName $VMName -BootOrder $dvd, $hd

# Enable Guest Services for file copy
Enable-VMIntegrationService -VMName $VMName -Name "Guest Service Interface"

Write-Host ""
Write-Host "VM created successfully!" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "1. Start the VM:  Start-VM '$VMName'"
Write-Host "2. Connect:       vmconnect localhost '$VMName'"
Write-Host "3. Install Windows (skip product key, select Enterprise)"
Write-Host "4. After install, run in the VM (elevated):"
Write-Host "     bcdedit /set testsigning on"
Write-Host "     shutdown /r /t 0"
Write-Host "5. After reboot, copy driver package into VM and install"
Write-Host ""
Write-Host "To share the driver folder with the VM after Windows is installed:"
Write-Host "  Copy-VMFile '$VMName' -SourcePath 'C:\Users\camer\git\Personal Projects\node-null\driver\x64\Release' -DestinationPath 'C:\DriverPackage' -CreateFullPath -FileSource Host"
Write-Host ""
Write-Host "Starting VM now..."
Start-VM -Name $VMName
vmconnect localhost $VMName
