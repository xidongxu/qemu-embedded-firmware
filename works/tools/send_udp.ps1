# 发送 UDP 测试包到指定地址/端口，用于验证 QEMU hostfwd 转发
# 用法: powershell -File send_udp.ps1 -Addr 127.0.0.1 -Port 4000 -Count 20
param(
    [string]$Addr = "127.0.0.1",
    [int]$Port = 4000,
    [int]$Count = 20
)
$c = New-Object System.Net.Sockets.UdpClient
$data = [Text.Encoding]::ASCII.GetBytes("RTPTEST")
$sent = 0
for ($i = 0; $i -lt $Count; $i++) {
    try {
        [void]$c.Send($data, $data.Length, $Addr, $Port)
        $sent++
    } catch {
        Write-Host ("send #{0} failed: {1}" -f $i, $_.Exception.Message)
    }
    Start-Sleep -Milliseconds 100
}
$c.Close()
Write-Host ("sent {0}/{1} UDP pkts to {2}:{3}" -f $sent, $Count, $Addr, $Port)
