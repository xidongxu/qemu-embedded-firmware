# WORKLOG-2026-08-30：TLSF 统一内存分配器集成

> **目标**：项目所有内存统一走 TLSF（Two-Level Segregated Fit）算法——C 库
> `malloc/free/calloc/realloc`、C++ `new/delete`、FreeRTOS 内核 `pvPortMalloc/pvPortFree`，
> 共享同一个 TLSF 池（O(1) 分配、低碎片、统一统计）。
> **库**：`libmem/tlsf`（Matthew Conte TLSF 3.1，纯算法核心 `tlsf.c/tlsf.h`）。

---

## 1. 方案

一个 TLSF 实例 + 三种接入路径，全部汇入同一个池：

| 接入点 | 机制 | 代码 |
|---|---|---|
| C 库 `malloc/free/calloc/realloc/memalign` | 链接器 `-Wl,--wrap=...` → `__wrap_*` | `tlsf_port.c` |
| C++ `new/delete`（含 array/nothrow/sized 形式） | 覆盖全局 `operator new/delete` | `tlsf_heap.cpp` |
| FreeRTOS 内核 `pvPortMalloc/pvPortFree` | `FREERTOS_HEAP` 指向自定义 heap 实现 | `tlsf_freertos.c` |

- **池**：链接脚本 heap 区 `[_end, _estack - _Min_Stack_Size)`（QEMU 16MB RAM 主堆），懒初始化。
- **锁**：`portENTER_CRITICAL()/EXIT`（关中断），TLSF O(1) 操作临界区极短；调度器启动前单线程也安全。
- **统计**：`tlsf_port_get_free/min_free/used_size`（walk pool），`xPortGetFreeHeapSize` 现返回 TLSF 剩余。

---

## 2. 文件清单

### 新增
| 文件 | 说明 |
|---|---|
| `libmem/tlsf/ports/freertos/tlsf_port.h` | 集成层 API（malloc/calloc/realloc/memalign/free + stats） |
| `libmem/tlsf/ports/freertos/tlsf_port.c` | 单实例池 + 锁 + `__wrap_*` + 统计 |
| `libmem/tlsf/ports/freertos/tlsf_freertos.c` | FreeRTOS heap 实现（`pvPortMalloc` 等 → TLSF） |
| `libmem/tlsf/ports/freertos/tlsf_heap.cpp` | C++ `operator new/delete` 覆盖 |
| `libmem/tlsf/CMakeLists.txt` | `tlsf` 静态库（纯算法 + 集成层，仅 `TLSF_MALLOC=ON`） |
| `boards/mps2-an505/FreeRTOS/application/sysmem_tlsf.c` | TLSF 模式下的 `_sbrk`：独立 32KB 静态池供 newlib 内部用（与 TLSF 主池隔离） |

### 修改
| 文件 | 改动 |
|---|---|
| `CMakeLists.txt`（顶层） | `LANGUAGES C CXX ASM`；新增 `TLSF_MALLOC` 开关（mps2-an505 FreeRTOS 默认 ON）；`ON` 时 `FREERTOS_HEAP` 指向 `tlsf_freertos.c`，`OFF` 时用 `heap_4` |
| `cmake/arm-none-eabi-gcc.cmake` | 加 `CMAKE_CXX_FLAGS_INIT`（-mcpu/-mthumb/... -fno-exceptions -fno-rtti） |
| `libutils/CMakeLists.txt` | `add_subdirectory(tlsf)` |
| `boards/mps2-an505/FreeRTOS/CMakeLists.txt` | `ON`：链接 `tlsf` + `-Wl,--wrap=malloc,free,calloc,realloc,memalign`，源文件二选一用 `sysmem_tlsf.c`；`OFF`：用原始 `sysmem.c` |
| `boards/mps2-an505/FreeRTOS/application/sysmem.c` | **保持原始（CubeMX 版本，未改）**；非 TLSF 模式仍用它 |

---

## 3. 关键设计点

0. **开关 `TLSF_MALLOC`**：顶层定义，mps2-an505 FreeRTOS 默认 `ON`。`ON` 时编译集成层 +
   `sysmem_tlsf.c` + `tlsf_freertos.c` + `--wrap`；`OFF` 时完全回到原配置（`heap_4` + 原始 `sysmem.c`）。
1. **为什么不用 heap_3（pvPortMalloc→malloc）**：heap_3 的 `xPortGetFreeHeapSize()` 返回 0，
   而 `pj_phone.c` 依赖该值显示堆剩余。故用自定义 `tlsf_freertos.c`，可正确返回 TLSF 剩余。
2. **`--wrap` 不作用于 newlib 库内部**：printf 等 libc 内部 malloc 仍走 `_sbrk`（32KB 后备池），
   用户代码 + pjlib + C++ 的分配全走 TLSF（主池 ~3.x MB）。
3. **FreeRTOS 调度器启动前**：懒初始化，首次 `pvPortMalloc/malloc` 即建池；启动阶段单线程无竞争。
4. **`new` 自动接管**：arm-none-eabi 的 `operator new` 默认调 `malloc`；显式覆盖后直接调 `tlsf_port_malloc`，
   即使以后加 C++ 代码 new/delete 也统一走 TLSF。
5. **lwIP 内存**：`MEM_LIBC_MALLOC` 未开，lwIP 用内部静态池（mem.c，MEM_SIZE=64K），不走 TLSF——正常设计。

---

## 4. 验证结果（QEMU mps2-an505 + FreeSWITCH 1.11.3 + tap0）

- ✅ 构建：`cmake -B build-phone ... -DPJ_PHONE=ON` 全量编译链接通过（C++ 链接器）。
- ✅ 符号：`__wrap_malloc/free/calloc`、`tlsf_port_*` 在 elf；`pvPortMalloc` 来自
  `libfreertos_kernel.a(tlsf_freertos.c.obj)`；无 `heap_4.o`。
- ✅ 启动：`pjlib pool [PASS]`、`pj_test ALL PASSED`、lwIP up 172.16.23.50、注册 `200 OK`。
- ✅ 通话：`dial 9196` → ACTIVE（dur 3.2s），媒体收发正常（UDP xmit=247 recv=255），
  `memp: mem.used=0/65536 max=1332 err=0 | UDP_PCB 6/24 err=0`，无 ENOBUFS。
- ✅ 挂断正常。

---

## 5. 后续可选项

- 在 UDP 命令服务器加 `mem` 命令显示 TLSF 池统计（used/free/min-free）便于调试。
- 若真机内存紧张，可调 `tlsf_heap` pool 上限或给 `_sbrk` 后备池调大小（当前 32KB）。
- 其它工程（BareMetal/threadx/stm32f405rg）仍用各自默认 malloc，未接入 TLSF（需要时同法接入）。

> 构建命令（固件仓库根）：
> `cmake -B build-phone -S . -G Ninja -DCMAKE_MAKE_PROGRAM=C:/Users/xidon/program/Ninja/ninja.exe -DCMAKE_TOOLCHAIN_FILE=.../cmake/arm-none-eabi-gcc.cmake -DBOARD=mps2-an505 -DPROJECT=FreeRTOS -DPJ_PHONE=ON`
> 产物：`build-phone\boards\mps2-an505\FreeRTOS\an505-qemu.elf`
