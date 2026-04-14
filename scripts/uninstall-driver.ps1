#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Uninstall the GhostCOM virtual COM port kernel driver.

.DESCRIPTION
    Stops the driver service and removes the driver from the system.
#>

$ErrorActionPreference = "Stop"

Write-Host "GhostCOM driver uninstaller" -ForegroundColor Cyan
Write-Host "===========================" -ForegroundColor Cyan
Write-Host ""

# ── Stop the service ───────────────────────────────────────────

Write-Host "Stopping GhostCOM service..." -ForegroundColor Yellow
sc.exe stop GhostCOM 2>$null

# ── Remove the device ──────────────────────────────────────────

$devcon = Get-Command devcon.exe -ErrorAction SilentlyContinue
if ($devcon) {
    Write-Host "Removing device..." -ForegroundColor Yellow
    & devcon.exe remove "Root\GhostCOM" 2>$null
}

# ── Remove from driver store ──────────────────────────────────

Write-Host "Removing driver from driver store..." -ForegroundColor Yellow

# Find the OEM INF for our driver.
$drivers = pnputil /enum-drivers 2>$null
$oemInf = $null

$lines = $drivers -split "`n"
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match "ghostcom") {
        # Look backwards for the "Published Name" line.
        for ($j = $i; $j -ge [Math]::Max(0, $i - 10); $j--) {
            if ($lines[$j] -match "Published Name\s*:\s*(oem\d+\.inf)") {
                $oemInf = $Matches[1]
                break
            }
        }
        break
    }
}

if ($oemInf) {
    Write-Host "Found driver package: $oemInf" -ForegroundColor Green
    pnputil /delete-driver $oemInf /uninstall /force
} else {
    Write-Host "Driver package not found in driver store (may already be removed)." -ForegroundColor Yellow
}

# ── Delete the service ─────────────────────────────────────────

Write-Host "Deleting service..." -ForegroundColor Yellow
sc.exe delete GhostCOM 2>$null

Write-Host ""
Write-Host "Driver uninstalled." -ForegroundColor Green
