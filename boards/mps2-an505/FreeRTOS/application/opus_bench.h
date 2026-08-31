/*
 * opus_bench.h -- standalone libopus encode/decode timing benchmark
 *
 * Benchmark mode is enabled by building with -DPJ_PHONE_OPUS_BENCH=1
 * (plus PJ_PHONE=1); main.c then runs ONLY this task (no pjsua, no
 * lwIP, no LVGL) so the CPU is dedicated to measuring libopus.
 */
#ifndef OPUS_BENCH_H
#define OPUS_BENCH_H

void opus_bench_start(void);

#endif /* OPUS_BENCH_H */
