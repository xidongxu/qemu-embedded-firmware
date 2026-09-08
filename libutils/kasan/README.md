# kasan

**Cortex-M 最小 KASan（内存安全检测）库** —— 堆越界 / 下溢 / use-after-free /
double-free 即时捕获 + 泄漏检测。配合 GCC `-fsanitize=kernel-address` 使用：被测代码的每次
内存访问都被编译器转成对 `__asan_{load,store}{1,2,4,8,16}_noabort(addr)` 的调用，
本库拥有影子内存 + 堆分配器，实现**真 KASan 语义**。自包含、零依赖（无 CMSIS /
RTOS / printf），GCC/armclang 皆可。

## 原理（为什么比 canary 强）

- 影子区中**只有“已分配块的 user 区”是 unpoisoned**，其余（块头 / free 块 /
  未用区）全部 poison。
- 越界写 `p[size] = x`：编译器插入的 shadow 检查在**写入瞬间**命中相邻
  poison → 当场捕获（不是等到 free 才查 canary）。
- `free` 后整块重新 poison → 之后任何访问即 UAF 捕获。
- 块头 magic 校验 → double-free / bad-free 捕获。
- `kasan_malloc` 记录每块活分配（指针 / 大小 / 调用点），`kasan_leak_dump()`
  重计仍未释放的块 → 泄漏检测（记录表满则新分配照常但不入表，`kasan_leak_lost` 计数）。

影子默认从**被测区自身尾部**划出（尾部 1/8），无需专门影子 RAM，符合固定
MCU 内存布局；定义 `KASAN_SHADOW_BASE` 可改用独立 RAM 段（整区皆可测）。

## 使用

1. 被测目标加编译选项 + 链接本库：

```cmake
target_compile_options(<app> PRIVATE -fsanitize=kernel-address)
target_link_libraries(<app> PRIVATE kasan)
```

2. 启动时初始化：

```c
kasan_init();        /* 清影子 */
kasan_heap_init();   /* 建堆 arena */
```

3. 之后用 `kasan_malloc` / `kasan_free` 分配（块头 / 邻居由库管理），越界 /
   UAF / double-free 自动触发报告（写 `kasan_report_*` marker 后 trap；QEMU
   下用 gdb 读，可换成 UART/tracer sink）。
4. 泄漏检测：在工作负载结束（应该已释放全部临时块）后调 `kasan_leak_dump()`，
   若 `kasan_leak_count != 0` 则有泄漏；用 `kasan_leak_live(i)` 逐条取
   （返回 user 指针，回传 size / 调用点），或用 gdb 直接读 `kasan_leak_ptr /
   _size / _pc` 看第一条。

> 本库**必须无 sanitize 编译**（`kasan.c` 需直碰 shadow / poison 区）；
> 只有"被测代码"插桩。

## 内存布局（可 -D 覆盖，默认 = mps2-an505 QEMU 验证值）

| 宏 | 默认 | 含义 |
|---|---|---|
| `KASAN_REGION_BASE` | `0x80000000` | 被测区基址（真实 RAM） |
| `KASAN_REGION_SIZE` | `0x00040000` | 被测区总大小（256 KB） |
| `KASAN_SHADOW_BASE` | 区尾推导 | 影子基址：默认=被测区尾部 1/8（inline）；定义则用独立 RAM |
| `KASAN_HEAP_SIZE` | 64 KB | 堆 arena 大小（须落在除影子外的可用区内） |

inline 模式下：`shadow_of(a) = 区尾 + (a - 区基)/8`，仅对可用区（区头到影子区）
有效；影子区（区尾 1/8）不放置链接数据。链接脚本 RAM 长度须设可用区大小。

## 目录

```
kasan.h / kasan.c   库源码（shadow + 分配器 + noabort 钩子）
CMakeLists.txt      库构建（install/export + find_package）
kasanConfig.cmake.in
tests/
  host/    host 单测（RAM 假 region，#include kasan.c）
  qemu/    QEMU 固件测试（ARM + mps2-an505 板）
```

## 测试

### host 单测（无 ARM / QEMU）

`test_kasan.c` 把 region / shadow / arena 重定向到 host RAM 数组（宏注入），
`#include kasan.c` 直测：shadow API、heap first-fit 布局 / 切块 / free 后
poison、double-free / bad-free 报告、泄漏检测（dump / 逐条 walk / 释放移除 /
记录表满 `lost`）。库用 32 位地址（`uint32_t`），**host 测试需 32 位编译**
（Linux `-m32`；Windows 可用 zig 32 位或 64 位 + 低 image-base）。

```bash
cmake -B build-host -S libutils/kasan -DKASAN_BUILD_TESTS=ON
cmake --build build-host && ctest --test-dir build-host
```

### QEMU 固件（ARM，板绑定 mps2-an505）

`tests/qemu` 编一个 `-fsanitize=kernel-address` 插桩裸机镜像，`KASAN_TEST_CASE`
注入故障（1=堆越界 / 2=UAF / 3=double-free / 4=泄漏检测），报告落 `kasan_*`
marker 供 gdb 读取判 PASS。

```bash
cmake -B build -S . -DBOARD=mps2-an505 -DKASAN_BUILD_TESTS=ON \
      -DKASAN_TEST_CASE=1
cmake --build build --target kasan_qemu_test
```
