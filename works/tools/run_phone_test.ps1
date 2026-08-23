# run_phone_test.ps1 - PJSUA high-level phone app: guest (PJ_PHONE) dials
# a host-side pjsua UAS through slirp + hostfwd, then reports key log lines.
#
# Topology:
#   guest PJSUA  : SIP bind 0.0.0.0:15062 ; dials sip:user@10.0.2.2:<SipPort>
#   hostfwd      : udp::15062-:15062
#   host pjsua   : --local-port=<SipPort> --rtp-port=<RtpPort> --auto-answer=200
param(
    [int]$SipPort  = 5060,
    [int]$RtpPort  = 5000,
    [int]$WaitSec  = 40,
    [string]$Answer = '200',
    [int]$UseTimer = 1,
    [string]$IpAddr = '',
    [string]$AudioRec = '',
    [string]$HostRec = '',
    [switch]$RealAudio
)

$root = 'C:\Users\xidon\code\github\qemu-embedded-firmware'
$tc   = 'C:\Users\xidon\code\github\qemu-embedded-platform\testcase'
$q    = 'C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe'
$pjsua= 'C:\Users\xidon\program\pjproject-2.17\build-win64\pjsip-apps\Release\pjsua.exe'
$elf  = "$root\build-phone\boards\mps2-an505\FreeRTOS\an505-qemu.elf"
$lg   = "$root\works\logs"

New-Item -ItemType Directory -Force -Path $lg | Out-Null
Remove-Item "$lg\phone_pjsua.log","$lg\phone_pjsua.err","$lg\phone_guest.log","$lg\phone_guest.err" -Force -ErrorAction SilentlyContinue

# 1) host-side pjsua UAS
# NOTE: --auto-loop gives continuous bidirectional RTP but makes the host
# jitter buffer empty ~900 times per run (auto-loop processing skews host
# RX); --auto-play is the clean A1 baseline (host jitter-empty=0, but host
# only transmits while the 10s WAV plays).
if ($RealAudio) {
    # Real audio on the GUEST side only: the user talks into / listens to the
    # QEMU sound card (dsound = mpsx mic in + audio out).  The host pjsua is
    # the REMOTE end and stays on --null-audio + --auto-play (plays a WAV).
    # CRITICAL: do NOT give the host a real sound card in the same room --
    # the host speaker (playing guest audio) leaks into the QEMU microphone
    # (dsound), which the guest echo suppressor cannot cancel (it only knows
    # its own speaker) -> cross-device acoustic howl/feedback.
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
        # 30s tone so a 30s run has no host loop boundary (the old 10s WAV
        # with --auto-loop produced an RTP glitch every 10s -> guest
        # underflow burst -> faster crackle near each 10s boundary).
        "--auto-loop",
        "--jb-max-size=120",
        "--log-level=5",
        "--app-log-level=5",
        "--duration=60"
    )
} else {
    $hostArgs = @(
        "--id=sip:phone@127.0.0.1:$SipPort",
        "--local-port=$SipPort",
        "--rtp-port=$RtpPort",
        "--contact=sip:phone@10.0.2.2:$SipPort",
        "--auto-answer=$Answer",
        "--use-timer=$UseTimer",
        "--null-audio",
        "--play-file=$tc\sine_1k_8k_10s.wav",
        "--auto-play",
        "--jb-max-size=120",
        "--log-level=5",
        "--app-log-level=5",
        "--duration=60"
    )
}
if ($HostRec -ne '') {
    # Record the remote (guest) audio on the host: proves the guest's mic
    # capture actually arrives as RTP.  --auto-rec starts recording the
    # remote stream automatically once media is up.
    $hostArgs += "--rec-file=$HostRec"
    $hostArgs += "--auto-rec"
    Write-Host "host recording remote audio to $HostRec"
}
if ($IpAddr -ne '') {
    $hostArgs += "--ip-addr=$IpAddr"
}
Start-Process -FilePath $pjsua -ArgumentList $hostArgs -RedirectStandardOutput "$lg\phone_pjsua.log" -RedirectStandardError "$lg\phone_pjsua.err" -NoNewWindow -PassThru | Out-Null
# Give pjsua time to fully start (UDP listener on $SipPort) before the guest.
Start-Sleep -Seconds 8
Write-Host "pjsua UAS started (127.0.0.1:$SipPort rtp=$RtpPort answer=$Answer use-timer=$UseTimer ipaddr=$IpAddr)"

# 2) QEMU guest (PJ_PHONE high-level app)
if ($RealAudio) {
    # Real audio on the GUEST side only: the user talks into / listens to the
    # QEMU sound card (dsound = mpsx mic in + audio out).  The host pjsua is
    # the REMOTE end and stays on --null-audio + --auto-play (plays a WAV).
    $ga = @(
        '-machine','mps2-an505,audiodev=a0',
        '-accel','tcg,thread=multi',
        # 8k keeps full media quality (4k did not reduce the residual
        # conf-bridge underflow crackle, only lowered quality).
        '-audiodev','dsound,id=a0,out.frequency=8000,out.channels=1',
        '-cpu','cortex-m33',
        '-m','16M',
        '-display','none',
        '-serial','stdio',
        '-nic',"user,id=n0,model=lan9118,mac=52:54:00:12:34:01,hostfwd=udp::15062-:15062,hostfwd=udp::4000-:4000,hostfwd=udp::4001-:4001",
        '-kernel',$elf
    )
    Write-Host 'real audio: guest dsound (user side), host null+WAV (remote)'
} else {
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
    # Optional: record what the guest plays (mpsx-simple-audio) to a wav, so
    # we can verify the host->guest RTP actually makes it to the guest sound
    # card.
    if ($AudioRec -ne '') {
        $ga = @(
            '-machine','mps2-an505,audiodev=a0',
            '-audiodev',"wav,path=$AudioRec,id=a0",
            '-cpu','cortex-m33',
            '-m','16M',
            '-display','none',
            '-serial','stdio',
            '-nic',"user,id=n0,model=lan9118,mac=52:54:00:12:34:01,hostfwd=udp::15062-:15062,hostfwd=udp::4000-:4000,hostfwd=udp::4001-:4001",
            '-global',"mpsx-simple-mic.infile=$tc\sine_1k_8k_10s.wav",
            '-kernel',$elf
        )
        Write-Host "recording guest playback to $AudioRec"
    }
}
Start-Process -FilePath $q -ArgumentList $ga -RedirectStandardOutput "$lg\phone_guest.log" -RedirectStandardError "$lg\phone_guest.err" -NoNewWindow -PassThru | Out-Null
Write-Host "guest started, waiting ${WaitSec}s..."
Start-Sleep -Seconds $WaitSec

# 3) stop both
Get-Process qemu-system-arm,pjsua -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

# 4) report
Write-Host ""
Write-Host '================ guest log (key) ================'
Get-Content "$lg\phone_guest.log" | Select-String -Pattern 'pj_phone|PJSUA|pjsua_|make_call|call .* state|reg state|account|transport up|acc |media_status|FAILED|status=|status |invalid|error|err' | Select-Object -First 80
Write-Host '================ pjsua log (key) ================'
Get-Content "$lg\phone_pjsua.log" | Select-String -Pattern 'INVITE|SIP/2.0|200|OK|Call|Incoming|Answer|RTP|rx |tx |5060|5000|transport|listening|bound|audio' | Select-Object -First 60
