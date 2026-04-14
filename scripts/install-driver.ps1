#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Install the GhostCOM virtual COM port kernel driver.

.DESCRIPTION
    Copies the driver package to a staging directory and installs it
    using pnputil. For development, enables test signing if needed.

.PARAMETER TestSign
    Enable Windows test signing mode (requires reboot).

.PARAMETER DriverPath
    Path to the directory containing ghostcom.sys and ghostcom.inf.
    Defaults to ../driver/build/x64/Release.
#>

param(
    [switch]$TestSign,
    [string]$DriverPath = (Join-Path $PSScriptRoot "..\driver\build\x64\Release")
)

$ErrorActionPreference = "Stop"

Write-Host "GhostCOM driver installer" -ForegroundColor Cyan
Write-Host "=========================" -ForegroundColor Cyan
Write-Host ""

# ── Verify driver files exist ──────────────────────────────────

$sysFile = Join-Path $DriverPath "ghostcom.sys"
$infFile = Join-Path $DriverPath "ghostcom.inf"

if (-not (Test-Path $sysFile)) {
    Write-Error "Driver binary not found at: $sysFile`nBuild the driver first with: bun run build:driver"
    exit 1
}

if (-not (Test-Path $infFile)) {
    Write-Error "Driver INF not found at: $infFile"
    exit 1
}

# ── Enable test signing if requested ───────────────────────────

if ($TestSign) {
    Write-Host "Enabling test signing mode..." -ForegroundColor Yellow
    bcdedit /set testsigning on
    Write-Host "Test signing enabled. A reboot is required for this to take effect." -ForegroundColor Yellow
    Write-Host ""
}

# ── Install the driver ─────────────────────────────────────────

Write-Host "Installing driver from: $DriverPath" -ForegroundColor Green

# Use pnputil to add the driver to the driver store.
$result = pnputil /add-driver $infFile /install 2>&1

if ($LASTEXITCODE -ne 0) {
    Write-Host "pnputil failed. Trying devcon..." -ForegroundColor Yellow

    # Fall back to devcon if available.
    $devcon = Get-Command devcon.exe -ErrorAction SilentlyContinue
    if ($devcon) {
        & devcon.exe install $infFile "Root\GhostCOM"
        if ($LASTEXITCODE -ne 0) {
            Write-Error "devcon install failed. Check the output above."
            exit 1
        }
    } else {
        Write-Host $result
        Write-Error @"
Driver installation failed.

Make sure:
1. You are running as Administrator
2. Test signing is enabled (run with -TestSign flag, then reboot)
3. The driver is properly signed

To enable test signing:
    bcdedit /set testsigning on
    (reboot)

To install with devcon:
    devcon install ghostcom.inf Root\GhostCOM
"@
        exit 1
    }
} else {
    Write-Host $result
}

Write-Host ""
Write-Host "Driver installed successfully!" -ForegroundColor Green
Write-Host ""
Write-Host "Verify with:" -ForegroundColor Cyan
Write-Host "  pnputil /enum-drivers | Select-String -Context 3 'GhostCOM'"
Write-Host "  sc query GhostCOM"
