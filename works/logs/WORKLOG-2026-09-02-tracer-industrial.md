# WORKLOG 2026-09-02 — tracer: 工业级增强（两轮）

## 轮一：代码评审 + 工业级增强 + 修 3 个硬 bug（已提交 76d30c98）
详见该提交 diff 与 repo memory `tracer.md`（2026-09-02 节）。要点：
- 修 3 个 QEMU 实测 bug：`tracer_load32` 未对齐读 Lockup（volatile 化）、EXC_RETURN bit4 极性反、
  FPU 上下文解码位置错（改 FPCAR + lazy-save 触发）。
- P0 重入保护 `s_tracer_dumping` + 汇编 `cpsid i`；`TRACER_FW_VERSION` / `TRACER_AUTO_RESET_MS`
  / `TRACER_ASSERT` / `tracer_dump_all()`；host 测试 + parser 测试 + CI + LICENSE + `tracer::tracer`。

## 轮二：剩余待办落地（⚠ 两次误删丢失，均完整重做并复验）
目标项：#1 crash-safe 输出、#2 看门狗、#3 栈溢出 hook、#5 M0 降级、#7 uptime、#8 finstrument；
#6 S16 捕获判死移除；#4 崩溃落 Flash 未做。

### 实现要点
- **crash-safe 输出（#1）**：`TRACER_PUTCHAR(ch)`。tracer.h 在 PUTCHAR 下
  `#define TRACER_PRINTF tracer_xprintf`（tracer.c 内无锁 mini-printf，支持 %s/%c/%d/%u/%lu/%x/%X
  + '-'/'0'/宽度），不 include stdio → 真正零 printf 依赖；未定义时回退 TRACER_PRINTF（现状）。
  51 处调用零改动。**坑**：曾用 #undef+别名转发 → 宏自递归被 CPP 抑制成隐式声明；改头文件按模式直接定义解决。
- **看门狗（#2）**：weak `tracer_watchdog_kick()`；`tracer_delay_ms`（AUTO_RESET 延时）每 ~64ms 喂；
  纯 trap 不喂（狗做最终兜底）。
- **栈溢出 hook（#3）**：mps2 `vApplicationStackOverflowHook` → `tracer_assert_fail(task, "hook", line)`。
- **uptime（#7）**：weak `tracer_uptime_ms()`，dump/assert/快照打 `Up: N ms`；mps2 FreeRTOS 覆盖
  `xTaskGetTickCount()*portTICK_PERIOD_MS`。
- **finstrument（#8）**：enter/exit 用 PRIMASK 临界区（`tracer_irq_save/restore`，no_instrument_function）。
- **M0 降级（#5）**：头文件/README 明确 "M0/M0+ 不支持（Thumb-1 无多寄存器 PUSH）"。
- **S16 判死（#6）**：QEMU 实测 M33 lazy stacking 下异常入口后 S16–S31 不保留 faulting 值
  （fault 前 S16live=40A00000，handler 内 VSTMIA 读到 0）→ 捕获无意义，已移除并注释原因。
- `Core/Inc/uart.h` 补 `put_char` 声明（原本定义了没声明；它是 PUTCHAR 模式的理想 sink）。

### 验证（重做后全部复验）
- 五配置编译零警告：M33 hard-float / M3 / host / miniprint host / PUTCHAR 模式；parser 单测通过。
- **PUTCHAR QEMU 实测**：BareMetal 临时 `-DTRACER_PUTCHAR=put_char` → dump 输出与 printf 模式逐字
  一致（FW/Up/寄存器/CFSR/Raw stack），验证后 `git checkout` 还原 CMake（注意 PowerShell 改写会丢
  UTF-8 BOM，用 git checkout 最干净）。
- FreeRTOS 重建 + QEMU 冒烟干净启动；BareMetal printf 模式 clean 重建 dump 正常（无 Lockup）。
- 新增 `tests/test_miniprint.c`（mini-printf 格式断言，本机无 host gcc 仅交叉编译 -c 验证，
  CI ubuntu 实跑）+ 挂 CTest/CI。

### 剩余待办
- [ ] 崩溃记录落 Flash + 复位原因（#4，工作量最大，用户决定"先考虑后面再说"）

### 备注
- ⚠ 教训（两次）：未提交改动两次被清空（HEAD 均停在 76d30c98）。长工作务必**及时提交**。
- 提交范围（本轮，已提交）：`libutils/tracer/{tracer.c,tracer.h,README.md,tests/CMakeLists.txt,
  tests/test_miniprint.c}`、`boards/mps2-an505/FreeRTOS/application/main.c`、
  `boards/mps2-an505/Core/Inc/uart.h`、`.github/workflows/tracer-ci.yml`、
  `works/logs/WORKLOG-2026-09-02-tracer-industrial.md`。

---

## 轮三：CMake / CI / 运行时验证收尾（2026-09-02，提交 809e556e + eff5ff00）

选做清单 #2/#3/#4/#5（#1 崩溃落 Flash 用户暂缓）。全部完成并 QEMU/CI 验证。

### #2 CMake 强制 C99 + install/export（提交 809e556e）
- `target_compile_options` 加 `-std=c99`（GNU/Clang/ARMClang），ARMCC 用 `--c99`，仅对 C 翻译单元。
- 新增 `install(TARGETS/EXPORT)`：GNUInstallDirs + CMakePackageConfigHelpers，
  产出 `tracer::tracer` imported target，`find_package(tracer CONFIG)` 可用。
- include 目录改 `BUILD_INTERFACE`（add_subdirectory 用户不变）/`INSTALL_INTERFACE`
  （`${CMAKE_INSTALL_INCLUDEDIR}/tracer`）——**不能写源树绝对路径**（install(EXPORT) 直接报错，
  已踩坑修复）。头文件装 `include/tracer/tracer.h`。
- 验证：两板工程 configure/build 干净；交叉 configure+install 冒烟测试（TRACER_EXIDX_TABLES=ON）
  生成 4 个 cmake 文件；最小 find_package 消费工程 resolves include 路径正确。

### #3 `-funwind-tables` 用 option 控制（提交 809e556e）
- 新 `option(TRACER_EXIDX_TABLES ... OFF)`：默认 OFF，tracer 库不再无条件发 `.ARM.exidx`（省 flash）。
- ⚠ 注意 configure 顺序：根 CMakeLists `add_subdirectory(libutils)`(44) **先于**
  `boards/`(45)，board 无法用 option 提前影响 → FreeRTOS board（用 TRACER_USE_EXIDX 精确回溯）
  在 `target_compile_definitions(tracer PRIVATE TRACER_USE_EXIDX=1)` 旁**显式补
  `target_compile_options(tracer PRIVATE -funwind-tables)`**（宏+flag 一起，无顺序依赖）。
- 验证：build-phone tracer.c 命令含 `-std=c99 -funwind-tables`（exidx 不回归）；
  build-baremetal 无 `-funwind-tables`。

### #4 CI 结果检查 → 修复自引入就红的 host 测试（提交 eff5ff00）
- 现象：tracer-ci 三个 commit 全 failure，失败在 "Build and run host unit tests"（链接阶段）。
- 根因：`tests/test_tracer.c`、`test_miniprint.c` include 整个 `tracer.c`，其非 static 函数引用
  ARM linker-script 符号 `&_estack`/`&_sstack`（`&_stext`/`&_etext`）→ host gcc 链接 undefined。
  （本机无 host gcc/WSL，下载便携 **zig** 用 `zig cc -target x86_64-windows-gnu` 复现并验证。）
- 修复：两个 test include 前钉死 `TRACER_STACK_BASE/TOP`（+miniprint 的 `TRACER_TEXT_START/END`）
  为无害常量（raw dump 已禁用，walker 测试显式传界）。
- 验证：本地 zig(host clang) 编/跑 test_tracer、test_miniprint 全 passed，test_parser.py passed；
  推送后 GitHub Actions **success**（CI 首次全绿）。

### #5 FreeRTOS 运行时触发 assert / stack-overflow（QEMU mps2-an505 实测，未提交，验证后还原）
- **#5a assert**：`main_task_entry` 首行 `TRACER_ASSERT(0)` → dump：
  `Tracer: Assert Failed / Up: 5 ms / Assert: 0 / End of assert (trapped)`（uptime override 生效）。
- **#5b stack-overflow**：深递归触发 FreeRTOS method-2 失败——**heap 分配的栈 pxStack 与 TCB
  在 heap 紧邻（TCB 低地址），bomb 穿栈底先覆盖 TCB 的 pxStack/pxTopOfStack 字段 → method2
  读损坏指针判 canary==0xa5 → 假阴性，hook 不触发**（SP 打印证明 bomb 已穿栈底 4KB）。
- 可靠触发法：main_task 改 `xTaskCreateStatic`（静态栈 .bss，TCB 分离）+ 直接写坏
  pxStack[0..3]（真实溢出的 canary 破坏机制）→ vTaskDelay 让出 → method2 触发 → dump：
  `Assert Failed / Up: 0 ms / Assert: main_task / At: vApplicationStackOverflowHook:309 /
  Call stack: 100BFBC4 100C006C`。
- 结论/记录：静态栈+canary 破坏是可靠测试法；heap 栈深溢出会砸 TCB 属 FreeRTOS method2 已知边界
  （真机可靠检测建议 `uxTaskGetStackHighWaterMark` 水位告警 或 xTaskCreateStatic 关键任务）。
- ⚠ 验证全程 `git checkout -- main.c` 还原，无测试残留提交。

---

## 轮四：崩溃黑匣子（crash log）落地 阶段1-3（2026-09-02，7263002e + 77472b23 + cbb822ba）

设计定稿：**两段式**——崩溃现场（系统不可信）只做防御性裸写 staging（不直接开文件系统：FS 状态/锁/栈在崩溃现场可能已坏）；
重启后（可信）boot 读回并归档成 littlefs 文件。内容**纯文本**（dump+预崩溃 ring+CRC），人可读、`tracer_parser.py` 可符号化、
无私有二进制 ABI。差异化核心：**崩溃前日志 + 现场 dump + 重启文件归档三合一**（单一"崩溃时开文件系统写"在崩溃现场不可靠，
故拆为"现场裸写 + 可信环境归档"两段）。

### 阶段1（7263002e）库侧 TRACER_USE_CRASHLOG=1
- 配置宏：`TRACER_USE_CRASHLOG`(0)/`TRACER_CRASHLOG_RING_SIZE`(2048)/`TRACER_CRASHLOG_CAP_SIZE`(8192)；默认构建零代码/零 RAM 变化。
- 输出层重构为 **sink 路由**（serial/ring/capture 三 sink，`s_emit` 指针切换）：mini-printf 抽 `tracer_xvprintf(fmt,va)` +
  `xprintf` 薄封装（test_miniprint 回归保行为）；PUTCHAR||CRASHLOG 才编 mini-printf，无 PUTCHAR 时 `putchar()` 回退（需 stdio）。
- `tracer_serial_char`：PUTCHAR/putchar + **capture 镜像**（s_cap_active 时同字符进 s_cap）。
- `tracer_ring_printf`：PRIMASK 临界（`tracer_pm_save/restore`，host/非 GCC no-op）+ 环形覆盖（s_ring_start/count）。
- `tracer_crash_save` weak no-op；fault handler/assert 接 `tracer_cap_begin()`（dump 前）与 `tracer_crash_finalize()`（trap 前：
  停镜像→附 ring 尾→CRC footer→save→打印 `[crashlog] record N bytes crc=...`）。
- **坑**：TRACER_WEAK 宏定义在 crashlog 块**之后** → crashlog 块用独立局部 weak 属性（IAR pragma / GCC attribute）；
  `crash_save` 放 mps2 FreeRTOS application 目录（非 Core 共享，BareMetal crashlog=0 不编）。
- host 单测 `tests/test_crashlog.c`（ring 覆盖/镜像/ring 尾+footer/ring 不泄漏串口）挂 CTest+CI；ARM 三配置 fsyntax 过。
- mps2 FreeRTOS CMake：tracer 目标 `-DTRACER_USE_CRASHLOG=1` + `-DTRACER_PUTCHAR=put_char;-include;../Core/Inc/uart.h`（C-only genex），
  app 目标 `-DTRACER_USE_CRASHLOG=1`（ring/save 声明可见）。QEMU assert 实测：capture 含 dump 文本+2 条 ring+CRC footer，crc 一致。

### 阶段2（77472b23）mps2 staging 双槽裸写 + boot 读回
- `crash_nv.c/h`（FreeRTOS application）：override `tracer_crash_save` → SPI NOR 顶部 0x0FFE0000 2×4K 槽（PJ_PHONE 构建
  不跑 fatfs/spi_flash，NOR 空闲无卷冲突）。槽布局 `[16B hdr: 'TNC1'|len|crc32][payload]`。
- 写序防断电：选非当前有效槽 → 擦 4K → 写 hdr → 写 payload；boot 仅 magic+CRC 双过才接受，半写槽忽略、另一槽旧记录仍可读。
- 惰性 `spi_flash_init()`（PJ_PHONE main 跳过 FS 初始化）。`crash_nv_read_latest`/`crash_nv_boot_report`（阶段2 先打印）。
- **坑**：本机 `QEMU-MACHINE` 标准 QEMU mps2-an505 **无 mtd flash**；必须用补丁版
  `C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe`（含 w25q02jvm）+ `-drive if=mtd,format=raw,file=<256M img>`。
- 验证：bootA assert → 写 NOR → 杀 QEMU（掉电）→ bootB 正常启动读回逐字一致 record（crc 相同）后继续 pjsua 全栈。

### 阶段3（cbb822ba）littlefs 归档 + consumed
- `crash_nv_boot_report` 升级：打印 → `crash_nv_archive_to_fs`（littlefs `crash_last.txt` + 滚动 `crash_prev.txt`）→ 成功才
  `crash_nv_clear`（擦当前有效槽）。下次 boot 静默（consumed）。
- littlefs 卷**限制在保留区之下**：`lfs_spi_flash_config_init` 后覆盖 `cfg.block_count = CRASH_NV_BASE/4096`（默认 whole-256MiB 会踩 crash 槽）。
  卷只在"有待归档记录"时 mount（干净 boot 零 FS 开销，PJ_PHONE 常态无文件系统负担）。
- 验证：bootC 归档 356B + clear；bootD 静默 consumed；主机查 img：record/ring/crc/文件名都在 littlefs 低地址卷内、staging 双槽头失效。

### 阶段4 破坏性验证（无代码变更）
- 主机篡改 img：槽1 半写（magic/len 对 + 错误 CRC + 垃圾 payload）+ 槽0 纯垃圾 → boot 静默、系统正常到 pjsua，不卡死（CRC/magic 校验正确忽略）。
- 完整链：崩溃→双槽裸写→掉电重启读回→littlefs 文件归档→consumed→抗损坏，全部 QEMU+主机双验证。

### 待办/注意
- stm32 后端（内部 Flash 末扇区 + `RCC->CSR` 复位源）未做（本机无真机/无法验证）。
- mps2 用补丁版 QEMU 才能验 flash 持久；标准版 mps2-an505 无 mtd。
- 崩溃前 ring 目前只有 main_task_entry 一条示例，产品化需在关键状态机（注册/通话/看门狗）处补 `tracer_ring_printf` 埋点。
- README 已补"崩溃黑匣子"章节；host 测试/CI 已含 crashlog。
