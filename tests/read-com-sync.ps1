param([string]$Port = "COM10", [int]$Bytes = 6, [int]$TimeoutMs = 8000)

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class WinCom {
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Auto)]
    public static extern IntPtr CreateFile(string name, uint acc, uint share, IntPtr sa, uint cd, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool ReadFile(IntPtr h, byte[] buf, int n, out int read, IntPtr ov);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] public static extern bool SetCommTimeouts(IntPtr h, ref COMMTIMEOUTS t);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool SetupComm(IntPtr h, int inQ, int outQ);

    [StructLayout(LayoutKind.Sequential)]
    public struct COMMTIMEOUTS {
        public uint ReadIntervalTimeout;
        public uint ReadTotalTimeoutMultiplier;
        public uint ReadTotalTimeoutConstant;
        public uint WriteTotalTimeoutMultiplier;
        public uint WriteTotalTimeoutConstant;
    }
}
"@

$path = "\\.\$Port"
$handle = [WinCom]::CreateFile($path, 0xC0000000, 0, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
$err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()

if ($handle -eq [IntPtr]-1 -or $handle -eq [IntPtr]::Zero) {
    Write-Host "ERROR:Failed to open $Port (err=$err)"
    exit 1
}

Write-Host "OPEN"

# Set total read timeout
$t = [WinCom+COMMTIMEOUTS]::new()
$t.ReadIntervalTimeout = 0
$t.ReadTotalTimeoutMultiplier = 0
$t.ReadTotalTimeoutConstant = $TimeoutMs
[WinCom]::SetCommTimeouts($handle, [ref]$t) | Out-Null

$buf = New-Object byte[] $Bytes
$totalRead = 0
$sw = [Diagnostics.Stopwatch]::StartNew()

while ($totalRead -lt $Bytes -and $sw.ElapsedMilliseconds -lt $TimeoutMs) {
    $n = 0
    $ok = [WinCom]::ReadFile($handle, $buf, $Bytes - $totalRead, [ref]$n, [IntPtr]::Zero)
    if (!$ok -or $n -eq 0) { break }
    # Copy into offset position
    $totalRead += $n
}

$str = [Text.Encoding]::ASCII.GetString($buf, 0, $totalRead)
Write-Host "RECEIVED:$totalRead:$str"
[WinCom]::CloseHandle($handle) | Out-Null
