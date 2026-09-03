# WORKLOG 2026-09-03 — tracer: 分级运行日志接口（leveled runtime log）

## 需求（用户原话）
"当前这个 tracer 库可以添加日志相关的接口么？我想实现只要应用这个库就可以实现日志打印。同时，如果运行时
出现问题，使用 tracer 打印日志还有一个好处就是可以将 tracer 的日志缓冲区存储起来，不用刻意调用 tracer 的
日志打印接口来存日志。另外，如果需要添加日志打印接口，需要预留异步日志存储接口，由用户自行对接将缓冲区的
日志写文件还是写 flash，如果用户不对接异步日志接口，则使用同步日志的方式输出。"

## 设计决策（vscode_askQuestions 收集 3 轮，最终定稿）
| 决策点 | 最终选择 |
|---|---|
| API 形态 | **仅带 level 的统一函数** `tracer_log(level, fmt, ...)`（无独立无 level 入口；3 轮内用户从"无 level 的 tracer_logf"改为"带分级"，再改为"仅 level 统一函数"） |
| 过滤时机 | **运行期开关**（全编译进固件，`tracer_log_set/get_level()` 随时调；默认 `TRACER_LOG_DEFAULT_LEVEL`=INFO） |
| 缓冲关系 | **合一**（复用崩溃 ring `s_ring`；崩溃 record 自动含最近日志，无需刻意存） |
| 异步存储 | **weak 回调 `tracer_log_sink` + drain 导出 `tracer_log_drain` 都留**（两选一或都用；都不接=纯同步串口输出） |
| 实现位置 | **并入 tracer.c / tracer.h**（用户偏好少文件，不新增库文件） |

## 实现要点
- 行格式：`[<ms> ms]X: <body>\r\n`（`<ms>` 复用 weak `tracer_uptime_ms()`；`X`=`T/D/I/W/E`）。每行在
  **调用者栈上**格式化（`TRACER_LOG_LINE_SIZE` 默认 160，超长截断仍保 CRLF）→ 可重入、可 ISR 调用。
- 一次 PRIMASK 临界区完成：① 同步串口输出整行（默认"同步日志"）；② 写入共享崩溃 ring（与
  `tracer_ring_printf` 同缓冲、同 `s_ring_total` 虚拟流）；出临界区后 ③ weak `tracer_log_sink(line,len)` 回调。
- ring 新增 `s_ring_total`（虚拟流游标）；`tracer_log_drain()` 用独立读游标 `s_log_drain_at` 做增量导出，
  消费过慢时跳过已被覆盖的最旧字节。
- weak `tracer_log_sink` 默认 no-op（IAR `#pragma weak` + GCC 属性，复用 `TRACER_CRASH_WEAK` 写法）。

## 坑（实测）
1. **mini-printf 的 sink 全部传 `ctx=NULL`**（`tracer_xputc`/`tracer_ring_prefix` 硬编码 `s_emit(NULL,c)`），
   最初的"经 ctx 写行缓冲"方案会 NULL 解引用 → 改用静态 `s_log_buf` 指针携带目标缓冲（仅在 PRIMASK 临界区
   内触碰，ISR 无法打断污染）。
2. **ARM 上 enum 底层可能是 unsigned** → `level >= TRACER_LOG_TRACE(0)` 恒真触发 `-Wtype-limits`；
   范围检查改 `(int)` 转换消除。
3. weak 覆盖**无法在同一翻译单元测试**（`#include tracer.c` 的单测与 weak 定义同 TU 会重复定义）→ sink 覆盖
   用"独立编译 tracer.c 目标 + 测试强符号覆盖"的第二个可执行文件测。

## 新增/修改文件
- `tracer.h`：`tracer_log_level_t` 枚举 + `tracer_log/set_level/get_level/drain` 声明 + weak
  `tracer_log_sink` + `TRACER_LOG_DEFAULT_LEVEL`/`TRACER_LOG_LINE_SIZE` 宏（全在 crashlog 块内）。
- `tracer.c`：`s_ring_total`、`s_log_buf`/`s_log_drain_at`/`s_log_level`、`tracer_log_buf_char`、
  `tracer_log/set_level/get_level`、weak `tracer_log_sink`、`tracer_log_drain`（CRASH/LOG 各自门控区）。
- `tests/test_tracer_log.c`（新）：默认级别过滤、运行期升降级、级别字母与 `[ms]` 前缀、合一 ring、截断、
  drain 增量/覆盖跳过、崩溃 record 自动含最近日志。
- `tests/test_tracer_log_sink.c`（新）：weak sink 强覆盖（独立 TU）、过滤行不回调、drain 连续流校验。
- `tests/CMakeLists.txt` + `.github/workflows/tracer-ci.yml`：接入两个 host 测试。
- `tracer/README.md`：新增"分级运行日志"小节（含 sink/drain 用法示例）。

## 更新（同日）：开关拆分 —— `TRACER_USE_CRASH` + `TRACER_USE_LOG`
用户定：黑匣子与日志**拆成两个独立宏**且可单独开；黑匣子宏彻底改名 `TRACER_USE_CRASH`（无兼容别名）；两板
（mps2/stm32 FreeRTOS）都开 CRASH+LOG。
- **新开关**：`TRACER_USE_CRASH`（黑匣子：`tracer_ring_printf` 事件 + dump capture + `tracer_crash_save`/
  store）；`TRACER_USE_LOG`（`tracer_log`/级别/宏组/sink/drain）。两者独立，默认 0。
- **公共件**（mini-printf / PRIMASK / 共享 ring / uptime）改在 `TRACER_USE_CRASH || TRACER_USE_LOG` 下编译；
  旧 `TRACER_USE_CRASHLOG` 与 `TRACER_CRASHLOG_*` 尺寸宏全仓清除——ring 更名 `TRACER_RING_SIZE`（公共），
  捕获缓冲 `TRACER_CRASH_SIZE`（crash）。
- **坑**：log 的 weak sink 原本复用 crash 块里的 `TRACER_CRASH_WEAK` 宏，CRASH off 时未定义 → 改用 LOG 块
  内独立 `TRACER_LOG_WEAK`（IAR pragma / GCC attribute）。
- **组合语义**：LOG-only = 串口日志 + sink/drain（ring 作 drain 源，无崩溃）；CRASH-only = 预崩溃事件 +
  record 落存储（无 `tracer_log`）；both = 合一（日志进同一 ring，record 含最近日志）。
- **测试组合**：test_crashlog=CRASH-only、test_tracer_log=both（含合一断言）、test_tracer_log_sink=
  LOG-only（独立 TU，证明 LOG 可脱离 CRASH 编译运行）、crash_store=CRASH。CI 的 ARM fsyntax 扩到三组合
  （both / LOG-only / CRASH-only）。
- **板级**：mps2 FreeRTOS CMake（tracer + app 目标）与 stm32 FreeRTOS Makefile（C_DEFS）均改
  `-DTRACER_USE_CRASH=1 -DTRACER_USE_LOG=1`。README 配置表/两节标题与说明同步（含新旧宏对照）。
- **验证**：host 4 组合过、ARM fsyntax 三组合零警告、mps2 固件链接 clean（rc=0）。

## 验证
- host 单测 6/6 通过（test_tracer / miniprint / crashlog / crash_store 回归 + 两个新日志测试），本机 zig cc。
- ARM fsyntax（cortex-m33，`TRACER_USE_CRASH`/`TRACER_USE_LOG` 三组合）零警告。
- 板级固件完整重链接通过（mps2-an505 FreeRTOS `an505-qemu.elf`，CRASH+LOG on）。
- 待办（可选，另开任务）：把 `pj_phone.c` 的状态机事件从 `tracer_ring_printf` 视需要迁移/新增
  `tracer_log(...)` 到应用侧做"运行日志上屏 + 异步落 flash"演示；stm32 板同样可接入。

## 更新（同日，第二轮）：流式输出 + 块式 sink —— 去掉行上限
用户定：**日志要流式**（"哪有日志输出还要自己分行的？"），sink 改**块回调**（"输出满 128 字节回调 sink 一
次，输出结束时若不满 128 字节调用 flush 输出缓冲区中所有数据"）。
- **删除 `TRACER_LOG_LINE_SIZE`**（原 160 行缓冲/截断）→ 引入 `TRACER_LOG_SINK_CHUNK_SIZE`（默认 128，
  纯 sink 推送块，不限制单次日志长度）。
- `tracer_log()` 改**逐字符流式**（无行长上限）：一次 PRIMASK 临界区内 ① 每字符同步串口 ② 每字符进共享
  ring ③ 每累计 128B 调用一次 weak `tracer_log_sink(chunk,len)`，`tracer_log()` 结束时把残余（<128B）再
  flush 一次；`s_log_chunk[128]` 单全局缓冲（临界区内独占），移除旧 `s_log_buf`/`tracer_log_buf_char`。
- sink 语义：块可能**横跨行边界**（一条 >128B 日志会分成多块回调），接收方只需 append；sink 在 PRIMASK
  临界区内被调 → 必须轻量（拷贝/入队），勿阻塞写 flash。
- `tracer_log()` 返回值 = 本次流出的字节数（0=被运行期级别过滤），与 drain/串口一致。
- 测试更新：`test_tracer_log.c` 第 5 节截断测试改为 **320B 长记录完整流出**（串口全量 + ring 每字节、旧字节
  卷走）；`test_tracer_log_sink.c` 改为**累积块**校验——短行 1 次 sink 调用、长行分 `ceil(n/128)` 块、每块
  ≤128、末尾残余 = n%128、重组字节与流式记录逐字节一致。
- `main.c`（mps2 试点）`tracer_dump_tasks()` 简化：原来为 160B 上限按 `\n` 手动拆行逐条 `TRACER_LOGI` →
  现在直接 `TRACER_LOGI("%s", buf)` 整表一条（0-printf 策略不变）。全文件已无 `printf`。
- **验证**：host 日志两测 + 其余 4 回归全过；ARM fsyntax 三组合零警告；mps2 FreeRTOS 重链 rc=0；QEMU 启动
  clean（`[0 ms] I: Start`，pjsua 进媒体）。
- **坑**：zig cc 编译 `..\tracer.c` 这类**相对父目录源码**会报 `CacheCheckFailed`（zig 缓存缺陷）→ host 测试
  命令一律用绝对路径输入源文件。
