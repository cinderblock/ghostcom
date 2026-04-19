param([string]$Port = "COM20", [int]$Bytes = 6, [int]$TimeoutMs = 5000)
$sp = New-Object System.IO.Ports.SerialPort($Port, 9600, "None", 8, "One")
$sp.ReadTimeout = $TimeoutMs
$sp.Open()
Write-Host "OPEN"
try {
    $buf = New-Object byte[] $Bytes
    $read = 0
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($read -lt $Bytes -and $sw.ElapsedMilliseconds -lt $TimeoutMs) {
        $n = $sp.Read($buf, $read, $Bytes - $read)
        $read += $n
    }
    $str = [System.Text.Encoding]::ASCII.GetString($buf, 0, $read)
    Write-Host "RECEIVED:$read:$str"
} catch {
    Write-Host "ERROR:$($_.Exception.Message)"
} finally {
    $sp.Close()
}
