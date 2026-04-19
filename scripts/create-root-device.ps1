#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Create the Root\GhostCOM device node and install the driver onto it.

.DESCRIPTION
    Replacement for `devcon install ghostcom.inf Root\GhostCOM` when devcon
    is not available. Invokes the same SetupAPI + newdev.dll sequence that
    devcon uses internally: SetupDiCreateDeviceInfo -> SetupDiSetDeviceRegistryProperty
    (hardware ID) -> SetupDiCallClassInstaller(DIF_REGISTERDEVICE) ->
    UpdateDriverForPlugAndPlayDevices.

    Needed because pnputil /add-driver /install only stages packages and
    binds them to EXISTING devices - it does not create ROOT-enumerated
    devnodes from scratch.
#>

param(
    [string]$InfPath = (Join-Path $PSScriptRoot "..\driver\build\x64\Release\ghostcom.inf"),
    [string]$HardwareId = "Root\GhostCOM"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $InfPath)) {
    Write-Error "INF not found at: $InfPath"
    exit 1
}

$InfPath = (Resolve-Path $InfPath).Path

Write-Host "Creating root device Root\GhostCOM and installing $InfPath" -ForegroundColor Cyan

$source = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class DevInstaller {
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid ClassGuid, IntPtr hwndParent);

    [DllImport("setupapi.dll", SetLastError = true)]
    public static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool SetupDiCreateDeviceInfoW(
        IntPtr DeviceInfoSet,
        [MarshalAs(UnmanagedType.LPWStr)] string DeviceName,
        ref Guid ClassGuid,
        [MarshalAs(UnmanagedType.LPWStr)] string DeviceDescription,
        IntPtr hwndParent,
        int CreationFlags,
        ref SP_DEVINFO_DATA DeviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    public static extern bool SetupDiSetDeviceRegistryPropertyW(
        IntPtr DeviceInfoSet,
        ref SP_DEVINFO_DATA DeviceInfoData,
        int Property,
        byte[] PropertyBuffer,
        int PropertyBufferSize);

    [DllImport("setupapi.dll", SetLastError = true)]
    public static extern bool SetupDiCallClassInstaller(
        int InstallFunction,
        IntPtr DeviceInfoSet,
        ref SP_DEVINFO_DATA DeviceInfoData);

    [DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode, EntryPoint = "UpdateDriverForPlugAndPlayDevicesW")]
    public static extern bool UpdateDriverForPlugAndPlayDevices(
        IntPtr hwndParent,
        [MarshalAs(UnmanagedType.LPWStr)] string HardwareId,
        [MarshalAs(UnmanagedType.LPWStr)] string FullInfPath,
        int InstallFlags,
        out bool bRebootRequired);

    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVINFO_DATA {
        public int cbSize;
        public Guid ClassGuid;
        public int DevInst;
        public IntPtr Reserved;
    }

    public const int SPDRP_HARDWAREID = 0x01;
    public const int DICD_GENERATE_ID = 0x01;
    public const int DIF_REGISTERDEVICE = 0x19;
    public const int INSTALLFLAG_FORCE = 0x01;

    public static int Install(string infPath, string hardwareId, Guid classGuid, string className) {
        IntPtr devInfoSet = SetupDiCreateDeviceInfoList(ref classGuid, IntPtr.Zero);
        if (devInfoSet == IntPtr.Zero || devInfoSet == new IntPtr(-1))
            return Marshal.GetLastWin32Error();

        try {
            SP_DEVINFO_DATA devInfoData = new SP_DEVINFO_DATA();
            devInfoData.cbSize = Marshal.SizeOf(devInfoData);

            if (!SetupDiCreateDeviceInfoW(devInfoSet, className, ref classGuid, null, IntPtr.Zero, DICD_GENERATE_ID, ref devInfoData))
                return Marshal.GetLastWin32Error();

            // MULTI_SZ hardware ID: the string followed by double null
            byte[] hwIdBytes = Encoding.Unicode.GetBytes(hardwareId + "\0\0");
            if (!SetupDiSetDeviceRegistryPropertyW(devInfoSet, ref devInfoData, SPDRP_HARDWAREID, hwIdBytes, hwIdBytes.Length))
                return Marshal.GetLastWin32Error();

            if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devInfoSet, ref devInfoData))
                return Marshal.GetLastWin32Error();

            bool rebootRequired;
            if (!UpdateDriverForPlugAndPlayDevices(IntPtr.Zero, hardwareId, infPath, INSTALLFLAG_FORCE, out rebootRequired))
                return Marshal.GetLastWin32Error();

            return 0;
        } finally {
            SetupDiDestroyDeviceInfoList(devInfoSet);
        }
    }
}
'@

Add-Type -TypeDefinition $source -Language CSharp -ErrorAction Stop

# System class GUID (matches INF: Class = System)
$systemClassGuid = [Guid]"4D36E97D-E325-11CE-BFC1-08002BE10318"

$rc = [DevInstaller]::Install($InfPath, $HardwareId, $systemClassGuid, "System")

if ($rc -eq 0) {
    Write-Host "Device created and driver installed." -ForegroundColor Green
    exit 0
} else {
    Write-Host "Install failed with Win32 error: $rc (0x{0:X})" -f $rc -ForegroundColor Red
    exit $rc
}
