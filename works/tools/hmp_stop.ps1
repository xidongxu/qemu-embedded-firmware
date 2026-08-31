# Connect to QEMU HMP monitor on 127.0.0.1:4444, pause the guest and dump
# the CPU PC (the guest is stuck in a busy-wait spin, so PC points at the
# offending loop).
param([string]$Cmd = 'info registers')
$c = New-Object System.Net.Sockets.TcpClient('127.0.0.1', 4444)
$c.ReceiveTimeout = 3000
$s = $c.GetStream()
$writer = New-Object System.IO.StreamWriter($s)
$writer.AutoFlush = $true
$reader = New-Object System.IO.StreamReader($s)
# drain banner up to first (qemu) prompt
$sb = New-Object System.Text.StringBuilder
$start = [DateTime]::Now
while ([DateTime]::Now.Subtract($start).TotalMilliseconds -lt 2000) {
    if ($s.DataAvailable) {
        $ch = $reader.Read()
        if ($ch -lt 0) { break }
        [void]$sb.Append([char]$ch)
    } else { Start-Sleep -Milliseconds 50 }
}
$writer.WriteLine('stop')
Start-Sleep -Milliseconds 300
$writer.WriteLine($Cmd)
Start-Sleep -Milliseconds 500
$out = New-Object System.Text.StringBuilder
$start = [DateTime]::Now
while ([DateTime]::Now.Subtract($start).TotalMilliseconds -lt 1500) {
    if ($s.DataAvailable) {
        $ch = $reader.Read()
        if ($ch -lt 0) { break }
        [void]$out.Append([char]$ch)
    } else { Start-Sleep -Milliseconds 50 }
}
$writer.WriteLine('cont')
Start-Sleep -Milliseconds 100
$writer.Close()
$c.Close()
$txt = $out.ToString()
# print PC line if present
$txt -split "`n" | Where-Object { $_ -match 'R15|r15| pc |pc=' } | ForEach-Object { $_.Trim() }
Write-Host '---raw tail---'
($txt -split "`n" | Select-Object -Last 12) -join "`n"
