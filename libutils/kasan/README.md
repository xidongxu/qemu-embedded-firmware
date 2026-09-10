# kasan

**Cortex-M 最小 KASan（内存安全检测）库** —— 堆越界 / 下溢 / use-after-free /
double-free 即时捕获。配合 GCC `-fsanitize=kernel-address` 使用：被测代码的每次
内存访问都被编译器转成对 `__asan_{load,store}{1,2,4,8,16}_noabort(addr)` 的调用，
本库拥有影子内存 + **可插拔的分配器后端**（默认对接项目里已在用的 TLSF），实现
**真 KASan 语义**。核心 `kasan.c` 零依赖（无 CMSIS / RTOS / printf）；TLSF 后端
另接 `libmem/tlsf`。GCC/armclang 皆可。

## 原理（为什么比 canary 强）

- 影子区中**只有"已分配块的 user 区"是 unpoisoned**，其余（块头 / free 块 /
  未用区）全部 poison。
- 越界写 `p[size] = x`：编译器插入的 shadow 检查在**写入瞬间**命中相邻
  poison → 当场捕获（不是等到 free 才查 canary）。
- `free` 后整块重新 poison → 之后任何访问即 UAF 捕获。
- 存活分配记录表校验 → double-free / bad-free 捕获。

影子默认从**被测区自身尾部**划出（尾部 1/8），无需专门影子 RAM，符合固定
MCU 内存布局；定义 `KASAN_SHADOW_BASE` 可改用独立 RAM 段（整区皆可测）。

影子字节按 8 字节粒度编码：`0x00` 全可访问、`0x01–0x07` 前 N 字节可访问、
`0xf1–0xf7` 前 N 字节 poison、`0xfa` freed（UAF）、`0xfb` redzone（块头 /
空闲区）、`0xff` 通用 poison。块尺寸非 8 倍数时，尾部用 partial 编码精确
标记，尾部 4 字节的越界也能被捕获。

报告除 `kasan_report_{type,addr,size,shadow,pc}` 外，还提供 `kasan_report_cause`
（1=redzone 越界 2=freed UAF 3=partial 边界 4=通用 poison）与
`kasan_report_shadow_dump[16]`（故障地址周围影子状态）；`kasan_shadow_name()`
可把影子值翻译成可读字符串（`"freed"`、`"redzone"` 等）。

## 使用

1. 被测目标加编译选项 + 链接本库：

```cmake
target_compile_options(<app> PRIVATE -fsanitize=kernel-address)
target_link_libraries(<app> PRIVATE kasan)
```

2. 启动时初始化（先注册分配器后端，默认用 TLSF）：

```c
kasan_set_alloc_backend(kasan_tlsf_backend());  /* 选择分配器后端 */
kasan_init();                                    /* 清影子 */
kasan_heap_init();                               /* 建堆 arena */
```

3. 之后用 `kasan_malloc` / `kasan_free` / `kasan_calloc` / `kasan_realloc` /
   `kasan_memalign` 分配（块头 / 空闲区由库维持 poison），越界 / UAF /
   double-free 自动触发报告（写 `kasan_report_*` marker 后 trap；QEMU 下用
   gdb 读，可换成 UART/tracer sink）。`realloc` 迁移后旧指针会被重新 poison，
   经旧指针的 UAF 同样被拦。

> 本库**必须无 sanitize 编译**（`kasan.c` 需直碰 shadow / poison 区）；
> 只有"被测代码"插桩。

## 分配器后端

kasan **不实现内存分配算法**：它只维护影子内存 + 一张"存活分配记录表"（用于
double-free / bad-free 探测），真正的分配策略委托给一个后端（
`kasan_alloc_backend_t`）。这样内存管理沿用项目里久经验证的算法，并且可以在
不同系统上插不同后端（TLSF / newlib `malloc` / RTOS 堆 / ...）：

```c
typedef struct kasan_alloc_backend {
    const char *name;
    void (*init)(uint32_t *base, uint32_t *size);   /* 建 arena，回报范围 */
    void *(*malloc)(uint32_t bytes);
    uint32_t (*usable)(void *p);                    /* 用户区大小，可为 NULL */
    void (*free)(void *p);
    void *(*realloc)(void *p, uint32_t bytes);      /* 可为 NULL（回退 malloc+copy+free） */
    void *(*memalign)(uint32_t align, uint32_t bytes); /* 可为 NULL（kasan_memalign 返 NULL） */
} kasan_alloc_backend_t;
```

- 内置 TLSF 后端：`kasan_tlsf_backend()`（`kasan_alloc_tlsf.c`，链接 `libmem/tlsf`）。
- 自定义后端：实现上述 4 个函数后 `kasan_set_alloc_backend(&my_backend)`。
- 后端须**无 sanitize 编译**（它直碰被 poison 的 arena 元数据，不看 shadow）。

## 内存布局（可 -D 覆盖，默认 = mps2-an505 QEMU 验证值）

| 宏 | 默认 | 含义 |
|---|---|---|
| `KASAN_REGION_BASE` | `0x80000000` | 被测区基址（真实 RAM） |
| `KASAN_REGION_SIZE` | `0x00040000` | 被测区总大小（256 KB） |
| `KASAN_SHADOW_BASE` | 区尾推导 | 影子基址：默认=被测区尾部 1/8（inline）；定义则用独立 RAM |
| `KASAN_HEAP_SIZE` | 64 KB | TLSF arena 大小（须落在除影子外的可用区内，内含 TLSF 控制块） |
| `KASAN_LIVE_MAX` | 4096 | 存活分配记录表容量（同时存活的分配数上限，每条 12 字节→约 48 KB；满则关闭 bad-free 探测） |

inline 模式下：`shadow_of(a) = 区尾 + (a - 区基)/8`，仅对可用区（区头到影子区）
有效；影子区（区尾 1/8）不放置链接数据。链接脚本 RAM 长度须设可用区大小。

## 目录

```
kasan.h             配置宏 + 后端接口 + API 声明
kasan.c             库核心（shadow + 存活记录表 + noabort 钩子）
kasan_alloc_tlsf.c  TLSF 分配器后端（对接 libmem/tlsf）
TODO.md             与 Linux KASan 的功能差距 + 缺陷待办清单
CMakeLists.txt      库构建（install/export + find_package）
kasanConfig.cmake.in
tests/
  host/    host 单测（RAM 假 region，#include kasan.c + 后端 + tlsf.c）
  qemu/    QEMU 固件测试（ARM + mps2-an505 板）
```

## 测试

### host 单测（无 ARM / QEMU）

`test_kasan.c` 把 region / shadow / arena 重定向到 host RAM 数组（宏注入），
`#include kasan.c` 直测：shadow API、heap first-fit 布局 / 切块 / free 后
poison、double-free / bad-free 报告、边界情形（`free(NULL)` no-op、溢出请求
拒绝、超 arena 请求返回 NULL 且堆仍可用、`malloc(0)` 给最小块）。
`KASAN_TEST_RETURNS` 使报告返回而非 trap。库用 32 位地址（`uint32_t`），
**host 测试需 32 位编译**（Linux `-m32`；Windows 可用 zig 32 位或 64 位 +
低 image-base）。

```bash
cmake -B build-host -S libutils/kasan -DKASAN_BUILD_TESTS=ON
cmake --build build-host && ctest --test-dir build-host
```

### QEMU 固件（ARM，板绑定 mps2-an505）

`tests/qemu` 编一个 `-fsanitize=kernel-address` 插桩裸机镜像，`KASAN_TEST_CASE`
注入故障（1=堆越界 / 2=UAF / 3=double-free / 4=堆前越界 underflow /
5=realloc 迁移后旧指针 UAF / 6=realloc 缩容写入释放尾巴），报告落 `kasan_*`
marker 供 gdb 读取判 PASS。

```bash
cmake -B build -S . -DBOARD=mps2-an505 -DKASAN_BUILD_TESTS=ON \
      -DKASAN_TEST_CASE=1
cmake --build build --target kasan_qemu_test
```
