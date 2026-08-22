/**
 * @file pj_test.c
 * @brief PJLIB FreeRTOS port self-test.
 *
 * Exercises pj_init(), threads, mutexes, semaphores, atomics, timer heap,
 * pools and the time/timestamp API on top of the os_core_freertos.c port.
 * Runs in a FreeRTOS task; prints PASS/FAIL per sub-test over the UART.
 */
#include <stdio.h>
#include <string.h>

#include "printf.h"
#include "pj_test.h"

#include <pj/os.h>
#include <pj/pool.h>
#include <pj/log.h>
#include <pj/timer.h>
#include <pj/errno.h>
#include <pj/string.h>

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            printf("pj_test: CHECK FAILED: %s (line %d)\r\n", \
                   #expr, __LINE__); \
            return -1; \
        } \
    } while (0)

/* Pool factory used by every pool in this test. */
static pj_caching_pool g_cp;

/* ------------------------------------------------------------------ */
/* Thread + mutex + atomic                                             */
/* ------------------------------------------------------------------ */
#define NTHREADS   4
#define PER_ITER   200

static pj_mutex_t *g_mutex;
static pj_atomic_t *g_atomic;
static int g_counter;

static int worker(void *arg)
{
    int i;

    (void)arg;

    for (i = 0; i < PER_ITER; ++i) {
        pj_mutex_lock(g_mutex);
        ++g_counter;
        pj_mutex_unlock(g_mutex);

        pj_atomic_inc(g_atomic);
    }

    return 0;
}

static int test_thread_mutex_atomic(void)
{
    pj_thread_t *th[NTHREADS];
    pj_status_t rc;
    int i;

    rc = pj_mutex_create(NULL, "gmut", PJ_MUTEX_SIMPLE, &g_mutex);
    CHECK(rc == PJ_SUCCESS);

    rc = pj_atomic_create(NULL, 0, &g_atomic);
    CHECK(rc == PJ_SUCCESS);

    g_counter = 0;

    for (i = 0; i < NTHREADS; ++i) {
        char name[16];

        pj_ansi_snprintf(name, sizeof(name), "pjwk%d", i);
        rc = pj_thread_create(NULL, name, worker, NULL, 4096, 0, &th[i]);
        CHECK(rc == PJ_SUCCESS);
    }

    for (i = 0; i < NTHREADS; ++i) {
        rc = pj_thread_join(th[i]);
        CHECK(rc == PJ_SUCCESS);
    }

    CHECK(g_counter == NTHREADS * PER_ITER);
    CHECK(pj_atomic_get(g_atomic) == NTHREADS * PER_ITER);

    for (i = 0; i < NTHREADS; ++i)
        pj_thread_destroy(th[i]);

    pj_mutex_destroy(g_mutex);
    pj_atomic_destroy(g_atomic);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Semaphore                                                           */
/* ------------------------------------------------------------------ */
static int test_sem(void)
{
    pj_sem_t *sem;
    pj_status_t rc;

    rc = pj_sem_create(NULL, "sem", 0, 10, &sem);
    CHECK(rc == PJ_SUCCESS);

    /* empty -> trywait fails */
    rc = pj_sem_trywait(sem);
    CHECK(rc != PJ_SUCCESS);

    rc = pj_sem_post(sem);
    CHECK(rc == PJ_SUCCESS);
    rc = pj_sem_post(sem);
    CHECK(rc == PJ_SUCCESS);

    rc = pj_sem_wait(sem);
    CHECK(rc == PJ_SUCCESS);
    rc = pj_sem_wait(sem);
    CHECK(rc == PJ_SUCCESS);

    /* drained again -> trywait fails */
    rc = pj_sem_trywait(sem);
    CHECK(rc != PJ_SUCCESS);

    pj_sem_destroy(sem);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Timer heap                                                          */
/* ------------------------------------------------------------------ */
static unsigned g_timer_fired;

static void timer_cb(pj_timer_heap_t *ht, pj_timer_entry *e)
{
    (void)ht;
    (void)e;
    ++g_timer_fired;
}

static int test_timer(void)
{
    pj_pool_t *pool;
    pj_timer_heap_t *ht;
    pj_timer_entry entry;
    pj_time_val delay, expire, now, next_delay;
    pj_status_t rc;

    pool = pj_pool_create(&g_cp.factory, "tmr", 8192, 1024, NULL);
    CHECK(pool != NULL);

    rc = pj_timer_heap_create(pool, 32, &ht);
    CHECK(rc == PJ_SUCCESS);

    pj_timer_entry_init(&entry, 0, NULL, timer_cb);
    g_timer_fired = 0;

    /* Schedule a 100 ms timer. */
    delay.sec = 0;
    delay.msec = 100;
    rc = pj_timer_heap_schedule(ht, &entry, &delay);
    CHECK(rc == PJ_SUCCESS);

    /* Wait up to 2 s for it to fire. */
    pj_gettimeofday(&expire);
    expire.sec += 2;

    do {
        pj_timer_heap_poll(ht, &next_delay);
        pj_gettimeofday(&now);
        if (g_timer_fired > 0)
            break;
        if (PJ_TIME_VAL_GTE(now, expire))
            break;
        pj_thread_sleep(5);
    } while (1);

    CHECK(g_timer_fired == 1);

    pj_timer_heap_destroy(ht);
    pj_pool_release(pool);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Pool                                                                */
/* ------------------------------------------------------------------ */
static int test_pool(void)
{
    pj_pool_t *pool;
    void *p1, *p2;

    pool = pj_pool_create(&g_cp.factory, "pool", 1024, 256, NULL);
    CHECK(pool != NULL);

    p1 = pj_pool_alloc(pool, 64);
    CHECK(p1 != NULL);
    CHECK(((pj_size_t)p1 & (PJ_POOL_ALIGNMENT - 1)) == 0);

    p2 = pj_pool_calloc(pool, 4, 16);
    CHECK(p2 != NULL);
    CHECK(p1 != p2);

    CHECK(pj_pool_get_used_size(pool) >= 64 + 64);

    pj_pool_release(pool);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Time / timestamp                                                    */
/* ------------------------------------------------------------------ */
static int test_time(void)
{
    pj_time_val tv1, tv2;
    pj_timestamp ts1, ts2;
    pj_uint32_t ms_wall, ms_ts;

    CHECK(pj_gettimeofday(&tv1) == PJ_SUCCESS);
    CHECK(pj_get_timestamp(&ts1) == PJ_SUCCESS);

    pj_thread_sleep(150);

    CHECK(pj_gettimeofday(&tv2) == PJ_SUCCESS);
    CHECK(pj_get_timestamp(&ts2) == PJ_SUCCESS);

    ms_wall = (pj_uint32_t)(tv2.sec * 1000 + tv2.msec) -
              (pj_uint32_t)(tv1.sec * 1000 + tv1.msec);
    ms_ts   = pj_elapsed_msec(&ts1, &ts2);

    printf("pj_test:   sleep(150ms): wall=%ums ts=%ums\r\n",
           ms_wall, ms_ts);

    CHECK(ms_wall >= 100 && ms_wall <= 1000);
    CHECK(ms_ts   >= 100 && ms_ts   <= 1000);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */
static int test_log(void)
{
    PJ_LOG(1, ("pj_test", "log level 1 (always shown)"));
    PJ_LOG(4, ("pj_test", "log level 4"));
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main entry                                                          */
/* ------------------------------------------------------------------ */
int pj_test_run(void)
{
    pj_status_t rc;
    int fail = 0;

    printf("\r\n=== PJLIB FreeRTOS port self-test ===\r\n");

    rc = pj_init();
    if (rc != PJ_SUCCESS) {
        printf("pj_test: pj_init() FAILED (status=%d)\r\n", rc);
        return -1;
    }

    printf("pj_test: pjlib %s initialized, running in thread '%s'\r\n",
           pj_get_version(),
           pj_thread_get_name(pj_thread_this()));

    pj_caching_pool_init(&g_cp, &pj_pool_factory_default_policy, 0);

    fail += test_pool()  ? 1 : 0;
    printf("pj_test: pool          [%s]\r\n", fail ? "FAIL" : "PASS");

    fail += test_sem()   ? 1 : 0;
    printf("pj_test: semaphore     [%s]\r\n", fail ? "FAIL" : "PASS");

    fail += test_time()  ? 1 : 0;
    printf("pj_test: time/ts       [%s]\r\n", fail ? "FAIL" : "PASS");

    fail += test_timer() ? 1 : 0;
    printf("pj_test: timer heap    [%s]\r\n", fail ? "FAIL" : "PASS");

    fail += test_thread_mutex_atomic() ? 1 : 0;
    printf("pj_test: thread/mtx/at [%s]\r\n", fail ? "FAIL" : "PASS");

    fail += test_log()   ? 1 : 0;
    printf("pj_test: logging       [%s]\r\n", fail ? "FAIL" : "PASS");

    printf("pj_test: %s\r\n",
           fail ? "FAILED" : "ALL PASSED");

    pj_shutdown();
    return fail ? -1 : 0;
}
