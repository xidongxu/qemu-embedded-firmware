# 发送有效 RTP 包 (PCMU/8000/20ms 静音) 到指定地址/端口
# 用于验证 QEMU hostfwd 4000 是否真正转发到 guest 的 RTP socket
# (垃圾包会被 pjsua 丢弃, 有效 RTP 会计入 rtcp.rx.pkt 从而让 wd 的 rx_pkt 增长)
param(
    [string]$Addr = "127.0.0.1",
    [int]$Port = 4000,
    [int]$Count = 30,
    [string]$Src = ""   # optional source IP to bind (e.g. 192.168.23.7 to mimic FS)
)
$c = New-Object System.Net.Sockets.UdpClient
if ($Src) {
    $srcEp = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($Src), 0)
    $c.Client.Bind($srcEp)
}
$ssrc = [uint32]0x12345678
$seq = [uint16]1000
$ts = [uint32]0

for ($i = 0; $i -lt $Count; $i++) {
    $rtp = New-Object byte[] (12 + 160)
    $rtp[0] = 0x80  # v=2, no marker
    $rtp[1] = 0x00  # PT=0 (PCMU)
    $rtp[2] = (($seq -shr 8) -band 0xFF)
    $rtp[3] = ($seq -band 0xFF)
    $rtp[4] = (($ts -shr 24) -band 0xFF)
    $rtp[5] = (($ts -shr 16) -band 0xFF)
    $rtp[6] = (($ts -shr 8) -band 0xFF)
    $rtp[7] = ($ts -band 0xFF)
    $rtp[8] = (($ssrc -shr 24) -band 0xFF)
    $rtp[9] = (($ssrc -shr 16) -band 0xFF)
    $rtp[10] = (($ssrc -shr 8) -band 0xFF)
    $rtp[11] = ($ssrc -band 0xFF)
    for ($k = 12; $k -lt $rtp.Length; $k++) { $rtp[$k] = 0x80 }  # PCMU silence
    try {
        [void]$c.Send($rtp, $rtp.Length, $Addr, $Port)
    } catch {
        Write-Host ("send #{0} failed: {1}" -f $i, $_.Exception.Message)
    }
    $seq = ($seq + 1) -band 0xFFFF
    $ts = ($ts + 160) -band 0xFFFFFFFF
    Start-Sleep -Milliseconds 20
}
$c.Close()
Write-Host ("sent {0} valid RTP pkts to {1}:{2} (src={3})" -f $Count, $Addr, $Port, $(if($Src){"$Src"}else{"default"}))
