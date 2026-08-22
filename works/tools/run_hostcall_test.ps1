# run_hostcall_test.ps1 - Step 1: prove guest<->host SIP/RTP interop.
# Starts a host-side pjsua UAS (auto-answer) + the QEMU guest in
# PJ_HOST_CALL mode; both run ~WaitSec seconds, then we report the key log
# lines from both sides.
#
# Topology (slirp user-net + hostfwd):
#   guest SIP bind 0.0.0.0:15062 ; dials  sip:user@10.0.2.2:<SipPort>
#   guest SDP offer  c=127.0.0.1 m=4000   (host sends RTP -> 127.0.0.1:4000)
#   hostfwd: udp::15062-:15062, udp::4000-:4000, udp::4001-:4001
#   host pjsua: --local-port=<SipPort> --rtp-port=<RtpPort> --auto-answer=200
param(
    [int]$SipPort  = 5060,
    [int]$RtpPort  = 5000,
    [int]$WaitSec  = 20
)

$root = 'C:\Users\xidon\code\github\qemu-embedded-firmware'
$tc   = 'C:\Users\xidon\code\github\qemu-embedded-platform\testcase'
$q    = 'C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe'
$pjsua= 'C:\Users\xidon\program\pjproject-2.17\build-win64\pjsip-apps\Release\pjsua.exe'
$elf  = "$tc\an505-hostcall.elf"
$lg   = "$root\works\logs"

New-Item -ItemType Directory -Force -Path $lg | Out-Null
Remove-Item "$lg\pjsua_host.log","$lg\pjsua_host.err","$lg\hostcall_guest.log","$lg\hostcall_guest.err" -Force -ErrorAction SilentlyContinue

# 1) host-side pjsua UAS
Start-Process -FilePath $pjsua -ArgumentList @(
    "--id=sip:phone@127.0.0.1:$SipPort",
    "--local-port=$SipPort",
    "--rtp-port=$RtpPort",
    "--auto-answer=200",
    "--null-audio",
    "--play-file=$tc\sine_1k_8k_10s.wav",
    "--log-level=5",
    "--app-log-level=5",
    "--duration=30"
) -RedirectStandardOutput "$lg\pjsua_host.log" -RedirectStandardError "$lg\pjsua_host.err" -NoNewWindow -PassThru | Out-Null
Start-Sleep -Seconds 3
Write-Host "pjsua UAS started (127.0.0.1:$SipPort rtp=$RtpPort)"

# 2) QEMU guest (PJ_HOST_CALL caller)
$ga = @(
    '-machine','mps2-an505',
    '-cpu','cortex-m33',
    '-m','16M',
    '-display','none',
    '-serial','stdio',
    '-nic',"user,id=n0,model=lan9118,mac=52:54:00:12:34:01,hostfwd=udp::15062-:15062,hostfwd=udp::4000-:4000,hostfwd=udp::4001-:4001",
    '-global',"mpsx-simple-mic.infile=$tc\sine_1k_8k_10s.wav",
    '-kernel',$elf
)
Start-Process -FilePath $q -ArgumentList $ga -RedirectStandardOutput "$lg\hostcall_guest.log" -RedirectStandardError "$lg\hostcall_guest.err" -NoNewWindow -PassThru | Out-Null
Write-Host "guest started, waiting ${WaitSec}s..."
Start-Sleep -Seconds $WaitSec

# 3) stop both
Get-Process qemu-system-arm,pjsua -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

# 4) report
Write-Host ""
Write-Host '================ guest log (key) ================'
Get-Content "$lg\hostcall_guest.log" | Select-String -Pattern 'pj_sip_dual|media|PASSED|FAILED|empty|normal|missing|state ->|dialing|transport up|rewritten|audio:|mic:|IRQ' | Select-Object -First 60
Write-Host '================ pjsua log (key) ================'
Get-Content "$lg\pjsua_host.log" | Select-String -Pattern 'INVITE|SIP/2.0|200|OK|RTP|Call|Incoming|Answer|rx |tx |5000|5060|transport|listening|bound' | Select-Object -First 50
