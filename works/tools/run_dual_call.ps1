# Launch the dual-QEMU inter-instance SIP call (stage 11-12).
#
# Two patched QEMU mps2-an505 instances, both with guest IP 10.0.2.15, talk
# THROUGH the slirp host gateway 10.0.2.2 using UDP hostfwd port forwards:
#   caller (UAC): hostfwd udp::16062-:15062, udp::4000-:4000
#   callee (UAS): hostfwd udp::15062-:15062, udp::4002-:4002
#
# The caller's elf must be built with -DPJ_DUAL_ROLE=caller, the callee's
# with -DPJ_DUAL_ROLE=callee (see works/logs/WORKLOG-2026-08-17-pjsip-call.md).
#
# Outputs (in $TestCase):
#   callee.log / caller.log   - serial console (SIP + real-time media stats)
#   out_callee.wav            - what the callee hears (expect ~1001 Hz)
#   out_caller.wav            - what the caller hears (expect ~439 Hz)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File run_dual_call.ps1
param(
    [string]$BuildDir = 'C:\Users\xidon\code\github\qemu-embedded-firmware',
    [string]$TestCase  = 'C:\Users\xidon\code\github\qemu-embedded-platform\testcase',
    [string]$Qemu      = 'C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe',
    [string]$CallerWav = 'sine_1k_8k.wav',    # caller mic source (1 kHz)
    [string]$CalleeWav = 'sine_440_8k.wav',   # callee mic source (440 Hz)
    [int]$RunSeconds   = 50
)

$elfC = Join-Path $BuildDir 'build-callee\boards\mps2-an505\FreeRTOS\an505-qemu.elf'
$elfA = Join-Path $BuildDir 'build-caller\boards\mps2-an505\FreeRTOS\an505-qemu.elf'

if (-not (Test-Path $elfA) -or -not (Test-Path $elfC)) {
    Write-Error "elf not found. Build with -DPJ_DUAL_ROLE=caller / =callee first."
    exit 1
}

# Callee first: it must be listening before the caller dials.
$pB = Start-Process -FilePath $Qemu -ArgumentList @(
    '-machine', 'mps2-an505,audiodev=b0',
    '-audiodev', "wav,path=$TestCase\out_callee.wav,id=b0",
    '-cpu', 'cortex-m33', '-m', '16M', '-display', 'none', '-serial', 'stdio',
    '-nic', 'user,model=lan9118,hostfwd=udp::15062-:15062,hostfwd=udp::4002-:4002',
    '-global', "mpsx-simple-mic.infile=$TestCase\$CalleeWav",
    '-kernel', $elfC
) -RedirectStandardOutput "$TestCase\callee.log" -RedirectStandardError "$TestCase\callee.err" -NoNewWindow -PassThru

Start-Sleep -Seconds 4

# Caller second: dials the callee through the host gateway.
$pA = Start-Process -FilePath $Qemu -ArgumentList @(
    '-machine', 'mps2-an505,audiodev=a0',
    '-audiodev', "wav,path=$TestCase\out_caller.wav,id=a0",
    '-cpu', 'cortex-m33', '-m', '16M', '-display', 'none', '-serial', 'stdio',
    '-nic', 'user,model=lan9118,hostfwd=udp::16062-:15062,hostfwd=udp::4000-:4000',
    '-global', "mpsx-simple-mic.infile=$TestCase\$CallerWav",
    '-kernel', $elfA
) -RedirectStandardOutput "$TestCase\caller.log" -RedirectStandardError "$TestCase\caller.err" -NoNewWindow -PassThru

Write-Host "dual QEMU call running for $RunSeconds s..."
Start-Sleep -Seconds $RunSeconds

Stop-Process -Id $pA.Id, $pB.Id -Force -ErrorAction SilentlyContinue
Write-Host "done. See $TestCase\caller.log / callee.log and out_*.wav"
