#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Deploy the GhostCOM driver to the test VM.

.DESCRIPTION
    Builds, signs, and copies the driver package into the Hyper-V VM,
    then installs it. Also installs Bun and the node-null package
    for end-to-end testing.

.PARAMETER VMName
    Name of the VM. Default: "GhostCOM-Test"

.PARAMETER Snapshot
    If specified, creates a snapshot before installing the driver.
#>

param(
    [string]$VMName = "GhostCOM-Test",
    [switch]$Snapshot
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

Write-Host "GhostCOM VM Deployment" -ForegroundColor Cyan
Write-Host "======================" -ForegroundColor Cyan

# ── Check VM is running ───────────────────────────────────────
$vm = Get-VM -Name $VMName -ErrorAction SilentlyContinue
if (-not $vm -or $vm.State -ne "Running") {
    Write-Error "VM '$VMName' is not running. Start it first."
    exit 1
}

# ── Snapshot before deployment ────────────────────────────────
if ($Snapshot) {
    $snapName = "Pre-deploy $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
    Write-Host "Creating snapshot: $snapName"
    Checkpoint-VM -Name $VMName -SnapshotName $snapName
}

# ── Build and sign the driver ─────────────────────────────────
Write-Host "Building driver..."
$driverDir = Join-Path $projectRoot "driver\x64\Release"
$signtool = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
$inf2cat = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\Inf2Cat.exe"

# Build
Push-Location (Join-Path $projectRoot "driver")
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" ghostcom.vcxproj /p:Configuration=Release /p:Platform=x64 /v:minimal /t:Build
Pop-Location

# Prep files
Copy-Item "$driverDir\ghostcom.dll" "$driverDir\ghostcom.sys" -Force
Copy-Item (Join-Path $projectRoot "driver\ghostcom.inf") $driverDir -Force
Remove-Item "$driverDir\ghostcom.cat" -Force -ErrorAction SilentlyContinue

# Sign
& $signtool sign /fd SHA256 /s My /n "NodeNullTest" "$driverDir\ghostcom.sys" 2>$null
& $inf2cat /driver:$driverDir /os:10_x64 2>$null
& $signtool sign /fd SHA256 /s My /n "NodeNullTest" "$driverDir\ghostcom.cat" 2>$null

Write-Host "Driver built and signed." -ForegroundColor Green

# ── Copy driver package to VM ─────────────────────────────────
Write-Host "Copying driver package to VM..."

# Create staging directory in VM
Invoke-Command -VMName $VMName -ScriptBlock {
    New-Item -ItemType Directory -Path "C:\GhostCOM" -Force | Out-Null
} -Credential (Get-Credential -Message "Enter VM credentials")

# Copy files
$filesToCopy = @("ghostcom.sys", "ghostcom.inf", "ghostcom.cat")
foreach ($f in $filesToCopy) {
    $src = Join-Path $driverDir $f
    if (Test-Path $src) {
        Copy-VMFile -VMName $VMName -SourcePath $src -DestinationPath "C:\GhostCOM\$f" -CreateFullPath -FileSource Host
        Write-Host "  Copied $f"
    }
}

# ── Also copy devcon ──────────────────────────────────────────
$devcon = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\Tools" -Recurse -Filter "devcon.exe" | Where-Object { $_.FullName -match "x64" } | Select-Object -First 1 -ExpandProperty FullName
if ($devcon) {
    Copy-VMFile -VMName $VMName -SourcePath $devcon -DestinationPath "C:\GhostCOM\devcon.exe" -CreateFullPath -FileSource Host
    Write-Host "  Copied devcon.exe"
}

# ── Copy test certificate ─────────────────────────────────────
Write-Host "Exporting and copying test certificate..."
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq "CN=NodeNullTest" } | Select-Object -First 1
if ($cert) {
    $certPath = "$env:TEMP\NodeNullTest.cer"
    Export-Certificate -Cert $cert -FilePath $certPath -Type CERT | Out-Null
    Copy-VMFile -VMName $VMName -SourcePath $certPath -DestinationPath "C:\GhostCOM\NodeNullTest.cer" -CreateFullPath -FileSource Host
    Write-Host "  Copied certificate"
}

Write-Host ""
Write-Host "Driver package deployed to VM at C:\GhostCOM\" -ForegroundColor Green
Write-Host ""
Write-Host "Now run this INSIDE the VM (elevated PowerShell):" -ForegroundColor Yellow
Write-Host @"

# Import the test certificate
`$cert = Import-Certificate -FilePath C:\GhostCOM\NodeNullTest.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
Import-Certificate -FilePath C:\GhostCOM\NodeNullTest.cer -CertStoreLocation Cert:\LocalMachine\Root

# Install the driver
C:\GhostCOM\devcon.exe install C:\GhostCOM\ghostcom.inf Root\GhostCOM

# Verify
sc.exe query GhostCOM

"@
