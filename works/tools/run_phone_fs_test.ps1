# run_phone_fs_test.ps1 - PJSUA phone (guest) against a HOST FreeSWITCH.
#
# Topology:
#   FreeSWITCH : internal profile  (192.168.23.7:5060, where the Android
#                                   phone 1005 registers)
#                internal-lo profile (127.0.0.1:5060, reached by the guest
#                                   as the fixed slirp gateway 10.0.2.2)
#   guest      : PJSUA registers extension 1000 via 10.0.2.2:5060
#                (internal-lo), then dials sip:1005@192.168.23.7:5060
#                (the phone's own home domain) so the dialplan rings the
#                actual phone instead of falling into voicemail.
#
# Requires FreeSWITCH to already be running with:
#   - fs_bind_all.ps1            (bind SIP/RTP on 0.0.0.0)
#   - internal-lo.xml profile    (fs_add_loopback_profile.ps1)
#   - the full 10.0.2.2 domain block in directory/default.xml
#     (fs_fix_loopback_domain.ps1)
#   - the Android phone 1005 registered (default profile @ 192.168.23.7)
#
# Expect on success:
#   pj_phone: acc 0 reg state=200 (OK)
#   pj_phone: make_call(sip:1005@192.168.23.7:5060) -> 0 (call=0)
#   pj_phone: call 0 state=3 (EARLY)      <- phone is ringing
#   pj_phone: call 0 state=5 (CONFIRMED)  <- remote answered
param(
    [int]$WaitSec = 40,
    [string]$Tag   = 'fs'
)

$root = 'C:\Users\xidon\code\github\qemu-embedded-firmware'
$tc   = 'C:\Users\xidon\code\github\qemu-embedded-platform\testcase'
$q    = 'C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe'
$elf  = "$root\build-phone\boards\mps2-an505\FreeRTOS\an505-qemu.elf"
$lg   = "$root\works\logs"

New-Item -ItemType Directory -Force -Path $lg | Out-Null
$guestLog = "$lg\phone_fs_${Tag}_guest.log"
$guestErr = "$lg\phone_fs_${Tag}_guest.err"
Remove-Item $guestLog, $guestErr -Force -ErrorAction SilentlyContinue

Write-Host "guest elf: $elf"
Write-Host "guest args:"
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
$ga | ForEach-Object { Write-Host "  $_" }

Start-Process -FilePath $q -ArgumentList $ga -RedirectStandardOutput $guestLog -RedirectStandardError $guestErr -NoNewWindow -PassThru | Out-Null
Write-Host "guest started, waiting ${WaitSec}s (FreeSWITCH must be running) ..."
Start-Sleep -Seconds $WaitSec

Get-Process qemu-system-arm -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Write-Host ""
Write-Host '================ guest log (key) ================'
Get-Content $guestLog | Select-String -Pattern 'pj_phone|PJSUA|pjsua_|make_call|call .* state|reg state|account|transport up|media_status|conf connected|wd: rx_pkt|status=|status |invalid|error|FAILED' | Select-Object -First 100
Write-Host '================ guest log (call-only) =========='
Get-Content $guestLog | Select-String -Pattern 'pj_phone: (acc|call|make_call)' | Select-Object -First 40
Write-Host ''
Write-Host "full log: $guestLog"
