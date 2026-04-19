<#
.SYNOPSIS
    Build the GhostCOM kernel driver.

.DESCRIPTION
    Invokes MSBuild with WDK targets to build the driver.
    Requires Visual Studio and the Windows Driver Kit (WDK) to be installed.

.PARAMETER Configuration
    Build configuration: Debug or Release.

.PARAMETER Platform
    Target platform: x64 or ARM64.

.PARAMETER Sign
    If specified, self-sign the driver for test signing.
#>

param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [ValidateSet("x64", "ARM64")]
    [string]$Platform = "x64",

    [switch]$Sign
)

$ErrorActionPreference = "Stop"

Write-Host "GhostCOM driver build" -ForegroundColor Cyan
Write-Host "=====================" -ForegroundColor Cyan
Write-Host "Configuration: $Configuration"
Write-Host "Platform:      $Platform"
Write-Host ""

$driverDir = Join-Path $PSScriptRoot "..\driver"
$projectFile = Join-Path $driverDir "ghostcom.vcxproj"

# ── Check prerequisites ───────────────────────────────────────

# Find MSBuild. -latest alone misses BuildTools; -products * includes them.
$msbuild = $null
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vsWhere) {
    $vsPath = & $vsWhere -latest -products * -property installationPath 2>$null
    if ($vsPath) {
        $msbuild = Get-ChildItem "$vsPath\MSBuild" -Recurse -Filter "MSBuild.exe" |
                   Where-Object { $_.FullName -match "amd64" } |
                   Select-Object -First 1 -ExpandProperty FullName
    }
}

# Fallback: probe well-known BuildTools path directly.
if (-not $msbuild) {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $msbuild = $c; break }
    }
}

if (-not $msbuild) {
    Write-Error @"
MSBuild not found. Please install:
1. Visual Studio 2022 (Community or higher)
2. "Desktop development with C++" workload
3. Windows Driver Kit (WDK) for Windows 11

Download WDK: https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk
"@
    exit 1
}

Write-Host "Using MSBuild: $msbuild" -ForegroundColor Gray

# Check for WDK
$wdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
if (-not (Test-Path "$wdkRoot\Include")) {
    Write-Error "Windows Driver Kit not found at $wdkRoot. Install WDK first."
    exit 1
}

# ── Check for .vcxproj ─────────────────────────────────────────

if (-not (Test-Path $projectFile)) {
    Write-Host "No .vcxproj found. You need to create the Visual Studio project." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Quick setup:" -ForegroundColor Cyan
    Write-Host "1. Open Visual Studio"
    Write-Host "2. Create new project → 'Kernel Mode Driver, Empty (KMDF)'"
    Write-Host "3. Add existing source files from driver/src/"
    Write-Host "4. Save the project as driver/ghostcom.vcxproj"
    Write-Host ""
    Write-Host "Or use the EWDK (Enterprise WDK) standalone build environment."
    exit 1
}

# ── Build ──────────────────────────────────────────────────────

Write-Host "Building driver..." -ForegroundColor Green

$buildDir = Join-Path $driverDir "build\$Platform\$Configuration"
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

& $msbuild $projectFile `
    /p:Configuration=$Configuration `
    /p:Platform=$Platform `
    /p:OutDir="$buildDir\" `
    /v:minimal

if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed."
    exit 1
}

Write-Host ""
Write-Host "Build succeeded! Output: $buildDir" -ForegroundColor Green

# ── Copy INF ───────────────────────────────────────────────────

$infSrc = Join-Path $driverDir "ghostcom.inf"
if (Test-Path $infSrc) {
    Copy-Item $infSrc $buildDir -Force
    Write-Host "Copied INF file to output directory."
}

# ── Self-sign for testing ─────────────────────────────────────

if ($Sign) {
    Write-Host ""
    Write-Host "Self-signing driver for test mode..." -ForegroundColor Yellow

    $certName = "GhostCOMTestCert"
    $sysPath = Join-Path $buildDir "ghostcom.sys"

    # Create a self-signed certificate if one doesn't exist.
    $cert = Get-ChildItem Cert:\CurrentUser\My |
            Where-Object { $_.Subject -eq "CN=$certName" } |
            Select-Object -First 1

    if (-not $cert) {
        Write-Host "Creating self-signed test certificate..."
        $cert = New-SelfSignedCertificate `
            -CertStoreLocation Cert:\CurrentUser\My `
            -Subject "CN=$certName" `
            -Type CodeSigningCert `
            -KeyExportPolicy Exportable

        # Trust the cert
        $store = New-Object System.Security.Cryptography.X509Certificates.X509Store(
            "TrustedPublisher", "LocalMachine")
        $store.Open("ReadWrite")
        $store.Add($cert)
        $store.Close()

        $store = New-Object System.Security.Cryptography.X509Certificates.X509Store(
            "Root", "LocalMachine")
        $store.Open("ReadWrite")
        $store.Add($cert)
        $store.Close()
    }

    # Sign the driver
    $signtool = Get-ChildItem "$wdkRoot\bin" -Recurse -Filter "signtool.exe" |
                Where-Object { $_.FullName -match "x64" } |
                Select-Object -First 1 -ExpandProperty FullName

    if ($signtool) {
        & $signtool sign /fd SHA256 /a /s My /n $certName $sysPath
        if ($LASTEXITCODE -eq 0) {
            Write-Host "Driver signed successfully." -ForegroundColor Green
        } else {
            Write-Warning "Signing failed. You may need to sign manually."
        }

        # Create catalog
        $inf2cat = Get-ChildItem "$wdkRoot\bin" -Recurse -Filter "Inf2Cat.exe" |
                   Select-Object -First 1 -ExpandProperty FullName
        if ($inf2cat) {
            & $inf2cat /driver:$buildDir /os:10_x64
            $catPath = Join-Path $buildDir "ghostcom.cat"
            if (Test-Path $catPath) {
                & $signtool sign /fd SHA256 /a /s My /n $certName $catPath
            }
        }
    } else {
        Write-Warning "signtool.exe not found. Cannot sign driver."
    }
}

Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "  Install: bun run install:driver"
Write-Host "  Test:    sc query GhostCOM"
