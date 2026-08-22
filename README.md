# qemu embedded firmware

## toolchain
- Arm GNU Toolchain 15.3.Rel1 (Build arm-15.149)

## board 
- mps2-an505

## build
```shell
cmake -S . -B build -G Ninja -DBOARD=mps2-an505 -DPROJECT=FreeRTOS -DCMAKE_TOOLCHAIN_FILE="${PWD}/cmake/arm-none-eabi-gcc.cmake"
cmake --build build
```

## tests

Board smoke tests (FreeRTOS `main_task_entry`): LCD/touch, then audio and mic.

- **audio** (`Core/Src/audio.c`): renders a C-major arpeggio into a RAM buffer
  and lets the QEMU `mpsx-simple-audio` device loop it. The DONE interrupt
  (NVIC IRQ 49) is enabled via `audio_irq_enable()`; `audio_wait_done()` waits
  on the interrupt flag with a STATUS-poll fallback. Feed a host WAV via
  `-audiodev wav,path=out.wav,id=audio0 -machine mps2-an505,audiodev=audio0`.
- **mic** (`Core/Src/mic.c`): captures PCM from the QEMU `mpsx-simple-mic`
  device. The DONE interrupt (NVIC IRQ 50) drives `mic_capture()` (with a
  poll fallback). `mic_test()` verifies the captured signal level. Feed it a
  WAV: `-global mpsx-simple-mic.infile=<8k.wav>`.

Note: only the FreeRTOS startup wires IRQ slots 49/50; BareMetal/threadx keep
them zero, so those projects must not call `audio_irq_enable()` and use the
polled paths.

### Dual-QEMU SIP call (`pj_sip_dual_test.c`)

Two QEMU instances (mps2-an505) place a SIP call to each other over slirp
(user-mode networking + `hostfwd`). Roles are selected at build time with
`-DPJ_DUAL_ROLE=caller|callee`; build both ELFs into
`testcase/an505-caller.elf` / `an505-callee.elf`, launch both QEMUs and check
both serial logs for `media ALL PASSED`.

Run script: `works/tools/run_dual_slirp.ps1` (10 s call, WAV-fed mics)
Analyze output: `works/tools/analyze_audio_deep.py` / `analyze_call_audio.py`

Key fix: `frm_per_pkt=1` (10 ms packets) must match the 10 ms media clock;
G.711's default 2 frames/pkt (20 ms) made the jitter buffer drain 2x too fast
(empty/silence). With only ~1% scattered loss the default PLC (240 ms,
`PJMEDIA_MAX_PLC_DURATION_MSEC`) is sufficient, so no pjproject source change
is needed. Full root-cause analysis: `works/logs/WORKLOG-2026-08-20-netdev-socket.md`.
