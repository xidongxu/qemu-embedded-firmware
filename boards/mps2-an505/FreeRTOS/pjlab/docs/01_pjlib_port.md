# 实验 01 — PJLIB 移植自测（pj_test.c）

**阶段**：PJLIB FreeRTOS 移植验证（os_core_freertos.c）。

## 目的
验证 pjproject 最底层的 **pjlib 操作系统抽象层**在 FreeRTOS 上正常工作。pjlib 是 pjproject 一切功能的基石（线程、同步、内存、时间），如果这一层移植不对，上层 SIP/媒体全都会崩。

## 思路
把 pjlib 的每个 OS 原语在 FreeRTOS 上跑一遍，逐个 `CHECK`，任何一个失败立即返回 `-1` 并打印失败表达式。

## 流程
1. `pj_init()` 初始化 pjlib（注册 FreeRTOS 后端）。
2. **线程 + 互斥锁 + 原子**：创建 4 个线程，各用 `pj_mutex_lock/unlock` 递增共享计数器、用 `pj_atomic_inc` 递增原子计数器；join 后校验 `counter == 4*200`。
3. **信号量**：创建计数信号量，验证 `trywait`（空→失败）、`post`、`wait`、再 `trywait`（已空→失败）。
4. **定时器堆**：创建定时器、验证触发与超时回调。
5. **内存池**：`pj_caching_pool` + `pj_pool_create/alloc`。
6. 汇总打印 `PASS/FAIL`。

## 学到什么
- pjlib 的 OS 抽象：`pj_thread/mutex/sem/atomic/timer/pool`。
- FreeRTOS 移植层（`os_core_freertos.c`）如何把 pjlib 调用映射到 FreeRTOS API（`xTaskCreate`、`xSemaphore` 等）。
- 理解"移植 pjproject 到新 RTOS 需要提供什么"。

## 如何启动
默认构建即可（main 里 `pj_test_run()` 最先执行）：
```
cmake --build build
qemu-system-arm.exe -machine mps2-an505 -cpu cortex-m33 -m 16M -display none -serial stdio -kernel build\boards\mps2-an505\FreeRTOS\an505-qemu.elf
```

## 成功标准
串口出现：`pj_test: thread/mtx/at [PASS]`、`pj_test: sem [PASS]` … 最终 `pj_test: ALL PASSED`。
