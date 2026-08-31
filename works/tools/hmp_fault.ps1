# Connect to QEMU HMP, stop the guest, dump Cortex-M33 fault registers.
$c = New-Object System.Net.Sockets.TcpClient('127.0.0.1', 4444)
$c.ReceiveTimeout = 3000
$s = $c.GetStream()
$writer = New-Object System.IO.StreamWriter($s)
$writer.AutoFlush = $true
$reader = New-Object System.IO.StreamReader($s)
$start = [DateTime]::Now
while ([DateTime]::Now.Subtract($start).TotalMilliseconds -lt 1500) {
    if ($s.DataAvailable) { $ch = $reader.Read(); if ($ch -lt 0) { break } } else { Start-Sleep -Milliseconds 40 }
}
$writer.WriteLine('stop')
Start-Sleep -Milliseconds 300
$writer.WriteLine('xp /8 0xE000ED20')
Start-Sleep -Milliseconds 600
$out = New-Object System.Text.StringBuilder
$start = [DateTime]::Now
while ([DateTime]::Now.Subtract($start).TotalMilliseconds -lt 1500) {
    if ($s.DataAvailable) { $ch = $reader.Read(); if ($ch -lt 0) { break }; [void]$out.Append([char]$ch) } else { Start-Sleep -Milliseconds 40 }
}
$writer.WriteLine('cont')
Start-Sleep -Milliseconds 100
$writer.Close(); $c.Close()
Write-Host '=== SCB fault area (CFSR@0x28 HFSR@0x2C MMFAR@0x34 BFAR@0x38) ==='
($out.ToString() -split "`n") | ForEach-Object { $_.TrimEnd() }
