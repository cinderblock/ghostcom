#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Uninstall the GhostCOM virtual COM port kernel driver.

.DESCRIPTION
    Full teardown so `bun run install:driver` afterwards can do a clean
    fresh install via create-root-device.ps1:

      1. Remove the Root\GhostCOM device node (pnputil /remove-device).
         Leaving a stale device behind makes the next install take the
         "update existing device" path, which won't run if the INF
         version didn't change.

      2. Delete the driver package from the driver store
         (pnputil /delete-driver /uninstall), which also unbinds it
         from any lingering devices.

      3. Delete the service (sc delete), belt-and-suspenders - pnputil
         normally handles this via the INF's [Service Install Section]
         unbind, but `sc stop` on a PnP kernel driver always returns
         1052 so we can't rely on the service-control path.
#>

$ErrorActionPreference = "Stop"

Write-Host "GhostCOM driver uninstaller" -ForegroundColor Cyan
Write-Host "===========================" -ForegroundColor Cyan
Write-Host ""

# ── Remove the root device node ────────────────────────────────

Write-Host "Removing Root\GhostCOM device node(s)..." -ForegroundColor Yellow

$devices = Get-PnpDevice -ErrorAction SilentlyContinue |
    Where-Object {
        $_.InstanceId -like "ROOT\SYSTEM\*" -and
        $_.FriendlyName -match "GhostCOM Virtual COM Port Controller"
    }

if ($devices) {
    foreach ($d in $devices) {
        Write-Host "  $($d.InstanceId) - $($d.FriendlyName)" -ForegroundColor Gray
        pnputil /remove-device $d.InstanceId 2>&1 | Out-Null
    }
} else {
    Write-Host "  (none found)" -ForegroundColor Gray
}

# ── Remove from driver store ──────────────────────────────────

Write-Host "Removing driver from driver store..." -ForegroundColor Yellow

# Enumerate every ghostcom package in the store - we want them all gone
# so a same-version reinstall doesn't hit the "already exists / up-to-date"
# dedup path on the next `install:driver`.
$drivers = pnputil /enum-drivers 2>$null
$oemInfs = @()

$lines = $drivers -split "`n"
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match "ghostcom") {
        for ($j = $i; $j -ge [Math]::Max(0, $i - 10); $j--) {
            if ($lines[$j] -match "Published Name\s*:\s*(oem\d+\.inf)") {
                $oemInfs += $Matches[1]
                break
            }
        }
    }
}

$oemInfs = $oemInfs | Select-Object -Unique

if ($oemInfs.Count -gt 0) {
    foreach ($oemInf in $oemInfs) {
        Write-Host "  Removing $oemInf" -ForegroundColor Gray
        pnputil /delete-driver $oemInf /uninstall 2>&1 | Out-Null
    }
} else {
    Write-Host "  (no driver packages found)" -ForegroundColor Gray
}

# ── Delete the service ─────────────────────────────────────────

Write-Host "Deleting service..." -ForegroundColor Yellow
sc.exe delete GhostCOM 2>&1 | Out-Null

Write-Host ""
Write-Host "Driver uninstalled." -ForegroundColor Green
