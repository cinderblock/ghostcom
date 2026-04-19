#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Install the GhostCOM virtual COM port kernel driver.

.DESCRIPTION
    Two-phase install:

    1. If the Root\GhostCOM device instance doesn't exist yet (fresh
       machine, or `bun run uninstall:driver` removed it), create it
       via scripts/create-root-device.ps1 - a devcon-free SetupAPI
       bootstrap. This also copies the INF into the driver store and
       binds it to the new device.

    2. If the device already exists, run pnputil /add-driver /install
       which both updates the driver store and reinstalls the package
       on the existing device.

    pnputil exits non-zero in several no-op-but-not-failure cases
    ("Added driver packages: 0" when the device is already up-to-date
    against the staged package), so we parse stdout for concrete
    success markers rather than trusting the exit code.

.PARAMETER TestSign
    Enable Windows test signing mode (requires reboot). Normally only
    needed on a fresh VM; the build script self-signs with
    GhostCOMTestCert which only works in test-signing mode.

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

# ── Detect existing device instance ────────────────────────────

$existing = Get-PnpDevice -ErrorAction SilentlyContinue |
    Where-Object {
        $_.InstanceId -like "ROOT\SYSTEM\*" -and
        $_.FriendlyName -match "GhostCOM Virtual COM Port Controller"
    } |
    Select-Object -First 1

# ── Install ────────────────────────────────────────────────────

if (-not $existing) {
    # First-time install or post-uninstall - no Root\GhostCOM node exists,
    # and pnputil can't create one on its own. Fall back to our SetupAPI
    # bootstrap (the devcon-free devcon-install replacement).
    Write-Host "No existing Root\GhostCOM device - creating one via SetupAPI." -ForegroundColor Green

    $createScript = Join-Path $PSScriptRoot "create-root-device.ps1"
    if (-not (Test-Path $createScript)) {
        Write-Error "create-root-device.ps1 not found at $createScript"
        exit 1
    }

    & $createScript -InfPath $infFile
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to create root device (exit $LASTEXITCODE)."
        exit 1
    }
} else {
    Write-Host "Found existing device $($existing.InstanceId) - updating its driver." -ForegroundColor Green

    $result = & pnputil /add-driver $infFile /install 2>&1 | Out-String
    Write-Host $result

    # pnputil exits non-zero on "Added driver packages: 0" (i.e. the
    # staged package matches the one already on the device - a no-op,
    # not a failure). Treat any of these stdout markers as success:
    $successMarkers = @(
        "Driver package added successfully",
        "Driver package installed on device",
        "Driver package is up-to-date on device"
    )
    $success = $false
    foreach ($marker in $successMarkers) {
        if ($result -match [regex]::Escape($marker)) {
            $success = $true
            break
        }
    }

    if (-not $success) {
        Write-Error @"
Driver installation failed.

Make sure:
1. You are running as Administrator
2. Test signing is enabled (run with -TestSign flag, then reboot)
3. The driver is properly signed (bun run build:driver signs by default)

To enable test signing:
    bcdedit /set testsigning on
    (reboot)
"@
        exit 1
    }
}

Write-Host ""
Write-Host "Driver installed successfully!" -ForegroundColor Green
Write-Host ""
Write-Host "Verify with:" -ForegroundColor Cyan
Write-Host "  Get-PnpDevice -Class System | Where-Object FriendlyName -match GhostCOM"
Write-Host "  pnputil /enum-drivers | Select-String -Context 3 'GhostCOM'"
