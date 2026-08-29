# run_smoke_regression.ps1 - QEMU guest phone smoke regression.
#
# Drives the guest phone over its UDP command server (port 15000):
#   for each iteration: dial 9196 -> wait ACTIVE -> check RTP rx/tx ->
#   send DTMF 1234 -> hangup.  Reports PASS/FAIL per iteration and a summary.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File works\tools\run_smoke_regression.ps1
#   powershell -ExecutionPolicy Bypass -File works\tools\run_smoke_regression.ps1 -Iterations 10
#   powershell -ExecutionPolicy Bypass -File works\tools\run_smoke_regression.ps1 -Number 1005
#
# Prereq: QEMU guest up + registered; FreeSWITCH running.
# NOTE: keep this file ASCII-only (PowerShell 5.1 decodes .ps1 without BOM
# using the system ANSI codepage).

param(
    [string]$GuestIp      = '172.16.23.50',
    [int]   $Port         = 15000,
    [string]$Number       = '9196',
    [int]   $Iterations   = 3,
    [int]   $CallTimeoutS = 30,   # v1.11.3 call setup can take ~15s
    [int]   $MediaWaitS   = 3
)

$ErrorActionPreference = 'Stop'
$pass = 0
$fail = 0

function Send-Udp([string]$cmd, [int]$timeoutMs = 3000) {
    $u = New-Object Net.Sockets.UdpClient
    $u.Client.ReceiveTimeout = $timeoutMs
    $b = [Text.Encoding]::ASCII.GetBytes($cmd)
    try {
        $u.Send($b, $b.Length, $GuestIp, $Port) | Out-Null
        $e = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
        $r = $u.Receive([ref]$e)
        return ([Text.Encoding]::ASCII.GetString($r)).Trim()
    } catch {
        return $null
    } finally {
        $u.Close()
    }
}

function Get-CallState([string]$resp) {
    if ($resp -match 'call=(\w+)') { return $matches[1] }
    return ''
}

function Get-UdpPcbUsed([string]$resp) {
    if ($resp -match 'UDP_PCB (\d+)/') { return [int]$matches[1] }
    return -1
}

Write-Host ("Smoke regression: guest={0} number={1} iterations={2}" -f $GuestIp, $Number, $Iterations) -ForegroundColor Cyan

# --- pre-check: guest reachable, no active call ---
$s0 = Send-Udp 'status'
if ($null -eq $s0) {
    Write-Host '[FATAL] guest not reachable' -ForegroundColor Red
    exit 1
}
if ((Get-CallState $s0) -ne 'IDLE') {
    Write-Host '[WARN] active call before test - hanging up' -ForegroundColor Yellow
    Send-Udp 'hangup'
    Start-Sleep -Seconds 2
}

# Baseline lwIP resource usage (UDP_PCB must not grow across iterations).
$startTime = Get-Date
$baseMemp = Send-Udp 'memp'
$baseUdp = Get-UdpPcbUsed $baseMemp
Write-Host ("  baseline UDP_PCB used: {0}" -f $baseUdp)

for ($i = 1; $i -le $Iterations; $i++) {
    Write-Host ("`n==== iteration {0}/{1} ====" -f $i, $Iterations) -ForegroundColor Cyan
    $iterStart = Get-Date
    $ok = $true

    # 1. dial
    $d = Send-Udp "dial $Number"
    Write-Host ("  dial  : {0}" -f $d)
    if ($null -eq $d -or $d -notmatch 'rc=0') {
        Write-Host '  FAIL dial' -ForegroundColor Red
        $fail++
        Write-Host '  ==> FAIL' -ForegroundColor Red
        continue
    }

    # 2. wait for ACTIVE
    $state = ''
    $deadline = (Get-Date).AddSeconds($CallTimeoutS)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 800
        $resp = Send-Udp 'status'
        $state = Get-CallState $resp
        if ($state -eq 'ACTIVE') { break }
    }
    Write-Host ("  call  : {0}" -f $state)
    if ($state -ne 'ACTIVE') {
        Write-Host '  FAIL call not ACTIVE' -ForegroundColor Red
        $ok = $false
        Send-Udp 'hangup'
        continue
    }

    # 3. media check: stat rx/tx must increase between two samples
    Start-Sleep -Seconds $MediaWaitS
    $st1 = Send-Udp 'stat'
    Start-Sleep -Seconds 2
    $st2 = Send-Udp 'stat'
    Write-Host ("  stat  : {0}  ->  {1}" -f $st1, $st2)
    $rx1 = 0; $tx1 = 0
    if ($st1 -match 'rx=(\d+).*tx=(\d+)') { $rx1 = [int]$matches[1]; $tx1 = [int]$matches[2] }
    $rx2 = 0; $tx2 = 0
    if ($st2 -match 'rx=(\d+).*tx=(\d+)') { $rx2 = [int]$matches[1]; $tx2 = [int]$matches[2] }
    if ($rx2 -le $rx1 -or $tx2 -le $tx1) {
        Write-Host '  FAIL media not flowing (rx/tx not increasing)' -ForegroundColor Red
        $ok = $false
    } else {
        Write-Host ("  media : OK (rx {0}->{1}, tx {2}->{3})" -f $rx1, $rx2, $tx1, $tx2) -ForegroundColor Green
    }

    # 4. dtmf
    $dt = Send-Udp 'dtmf 1234'
    Write-Host ("  dtmf  : {0}" -f $dt)
    if ($null -eq $dt -or $dt -notmatch 'rc=0') {
        Write-Host '  FAIL dtmf' -ForegroundColor Red
        $ok = $false
    }

    # 5. hangup
    $h = Send-Udp 'hangup'
    Start-Sleep -Seconds 1
    $sEnd = Send-Udp 'status'
    Write-Host ("  hangup: {0}  ->  {1}" -f $h, $sEnd)
    if ((Get-CallState $sEnd) -ne 'IDLE') {
        Write-Host '  FAIL hangup (call not IDLE)' -ForegroundColor Red
        $ok = $false
    }

    # 6. resource check: UDP_PCB must stay near baseline (no leak)
    $m = Send-Udp 'memp'
    $udpUsed = Get-UdpPcbUsed $m
    Write-Host ("  memp  : {0}" -f $m)
    if ($udpUsed -gt ($baseUdp + 2)) {
        Write-Host ("  FAIL UDP_PCB leaked (used {0} > baseline {1}+2)" -f $udpUsed, $baseUdp) -ForegroundColor Red
        $ok = $false
    }

    $iterMs = ((Get-Date) - $iterStart).TotalMilliseconds
    Write-Host ("  time  : {0:N1}s" -f ($iterMs / 1000))

    if ($ok) { $pass++; Write-Host '  ==> PASS' -ForegroundColor Green }
    else     { $fail++; Write-Host '  ==> FAIL' -ForegroundColor Red }
}

$totalMs = ((Get-Date) - $startTime).TotalMilliseconds
Write-Host ("`n==== SUMMARY ====") -ForegroundColor Cyan
Write-Host ("PASS: {0}   FAIL: {1}   elapsed: {2:N1}s ({3:N1} min)" -f $pass, $fail, ($totalMs/1000), ($totalMs/60000))
if ($fail -eq 0) {
    Write-Host 'ALL PASSED' -ForegroundColor Green
    exit 0
} else {
    Write-Host 'SOME FAILED' -ForegroundColor Red
    exit 1
}
