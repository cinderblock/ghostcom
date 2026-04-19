param([string]$Port = "COM10", [int]$Bytes = 6, [int]$TimeoutMs = 8000)
Write-Output "STARTING port=$Port"
$path = "\\.\$Port"
try {
    $fs = [System.IO.FileStream]::new($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite)
    Write-Output "OPEN"
    $buf = New-Object byte[] $Bytes
    $total = 0
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($total -lt $Bytes -and $sw.ElapsedMilliseconds -lt $TimeoutMs) {
        $n = $fs.Read($buf, $total, $Bytes - $total)
        if ($n -gt 0) { $total += $n }
        else { Start-Sleep -Milliseconds 50 }
    }
    $str = [System.Text.Encoding]::ASCII.GetString($buf, 0, $total)
    Write-Output "RECEIVED:$total:$str"
    $fs.Close()
} catch {
    Write-Output "ERROR:$($_.Exception.Message)"
}
