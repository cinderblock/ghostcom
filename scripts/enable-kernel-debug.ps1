#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Enable kernel debugging, complete memory dumps, and Driver Verifier on
    the GhostCOM driver so kernel bugs produce diagnostic dumps instead of
    silent BSODs.

.DESCRIPTION
    Run this ONCE as Administrator (right-click PowerShell → Run as Admin,
    then invoke the script). Requires a REBOOT for all changes to take effect.

    After reboot:
      - Any BSOD from ghostcom.sys will produce a useful dump at
        C:\Windows\MEMORY.DMP that WinDbg can analyse.
      - Driver Verifier will bugcheck early with a specific error code
        instead of letting the driver corrupt memory silently.

.NOTES
    To DISABLE later, run: `verifier /reset` and reboot.
#>

$ErrorActionPreference = "Stop"

Write-Host "=== GhostCOM kernel debugging setup ===" -ForegroundColor Cyan
Write-Host ""

# ── 1. Enable kernel debugging via boot config ────────────────────────────────

Write-Host "1. Enabling kernel debug mode (bcdedit /debug on)" -ForegroundColor Yellow
bcdedit /debug on | Out-Null
bcdedit /bootdebug on | Out-Null
Write-Host "   OK — kernel debug hooks active after reboot" -ForegroundColor Green

# ── 2. Set complete memory dump (full kernel state on BSOD) ───────────────────

Write-Host ""
Write-Host "2. Configuring complete memory dumps" -ForegroundColor Yellow

$crashKey = "HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl"
# CrashDumpEnabled = 1 → full memory dump (captures entire physical RAM)
Set-ItemProperty -Path $crashKey -Name "CrashDumpEnabled" -Value 1 -Type DWord
# Overwrite existing dump file each time
Set-ItemProperty -Path $crashKey -Name "Overwrite" -Value 1 -Type DWord
# Keep last dump after reboot
Set-ItemProperty -Path $crashKey -Name "LogEvent" -Value 1 -Type DWord
# Always prompt/save
Set-ItemProperty -Path $crashKey -Name "AutoReboot" -Value 0 -Type DWord -ErrorAction SilentlyContinue

# Ensure C:\Windows has space for full dump (physical RAM + 257 MB)
$dumpPath = "C:\Windows\MEMORY.DMP"
Set-ItemProperty -Path $crashKey -Name "DumpFile" -Value $dumpPath -Type ExpandString

# Ensure pagefile is large enough (must be ≥ physical RAM + 257MB for complete dump)
$ram = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
$ramGB = [math]::Round($ram / 1GB, 1)
Write-Host "   RAM: $ramGB GB → pagefile should be ≥ $([math]::Ceiling($ramGB + 1)) GB for complete dump"
Write-Host "   OK — dumps will go to C:\Windows\MEMORY.DMP" -ForegroundColor Green

# ── 3. Enable Driver Verifier on ghostcom.sys ─────────────────────────────────

Write-Host ""
Write-Host "3. Enabling Driver Verifier on ghostcom.sys (standard flags)" -ForegroundColor Yellow
Write-Host "   Standard flags catch: special pool corruption, IRQL violations,"
Write-Host "   pool-tracking leaks, I/O verification, deadlock detection."
# /standard enables the recommended set of flags
# /driver ghostcom.sys limits verification to just our driver
verifier /standard /driver ghostcom.sys
Write-Host ""
Write-Host "   OK — Driver Verifier will attach to ghostcom.sys on next boot" -ForegroundColor Green

# ── 4. Summary ───────────────────────────────────────────────────────────────

Write-Host ""
Write-Host "=== REBOOT REQUIRED ===" -ForegroundColor Red
Write-Host ""
Write-Host "After reboot:" -ForegroundColor Cyan
Write-Host "  - Any kernel bug in ghostcom.sys will produce a bugcheck with"
Write-Host "    a specific error code pointing at the problem."
Write-Host "  - Complete memory dumps will land at C:\Windows\MEMORY.DMP"
Write-Host "  - Analyze dumps with: windbg -z C:\Windows\MEMORY.DMP"
Write-Host "    then run '!analyze -v'"
Write-Host ""
Write-Host "Reboot now with: shutdown /r /t 0" -ForegroundColor Yellow
Write-Host ""
Write-Host "To DISABLE Driver Verifier later:" -ForegroundColor Gray
Write-Host "  verifier /reset    (then reboot)"
