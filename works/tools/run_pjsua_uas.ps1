# run_pjsua_uas.ps1 - host-side pjsua UAS (auto-answer) as the far end of a
# guest->host SIP call (Step 1: prove guest<->host interop through slirp).
#
# The guest (QEMU, PJ_HOST_CALL mode) dials sip:user@10.0.2.2:<SipPort>
# (slirp gateway = host loopback), so pjsua must listen on 127.0.0.1 (or
# 0.0.0.0).  pjsua answers with 200 OK (--auto-answer) and runs NULL audio
# so no sound card is needed.  RTP base port defaults to 5000 (avoid the
# guest's hostfwd 4000/4001 range).
#
# Usage:
#   .\run_pjsua_uas.ps1 [-SipPort 5060] [-RtpPort 5000] [-Duration 30]
#   .\run_pjsua_uas.ps1 -SipPort 5060 -RtpPort 5000 -Log pjsua_uas.log
param(
    [int]$SipPort   = 5060,
    [int]$RtpPort   = 5000,
    [int]$Duration  = 30,
    [string]$Log    = 'C:\Users\xidon\code\github\qemu-embedded-firmware\works\logs\pjsua_uas.log'
)

$pjsua = 'C:\Users\xidon\program\pjproject-2.17\build-win64\pjsip-apps\Release\pjsua.exe'
if (-not (Test-Path $pjsua)) {
    Write-Host "pjsua not found: $pjsua" -ForegroundColor Red
    exit 1
}

New-Item -ItemType Directory -Force -Path (Split-Path $Log) | Out-Null

$args = @(
    "--id=sip:phone@127.0.0.1:$SipPort",
    "--local-port=$SipPort",
    "--rtp-port=$RtpPort",
    "--auto-answer=200",
    "--null-audio",
    "--log-level=5",
    "--app-log-level=5",
    "--duration=$Duration"
)

Write-Host "Starting pjsua UAS  id=sip:phone@127.0.0.1:$SipPort  rtp=$RtpPort  auto-answer=200 (null-audio)"
Write-Host "Guest must dial:  sip:user@10.0.2.2:$SipPort"
& $pjsua $args 2>&1 | Tee-Object -FilePath $Log
