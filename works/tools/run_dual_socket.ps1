# run_dual_socket.ps1 - dual-QEMU SIP call over the QEMU 'socket' netdev
# (point-to-point, NO slirp, NO hostfwd).  The two guests sit on
# 10.0.2.0/24 (caller .15, callee .16) and ARP each other directly.
# callee must start first (listens), then caller (connects).
# Usage:  powershell -ExecutionPolicy Bypass -File works\tools\run_dual_socket.ps1 [-Runs n]
#
# *** OBSOLETE (2026-08-22) ***: the firmware was reverted to the slirp/user-net
# hostfwd topology - A/B proved slirp was never the loss cause (3 test-side fixes:
# media handshake + ptime + VAD make it 200/200 loss-free too).  Use
# run_dual_slirp.ps1 instead.  Kept only as a reference for the socket topology.
param([int]$Runs = 1, [int]$Wait = 45)

$tc = 'C:\Users\xidon\code\github\qemu-embedded-platform\testcase'
$q  = 'C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe'

# callee (UAS): listens on 127.0.0.1:20000, static IP .16, MAC :01
$cb = @('-machine','mps2-an505,audiodev=b0',
    '-audiodev',"wav,path=$tc\out_callee.wav,id=b0",
    '-cpu','cortex-m33','-m','16M','-display','none','-serial','stdio',
    '-nic','socket,id=n0,listen=127.0.0.1:20000,model=lan9118,mac=52:54:00:12:34:01',
    '-global',"mpsx-simple-mic.infile=$tc\sine_440_8k.wav",
    '-kernel',"$tc\an505-callee.elf")

# caller (UAC): connects to 127.0.0.1:20000, static IP .15, MAC :02
$ca = @('-machine','mps2-an505,audiodev=a0',
    '-audiodev',"wav,path=$tc\out_caller.wav,id=a0",
    '-cpu','cortex-m33','-m','16M','-display','none','-serial','stdio',
    '-nic','socket,id=n0,connect=127.0.0.1:20000,model=lan9118,mac=52:54:00:12:34:02',
    '-global',"mpsx-simple-mic.infile=$tc\sine_1k_8k.wav",
    '-kernel',"$tc\an505-caller.elf")

for ($r = 1; $r -le $Runs; $r++) {
    Write-Host "===== RUN $r (socket netdev) ====="
    Remove-Item "$tc\caller.log","$tc\callee.log","$tc\out_caller.wav","$tc\out_callee.wav" -Force -ErrorAction SilentlyContinue
    Start-Process -FilePath $q -ArgumentList $cb -RedirectStandardOutput "$tc\callee.log" -RedirectStandardError "$tc\callee.err" -NoNewWindow -PassThru | Out-Null
    Start-Sleep -Seconds 4
    Start-Process -FilePath $q -ArgumentList $ca -RedirectStandardOutput "$tc\caller.log" -RedirectStandardError "$tc\caller.err" -NoNewWindow -PassThru | Out-Null
    Start-Sleep -Seconds $Wait
    Get-Process qemu-system-arm -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep 1
    Write-Host '-- callee --'
    Get-Content "$tc\callee.log" -ErrorAction SilentlyContinue | Select-String -Pattern 'lwIP-OS up|INVITE|jbuf size|rx  rtcp|play |rtcp tx|dtmf |media ' | Select-Object -Last 6
    Write-Host '-- caller --'
    Get-Content "$tc\caller.log" -ErrorAction SilentlyContinue | Select-String -Pattern 'lwIP-OS up|INVITE|jbuf size|rx  rtcp|play |rtcp tx|dtmf |media ' | Select-Object -Last 6
    python C:\Users\xidon\code\github\qemu-embedded-firmware\works\tools\analyze_call_audio.py "$tc\out_callee.wav" 2>&1 | Select-Object -First 1
    python C:\Users\xidon\code\github\qemu-embedded-firmware\works\tools\analyze_call_audio.py "$tc\out_caller.wav" 2>&1 | Select-Object -First 1
}
