param([int]$port = 55)
try {
    $sp = New-Object System.IO.Ports.SerialPort "COM$port", 9600, None, 8, One
    $sp.ReadTimeout = 8000
    $sp.Open()
    Write-Host "SerialPort opened COM$port"
    $buf = New-Object byte[] 64
    $n = $sp.Read($buf, 0, 64)
    Write-Host "Read $n bytes: $([System.Text.Encoding]::ASCII.GetString($buf, 0, $n))"
    $sp.Close()
} catch {
    Write-Host "ERROR: $_"
}
