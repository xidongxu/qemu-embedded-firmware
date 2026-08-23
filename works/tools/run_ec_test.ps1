# run_ec_test.ps1 - Echo-cancellation (EC) verification with a synthetic echo.
#
# How it works (fully automatic, no real sound card needed):
#   * host pjsua plays a 1 kHz tone over RTP  ->  guest decodes and plays it.
#     That played tone is the EC *reference*.
#   * QEMU's mpsx mic reads echo_1k_8k_30s.wav  (a delayed+attenuated copy of
#     the same 1 kHz tone), simulating the speaker leaking back into the mic.
#   * guest EC should cancel the 1 kHz echo out of its capture, so the RTP it
#     sends back has almost no 1 kHz.  The host records that stream and we
#     measure how much 1 kHz survived.
#
# Run twice for the A/B comparison:
#   EC on : run_ec_test.ps1 -Build build-phone
#   EC off: run_ec_test.ps1 -Build build-phone-ecoff   (PJ_PHONE_EC_TAIL_MS=0)
#
param(
    [string]$Build   = 'build-phone',
    [int]$SipPort    = 5060,
    [int]$RtpPort    = 5000,
    [int]$WaitSec    = 30,
    [string]$Answer  = '200',
    [int]$UseTimer   = 0,
    [string]$EchoWav = 'echo_1k_8k_30s.wav'
)

$root  = 'C:\Users\xidon\code\github\qemu-embedded-firmware'
$tc    = 'C:\Users\xidon\code\github\qemu-embedded-platform\testcase'
$q     = 'C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe'
$pjsua = 'C:\Users\xidon\program\pjproject-2.17\build-win64\pjsip-apps\Release\pjsua.exe'
$py    = 'C:\Users\xidon\program\Python\Python314\python.exe'
$elf   = "$root\$Build\boards\mps2-an505\FreeRTOS\an505-qemu.elf"
$lg    = "$root\works\logs"

if (-not (Test-Path $elf)) { Write-Error "ELF not found: $elf (build it first)"; exit 1 }
if (-not (Test-Path "$tc\$EchoWav")) { Write-Error "echo wav not found: $tc\$EchoWav"; exit 1 }

New-Item -ItemType Directory -Force -Path $lg | Out-Null
$rec = "$lg\ec_remote_$(Split-Path $Build -Leaf).wav"
Remove-Item "$lg\ec_pjsua.log","$lg\ec_pjsua.err","$lg\ec_guest.log","$lg\ec_guest.err",$rec -Force -ErrorAction SilentlyContinue

# 1) host pjsua UAS: null audio (no host sound card!), plays the 1 kHz tone to
#    the guest over RTP (EC reference) and records what the guest sends back.
$hostArgs = @(
    "--id=sip:phone@127.0.0.1:$SipPort",
    "--local-port=$SipPort",
    "--rtp-port=$RtpPort",
    "--contact=sip:phone@10.0.2.2:$SipPort",
    "--auto-answer=$Answer",
    "--use-timer=$UseTimer",
    "--null-audio",
    "--play-file=$tc\sine_1k_8k_30s.wav",
    "--auto-play",
    "--auto-loop",
    "--rec-file=$rec",
    "--auto-rec",
    "--jb-max-size=120",
    "--log-level=5",
    "--app-log-level=5",
    "--duration=60"
)
Start-Process -FilePath $pjsua -ArgumentList $hostArgs -RedirectStandardOutput "$lg\ec_pjsua.log" -RedirectStandardError "$lg\ec_pjsua.err" -NoNewWindow -PassThru | Out-Null
Start-Sleep -Seconds 8
Write-Host "pjsua UAS up; guest=EC build [$Build], echo=$EchoWav"

# 2) QEMU guest: playback goes to a wav backend (so the guest play path is
#    actually driven -> the echo canceller gets a live reference), mic input
#    is the synthetic echo wav (delayed/attenuated copy of the played tone).
$ga = @(
    '-machine','mps2-an505,audiodev=a0',
    '-audiodev',"wav,id=a0,path=$lg\ec_playback_$(Split-Path $Build -Leaf).wav,out.frequency=8000,out.channels=1",
    '-cpu','cortex-m33',
    '-m','16M',
    '-display','none',
    '-serial','stdio',
    '-nic',"user,id=n0,model=lan9118,mac=52:54:00:12:34:01,hostfwd=udp::15062-:15062,hostfwd=udp::4000-:4000,hostfwd=udp::4001-:4001",
    '-global',"mpsx-simple-mic.infile=$tc\$EchoWav",
    '-kernel',$elf
)
Start-Process -FilePath $q -ArgumentList $ga -RedirectStandardOutput "$lg\ec_guest.log" -RedirectStandardError "$lg\ec_guest.err" -NoNewWindow -PassThru | Out-Null
Write-Host "guest started, waiting ${WaitSec}s..."
Start-Sleep -Seconds $WaitSec

# 3) stop
Get-Process qemu-system-arm,pjsua -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

# 4) report: call state + EC creation + 1 kHz energy in the recorded stream
Write-Host ""
Write-Host '================ guest log (key) ================'
Get-Content "$lg\ec_guest.log" | Select-String -Pattern 'pj_phone|Echo|echo|call .*state|conf connected|make_call|media' | Select-Object -First 40
Write-Host '================ 1 kHz analysis (remote rec) ================'
if (Test-Path $rec) {
    & $py "$root\works\tools\analyze_audio.py" $rec --freq 1000 --bw 40
} else {
    Write-Host 'recording missing - host may not have started media'
}
Write-Host ""
Write-Host "recording: $rec"
