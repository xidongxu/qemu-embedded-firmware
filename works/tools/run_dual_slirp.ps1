# run_dual_slirp.ps1 - dual-QEMU SIP call over QEMU 'user' netdev (slirp NAT)
# with hostfwd port forwards.  Each instance is its OWN NAT (guest 10.0.2.15,
# gateway 10.0.2.2); the peers reach each other through host ports:
#   callee hostfwd: udp::15062-:15062 (SIP), udp::4002-:4002 (RTP),
#                   udp::4003-:4003 (RTCP), udp::20013-:20003 (media SYNC)
#   caller hostfwd: udp::16062-:15062 (SIP), udp::4000-:4000 (RTP),
#                   udp::4001-:4001 (RTCP), udp::20003-:20003 (media SYNC)
# callee starts first (waits for INVITE), then caller (dials).
# Long-call variant (2026-08-22): 10 s media (1000 frames), 10 s WAV sources.
# Usage:  powershell -ExecutionPolicy Bypass -File works\tools\run_dual_slirp.ps1 [-Runs n]
param([int]$Runs = 1, [int]$Wait = 60)

$tc = 'C:\Users\xidon\code\github\qemu-embedded-platform\testcase'
$q  = 'C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe'

# callee (UAS): waits for INVITE, MAC :01
$cb = @('-machine','mps2-an505,audiodev=b0',
    '-audiodev',"wav,path=$tc\out_callee.wav,id=b0",
    '-cpu','cortex-m33','-m','16M','-display','none','-serial','stdio',
    '-nic','user,id=n0,model=lan9118,mac=52:54:00:12:34:01,hostfwd=udp::15062-:15062,hostfwd=udp::4002-:4002,hostfwd=udp::4003-:4003,hostfwd=udp::20013-:20003',
    '-global',"mpsx-simple-mic.infile=$tc\sine_440_8k_10s.wav",
    '-kernel',"$tc\an505-callee.elf")

# caller (UAC): dials, MAC :02
$ca = @('-machine','mps2-an505,audiodev=a0',
    '-audiodev',"wav,path=$tc\out_caller.wav,id=a0",
    '-cpu','cortex-m33','-m','16M','-display','none','-serial','stdio',
    '-nic','user,id=n0,model=lan9118,mac=52:54:00:12:34:02,hostfwd=udp::16062-:15062,hostfwd=udp::4000-:4000,hostfwd=udp::4001-:4001,hostfwd=udp::20003-:20003',
    '-global',"mpsx-simple-mic.infile=$tc\sine_1k_8k_10s.wav",
    '-kernel',"$tc\an505-caller.elf")

for ($r = 1; $r -le $Runs; $r++) {
    Write-Host "===== RUN $r (slirp user-net + hostfwd) ====="
    Remove-Item "$tc\caller.log","$tc\callee.log","$tc\out_caller.wav","$tc\out_callee.wav" -Force -ErrorAction SilentlyContinue
    Start-Process -FilePath $q -ArgumentList $cb -RedirectStandardOutput "$tc\callee.log" -RedirectStandardError "$tc\callee.err" -NoNewWindow -PassThru | Out-Null
    Start-Sleep -Seconds 4
    Start-Process -FilePath $q -ArgumentList $ca -RedirectStandardOutput "$tc\caller.log" -RedirectStandardError "$tc\caller.err" -NoNewWindow -PassThru | Out-Null
    Start-Sleep -Seconds $Wait
    Get-Process qemu-system-arm -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep 1
    Write-Host '-- callee --'
    Get-Content "$tc\callee.log" -ErrorAction SilentlyContinue | Select-String -Pattern 'lwIP-OS up|INVITE|sync done|jbuf size|rx  rtcp|play |rtcp tx|dtmf |media ' | Select-Object -Last 6
    Write-Host '-- caller --'
    Get-Content "$tc\caller.log" -ErrorAction SilentlyContinue | Select-String -Pattern 'lwIP-OS up|INVITE|sync done|jbuf size|rx  rtcp|play |rtcp tx|dtmf |media ' | Select-Object -Last 6
    python C:\Users\xidon\code\github\qemu-embedded-firmware\works\tools\analyze_call_audio.py "$tc\out_callee.wav" 2>&1 | Select-Object -First 1
    python C:\Users\xidon\code\github\qemu-embedded-firmware\works\tools\analyze_call_audio.py "$tc\out_caller.wav" 2>&1 | Select-Object -First 1
}
