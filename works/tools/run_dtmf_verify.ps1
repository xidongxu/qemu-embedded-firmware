# run_dtmf_verify.ps1 - verify guest -> FreeSWITCH -> pjsua DTMF chain.
#
# Starts a pjsua UAS that registers to FreeSWITCH as extension 1006
# (auto-answer, null audio).  The QEMU guest phone calls 1006 and sends
# DTMF; pjsua logs every received DTMF digit.  After the wait window the
# script stops pjsua and reports which digits made it through the chain.
#
# Workflow:
#   1. Run this script (it starts pjsua and waits for registration).
#   2. On the QEMU guest phone, dial 1006, then press DTMF digits.
#   3. The script reports the received DTMF after the wait window.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File works\tools\run_dtmf_verify.ps1
#   powershell -ExecutionPolicy Bypass -File works\tools\run_dtmf_verify.ps1 -WaitSeconds 180
#
# NOTE: keep this file ASCII-only (PowerShell 5.1 decodes .ps1 without BOM
# using the system ANSI codepage).

param(
    [string]$FsHost      = '192.168.23.7',
    [int]   $SipPort     = 5062,
    [int]   $RtpPort     = 6000,
    [string]$Ext         = '1006',
    [string]$Password    = '1234',
    [int]   $WaitSeconds = 120,
    [string]$LogDir      = 'C:\Users\xidon\code\github\qemu-embedded-firmware\works\logs'
)

$ErrorActionPreference = 'Stop'

$pjsua = 'C:\Users\xidon\program\pjproject-2.17\build-win64\pjsip-apps\Release\pjsua.exe'
if (-not (Test-Path $pjsua)) { Write-Host "[X] pjsua not found: $pjsua" -ForegroundColor Red; exit 1 }

New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$log   = Join-Path $LogDir "dtmf_verify_$stamp.log"

$pjsuaArgs = @(
    "--id=sip:$Ext@$FsHost",
    "--registrar=sip:$FsHost",
    "--realm=$FsHost",
    "--username=$Ext",
    "--password=$Password",
    "--local-port=$SipPort",
    "--rtp-port=$RtpPort",
    "--auto-answer=200",
    "--null-audio",
    "--log-level=5",
    "--app-log-level=5",
    "--log-file=$log"
)

Write-Host "Starting pjsua UAS  ext=$Ext@$FsHost  localSip=$SipPort  rtp=$RtpPort" -ForegroundColor Cyan
$p = Start-Process -FilePath $pjsua -ArgumentList $pjsuaArgs -PassThru -WindowStyle Hidden `
     -RedirectStandardOutput "$log.stdout" -RedirectStandardError "$log.stderr"

# Wait for registration to FreeSWITCH (up to 30 s).
$registered = $false
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 500
    if ($p.HasExited) { break }
    if (Test-Path $log) {
        if (Select-String -Path $log -Pattern 'registration success' -Quiet) { $registered = $true; break }
        if (Select-String -Path $log -Pattern 'registration failed' -Quiet) { break }
    }
}

if ($registered) {
    Write-Host "[OK] registered to $FsHost as $Ext (auto-answer)." -ForegroundColor Green
} else {
    Write-Host "[!] registration not confirmed yet - check log: $log" -ForegroundColor Yellow
}

Write-Host ''
Write-Host '====================================================================' -ForegroundColor Yellow
Write-Host 'NOW on the QEMU guest phone:' -ForegroundColor Yellow
Write-Host "  1. Dial  $Ext   (it auto-answers with 200 OK)" -ForegroundColor Yellow
Write-Host '  2. In the active call, press DTMF digits, e.g. 1 2 3 4 * #' -ForegroundColor Yellow
Write-Host "  3. Wait for the window (${WaitSeconds}s) to finish" -ForegroundColor Yellow
Write-Host '====================================================================' -ForegroundColor Yellow
Write-Host ''

for ($i = $WaitSeconds; $i -gt 0; $i--) {
    Write-Host ("   waiting... {0,3}s (DTMF so far below)" -f $i) -NoNewline
    Start-Sleep -Seconds 1
    Write-Host ("`r" + ' ' * 60) -NoNewline
    if (Test-Path $log) {
        $seen = Select-String -Path $log -Pattern 'DTMF received' | Select-Object -Last 3
        if ($seen) {
            Write-Host ("`r" + ' ' * 60)
            $seen | ForEach-Object { Write-Host ("   rx: " + $_.Line.Trim()) -ForegroundColor Green }
        }
    }
}

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

Write-Host ''
Write-Host ''
Write-Host '==================== pjsua DTMF log ====================' -ForegroundColor Cyan
$hits = @()
foreach ($f in @($log, "$log.stdout", "$log.stderr")) {
    if (Test-Path $f) {
        $hits += Select-String -Path $f -Pattern 'DTMF received' | ForEach-Object { $_.Line.Trim() }
    }
}

if ($hits.Count -gt 0) {
    $digits = ''
    foreach ($h in $hits) { if ($h -match 'DTMF received:\s*(\S+)') { $digits += $matches[1] } }
    $hits | Select-Object -Last 40 | ForEach-Object { Write-Host $_ -ForegroundColor Green }
    Write-Host ''
    Write-Host "[RESULT] pjsua received DTMF digits: [$digits]" -ForegroundColor Green
    Write-Host '        -> guest->FreeSWITCH->SIP-endpoint DTMF chain is UP.' -ForegroundColor Green
} else {
    Write-Host '(no "DTMF received" lines found in pjsua log)' -ForegroundColor Red
    Write-Host "full log: $log" -ForegroundColor Red
}
Write-Host "pjsua log: $log" -ForegroundColor DarkGray
