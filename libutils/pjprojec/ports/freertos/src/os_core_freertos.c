/*
 * os_core_freertos.c
 *
 * PJLIB operating system abstraction layer for FreeRTOS (Cortex-M).
 *
 * Implements the pj_thread/pj_mutex/pj_sem/pj_atomic/pj_event/pj_barrier
 * API surface declared in <pj/os.h> on top of FreeRTOS primitives.
 *
 * Design notes:
 *  - pj_thread_t is tracked in a global linked list keyed by the FreeRTOS
 *    task handle, so pj_thread_this()/TLS work without OS thread-local
 *    storage (FreeRTOS has none by default).
 *  - Thread-local "slots" are stored inside each pj_thread_t.
 *  - Mutexes map to FreeRTOS recursive mutexes (safe superset).
 *  - Semaphores map to FreeRTOS counting semaphores.
 */
#include <pj/os.h>
#include <pj/except.h>
#include <pj/pool.h>
#include <pj/assert.h>
#include <pj/errno.h>
#include <pj/guid.h>
#include <pj/log.h>
#include <pj/rand.h>
#include <pj/string.h>

#include <stdlib.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>

#define THIS_FILE   "os_core_freertos.c"

/* Number of thread-local slots per thread. */
#ifndef PJ_THREAD_LOCAL_MAX
#   define PJ_THREAD_LOCAL_MAX   8
#endif

/* FreeRTOS priority used for pj_thread_create(). */
#ifndef PJ_FREERTOS_DEFAULT_PRIO
#   define PJ_FREERTOS_DEFAULT_PRIO   2
#endif

/* ------------------------------------------------------------------------
 * Structures
 */

struct pj_thread_t
{
    char            obj_name[PJ_MAX_OBJ_NAME];

    TaskHandle_t    task;           /* FreeRTOS task handle */
    pj_thread_proc *proc;           /* thread entry */
    void           *arg;            /* thread argument */
    int             ret;            /* proc() return value */

    SemaphoreHandle_t done_sem;     /* signalled when thread returns */
    volatile pj_bool_t finished;    /* thread has returned */

    pj_uint32_t     signature;      /* sanity marker */
    void           *tls[PJ_THREAD_LOCAL_MAX];   /* thread local slots */

    int             malloced;       /* struct was malloc'd (vs pool) */
    struct pj_thread_t *next;       /* list link */
};

struct pj_atomic_t
{
    pj_mutex_t         *mutex;
    pj_atomic_value_t   value;
    int                 malloced;
};

struct pj_mutex_t
{
    SemaphoreHandle_t   handle;
    char                obj_name[PJ_MAX_OBJ_NAME];
    pj_thread_t        *owner;     /* for pj_mutex_is_locked() */
    unsigned            nesting;
    int                 malloced;
};

#if defined(PJ_HAS_SEMAPHORE) && PJ_HAS_SEMAPHORE != 0
struct pj_sem_t
{
    SemaphoreHandle_t   sem;
    char                obj_name[PJ_MAX_OBJ_NAME];
    int                 malloced;
};
#endif  /* PJ_HAS_SEMAPHORE */

#if defined(PJ_HAS_EVENT_OBJ) && PJ_HAS_EVENT_OBJ != 0
struct pj_event_t
{
    pj_bool_t           manual_reset;
    pj_bool_t           is_set;
    unsigned            waiters;
    pj_mutex_t         *mutex;
    SemaphoreHandle_t   sem;
    int                 malloced;
};
#endif  /* PJ_HAS_EVENT_OBJ */

struct pj_barrier_t
{
    pj_mutex_t         *mutex;
    SemaphoreHandle_t   sem;        /* counting semaphore */
    unsigned            count;
    unsigned            trip_count;
    int                 malloced;
};

/* ------------------------------------------------------------------------
 * Global state
 */

static int initialized;                 /* pj_init() refcount */
static long thread_tls_id = -1;         /* TLS slot holding pj_thread_t* */
static unsigned tls_alloc_mask;         /* allocated TLS slot bitmap */

static pj_mutex_t *critical_section;    /* pj_enter_critical_section() */
static pj_mutex_t *thread_list_mutex;   /* protects thread_list */

static struct pj_thread_t *thread_list; /* all known pj_thread_t */

static unsigned atexit_count;
static void (*atexit_func[32])(void);

/* ------------------------------------------------------------------------
 * Helpers
 */

static void thread_list_lock(void)
{
    if (thread_list_mutex)
        pj_mutex_lock(thread_list_mutex);
}

static void thread_list_unlock(void)
{
    if (thread_list_mutex)
        pj_mutex_unlock(thread_list_mutex);
}

/* Find a registered pj_thread_t for the current FreeRTOS task. */
static pj_thread_t *find_current_thread(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    pj_thread_t *t;
    unsigned guard = 0;
    static unsigned not_reg_warn;

    thread_list_lock();
    /* Guard against a corrupted/looping thread list (pjsua creates several
     * threads concurrently; a bad list makes this loop forever). */
    for (t = thread_list; t && guard < 256; t = t->next, guard++) {
        if (t->task == self)
            break;
    }
    if (!t && not_reg_warn < 5) {
        not_reg_warn++;
        /* A task calling into pjlib (mutex/thread_this) without ever being
         * registered: this makes pj_thread_this() return NULL and pjsua's
         * PJSUA_LOCK_IS_LOCKED() misbehave. */
        PJ_LOG(1, ("", "PJ THREAD NOT REGISTERED: task=%p name=%s",
                   (void *)self, pcTaskGetName(self)));
    }
    if (guard >= 256) {
        PJ_LOG(1, ("", "PJ THREAD LIST CORRUPT (loop?): t=%p task=%p self=%p",
                   (void *)t, (void *)(t ? t->task : NULL), (void *)self));
        t = NULL;
    }
    thread_list_unlock();

    return t;
}

static void thread_list_add(pj_thread_t *t)
{
    thread_list_lock();
    t->next = thread_list;
    thread_list = t;
    thread_list_unlock();
}

static void thread_list_remove(pj_thread_t *t)
{
    pj_thread_t **pp;

    thread_list_lock();
    for (pp = &thread_list; *pp; pp = &(*pp)->next) {
        if (*pp == t) {
            *pp = t->next;
            break;
        }
    }
    thread_list_unlock();
}

/* The actual FreeRTOS task body for threads created via pj_thread_create(). */
static void freertos_thread_main(void *arg)
{
    pj_thread_t *rec = (pj_thread_t *)arg;
    pj_thread_desc desc;
    pj_thread_t *self;

    /* Register this task with PJLIB (finds the existing record). */
    pj_bzero(desc, sizeof(desc));
    pj_thread_register(rec->obj_name, desc, &self);

    /* Run the thread body. */
    rec->ret = (*rec->proc)(rec->arg);

    /* Signal completion for pj_thread_join()/destroy(). */
    rec->finished = PJ_TRUE;
    if (rec->done_sem)
        xSemaphoreGive(rec->done_sem);

    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------------
 * PJLIB init / shutdown
 */

PJ_DEF(pj_status_t) pj_init(void)
{
    char dummy_guid[PJ_GUID_MAX_LENGTH];
    pj_str_t guid;
    pj_status_t rc;

    if (initialized) {
        ++initialized;
        return PJ_SUCCESS;
    }

    /* Init logging. */
    pj_log_init();

#if PJ_HAS_THREADS
    /* Guard for the thread list. */
    rc = pj_mutex_create(NULL, "thrlist", PJ_MUTEX_SIMPLE, &thread_list_mutex);
    if (rc != PJ_SUCCESS)
        return rc;

    /* Register the calling task as the PJLIB main thread. */
    rc = pj_thread_init();
    if (rc != PJ_SUCCESS)
        return rc;

    /* Critical section. */
    rc = pj_mutex_create(NULL, "critsec", PJ_MUTEX_RECURSE, &critical_section);
    if (rc != PJ_SUCCESS)
        return rc;
#endif

    /* Initialize exception ID for the pool. */
    rc = pj_exception_id_alloc("PJLIB/No memory", &PJ_NO_MEMORY_EXCEPTION);
    if (rc != PJ_SUCCESS)
        return rc;

    /* Startup GUID. */
    guid.ptr = dummy_guid;
    pj_generate_unique_string(&guid);

    /* Startup timestamp. */
#if defined(PJ_HAS_HIGH_RES_TIMER) && PJ_HAS_HIGH_RES_TIMER != 0
    {
        pj_timestamp dummy_ts;
        if ((rc = pj_get_timestamp(&dummy_ts)) != PJ_SUCCESS)
            return rc;
    }
#endif

    ++initialized;
    pj_assert(initialized == 1);

    PJ_LOG(4, (THIS_FILE, "pjlib %s for FreeRTOS initialized", PJ_VERSION));

    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_atexit(void (*func)(void))
{
    if (atexit_count >= PJ_ARRAY_SIZE(atexit_func))
        return PJ_ETOOMANY;

    atexit_func[atexit_count++] = func;
    return PJ_SUCCESS;
}

PJ_DEF(void) pj_shutdown(void)
{
    int i;

    pj_assert(initialized > 0);
    if (--initialized != 0)
        return;

    for (i = (int)atexit_count - 1; i >= 0; --i) {
        (*atexit_func[i])();
    }
    atexit_count = 0;

    if (PJ_NO_MEMORY_EXCEPTION != -1) {
        pj_exception_id_free(PJ_NO_MEMORY_EXCEPTION);
        PJ_NO_MEMORY_EXCEPTION = -1;
    }

#if PJ_HAS_THREADS
    if (critical_section) {
        pj_mutex_destroy(critical_section);
        critical_section = NULL;
    }

    if (thread_list_mutex) {
        pj_mutex_destroy(thread_list_mutex);
        thread_list_mutex = NULL;
    }

    if (thread_tls_id != -1) {
        pj_thread_local_free(thread_tls_id);
        thread_tls_id = -1;
    }
#endif

    pj_errno_clear_handlers();
}

PJ_DEF(pj_uint32_t) pj_getpid(void)
{
    return 0;
}

PJ_DEF(pj_bool_t) pj_thread_is_registered(void)
{
#if PJ_HAS_THREADS
    if (thread_tls_id < 0)
        return PJ_FALSE;
    return pj_thread_local_get(thread_tls_id) != NULL;
#else
    return PJ_TRUE;
#endif
}

PJ_DEF(void) pj_jni_set_jvm(void *jvm)
{
    (void)jvm;
}

PJ_DEF(pj_bool_t) pj_jni_attach_jvm(void **jni_env)
{
    (void)jni_env;
    return PJ_FALSE;
}

PJ_DEF(void) pj_jni_detach_jvm(pj_bool_t attached)
{
    (void)attached;
}

/* ------------------------------------------------------------------------
 * Threads
 */

/* Internal: register the calling task as a PJLIB thread. */
PJ_DEF(pj_status_t) pj_thread_init(void)
{
    pj_thread_desc desc;
    pj_thread_t *dummy;

    /* Allocate the TLS slot used to store the current pj_thread_t. */
    if (thread_tls_id < 0) {
        long idx;
        pj_status_t rc = pj_thread_local_alloc(&idx);
        if (rc != PJ_SUCCESS)
            return rc;
        thread_tls_id = idx;
    }

    pj_bzero(desc, sizeof(desc));
    return pj_thread_register("thr%p", desc, &dummy);
}

PJ_DEF(int) pj_thread_get_prio(pj_thread_t *thread)
{
    if (!thread || !thread->task)
        return 0;
    return (int)uxTaskPriorityGet(thread->task);
}

PJ_DEF(pj_status_t) pj_thread_set_prio(pj_thread_t *thread, int prio)
{
    if (!thread || !thread->task)
        return PJ_EINVAL;
    vTaskPrioritySet(thread->task, (UBaseType_t)prio);
    return PJ_SUCCESS;
}

PJ_DEF(int) pj_thread_get_prio_min(pj_thread_t *thread)
{
    (void)thread;
    return 1;
}

PJ_DEF(int) pj_thread_get_prio_max(pj_thread_t *thread)
{
    (void)thread;
    return configMAX_PRIORITIES - 1;
}

PJ_DEF(void *) pj_thread_get_os_handle(pj_thread_t *thread)
{
    if (!thread)
        return NULL;
    return (void *)thread->task;
}

PJ_DEF(const char *) pj_thread_get_name(pj_thread_t *thread)
{
    if (!thread)
        return "";
    return thread->obj_name;
}

PJ_DEF(pj_status_t) pj_thread_create(pj_pool_t *pool,
                                     const char *thread_name,
                                     pj_thread_proc *proc,
                                     void *arg,
                                     pj_size_t stack_size,
                                     unsigned flags,
                                     pj_thread_t **thread)
{
    pj_thread_t *rec;
    UBaseType_t stack_words;
    char task_name[configMAX_TASK_NAME_LEN];

    PJ_ASSERT_RETURN(proc && thread, PJ_EINVAL);

    rec = (pj_thread_t *)calloc(1, sizeof(pj_thread_t));
    if (!rec)
        return PJ_ENOMEM;

    if (thread_name)
        pj_ansi_strncpy(rec->obj_name, thread_name,
                        PJ_ARRAY_SIZE(rec->obj_name));
    else
        pj_ansi_strcpy(rec->obj_name, "pjthread");

    rec->signature = 0x4652544f;   /* 'FRTO' */
    rec->proc = proc;
    rec->arg = arg;
    rec->malloced = 1;

    if (stack_size == 0 || stack_size == PJ_THREAD_DEFAULT_STACK_SIZE)
        stack_size = PJ_THREAD_DEFAULT_STACK_SIZE;

    stack_words = (UBaseType_t)((stack_size + 3) / 4);
    if (stack_words < configMINIMAL_STACK_SIZE)
        stack_words = configMINIMAL_STACK_SIZE;

    rec->done_sem = xSemaphoreCreateBinary();
    if (!rec->done_sem) {
        free(rec);
        return PJ_ENOMEM;
    }

    /* FreeRTOS task name is length-limited. */
    pj_ansi_strncpy(task_name, rec->obj_name,
                    PJ_ARRAY_SIZE(task_name) - 1);
    task_name[PJ_ARRAY_SIZE(task_name) - 1] = '\0';

    if (xTaskCreate(freertos_thread_main, task_name, stack_words, rec,
                    PJ_FREERTOS_DEFAULT_PRIO, &rec->task) != pdPASS)
    {
        vSemaphoreDelete(rec->done_sem);
        free(rec);
        return PJ_ENOMEM;
    }

    if (flags & PJ_THREAD_SUSPENDED)
        vTaskSuspend(rec->task);

    thread_list_add(rec);

    /* Root fix: make the NEW thread's own TLS slot point at rec right now,
     * so a find_current_thread() match never sees a NULL tls[] entry in
     * the window before freertos_thread_main() runs pj_thread_register().
     * This writes rec (the new thread's record), NOT the caller's TLS.
     * pj_thread_register() in the wrapper re-sets the same value. */
    if (thread_tls_id >= 0)
        rec->tls[thread_tls_id] = rec;

    *thread = rec;
    (void)pool;
    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_thread_create2(const char *thread_name,
                                      pj_thread_proc *proc,
                                      void *arg,
                                      pj_size_t stack_size,
                                      void *stack_addr,
                                      pj_thread_t *thread)
{
    /* Note: this port uses FreeRTOS-managed stacks, so stack_addr is
     * ignored here. Kept for API compatibility.
     */
    (void)stack_addr;
    return pj_thread_create(NULL, thread_name, proc, arg, stack_size,
                            PJ_THREAD_ALLOCATE_STACK ? PJ_THREAD_SUSPENDED : 0,
                            &thread);
}

PJ_DEF(pj_status_t) pj_thread_register(const char *thread_name,
                                       pj_thread_desc desc,
                                       pj_thread_t **thread)
{
    pj_thread_t *rec;

    (void)desc;

    /* Already registered for the current task? */
    rec = find_current_thread();
    if (!rec) {
        rec = (pj_thread_t *)calloc(1, sizeof(pj_thread_t));
        if (!rec)
            return PJ_ENOMEM;

        if (thread_name)
            pj_ansi_strncpy(rec->obj_name, thread_name,
                            PJ_ARRAY_SIZE(rec->obj_name));
        else
            pj_ansi_strcpy(rec->obj_name, "pjthread");

        rec->task = xTaskGetCurrentTaskHandle();
        rec->signature = 0x4652544f;
        rec->malloced = 1;
        thread_list_add(rec);
    }

    if (thread_tls_id >= 0)
        pj_thread_local_set(thread_tls_id, rec);

    if (thread)
        *thread = rec;

    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_thread_unregister(void)
{
    pj_thread_t *rec = find_current_thread();

    if (rec) {
        thread_list_remove(rec);
        if (rec->done_sem)
            vSemaphoreDelete(rec->done_sem);
        if (rec->malloced)
            free(rec);
    }

    if (thread_tls_id >= 0)
        pj_thread_local_set(thread_tls_id, NULL);

    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_thread_attach(const char *thread_name,
                                     pj_thread_desc desc,
                                     pj_thread_t **thread)
{
    return pj_thread_register(thread_name, desc, thread);
}

PJ_DEF(pj_thread_t *) pj_thread_this(void)
{
#if PJ_HAS_THREADS
    if (thread_tls_id < 0)
        return NULL;
    return (pj_thread_t *)pj_thread_local_get(thread_tls_id);
#else
    return NULL;
#endif
}

PJ_DEF(pj_status_t) pj_thread_join(pj_thread_t *thread)
{
    if (!thread)
        return PJ_EINVAL;

    if (thread->finished)
        return PJ_SUCCESS;

    if (!thread->done_sem)
        return PJ_EINVAL;

    if (thread->task == xTaskGetCurrentTaskHandle())
        return PJ_EINVAL;   /* cannot join self */

    xSemaphoreTake(thread->done_sem, portMAX_DELAY);
    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_thread_destroy(pj_thread_t *thread)
{
    if (!thread)
        return PJ_EINVAL;

    thread_list_remove(thread);

    /* Wait for it to finish (unless it is the calling thread). */
    if (!thread->finished && thread->done_sem &&
        thread->task != xTaskGetCurrentTaskHandle())
    {
        xSemaphoreTake(thread->done_sem, portMAX_DELAY);
    }

    if (thread->done_sem) {
        vSemaphoreDelete(thread->done_sem);
        thread->done_sem = NULL;
    }

    if (thread->malloced)
        free(thread);

    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_thread_sleep(unsigned msec)
{
    vTaskDelay(pdMS_TO_TICKS(msec));
    return PJ_SUCCESS;
}

PJ_DEF(void) pj_thread_check_stack(const char *file, int line)
{
    (void)file;
    (void)line;
}

/* ------------------------------------------------------------------------
 * Thread local storage
 */

PJ_DEF(pj_status_t) pj_thread_local_alloc(long *index)
{
    unsigned i;

    if (!index)
        return PJ_EINVAL;

    for (i = 0; i < PJ_THREAD_LOCAL_MAX; ++i) {
        if (!(tls_alloc_mask & (1u << i))) {
            tls_alloc_mask |= (1u << i);
            *index = (long)i;
            return PJ_SUCCESS;
        }
    }

    return PJ_ETOOMANY;
}

PJ_DEF(void) pj_thread_local_free(long index)
{
    if (index >= 0 && index < (long)PJ_THREAD_LOCAL_MAX)
        tls_alloc_mask &= ~(1u << (unsigned)index);
}

PJ_DEF(pj_status_t) pj_thread_local_set(long index, void *value)
{
    pj_thread_t *rec;

    if (index < 0 || index >= (long)PJ_THREAD_LOCAL_MAX)
        return PJ_EINVAL;

    rec = find_current_thread();
    if (!rec)
        return PJ_EINVAL;

    rec->tls[index] = value;
    return PJ_SUCCESS;
}

PJ_DEF(void *) pj_thread_local_get(long index)
{
    pj_thread_t *rec;

    if (index < 0 || index >= (long)PJ_THREAD_LOCAL_MAX)
        return NULL;

    rec = find_current_thread();
    if (!rec)
        return NULL;

    return rec->tls[index];
}

/* ------------------------------------------------------------------------
 * Atomic variables
 */

PJ_DEF(pj_status_t) pj_atomic_create(pj_pool_t *pool,
                                     pj_atomic_value_t initial,
                                     pj_atomic_t **atomic_var)
{
    pj_atomic_t *rec;
    pj_status_t rc;

    PJ_ASSERT_RETURN(atomic_var, PJ_EINVAL);

    rec = (pj_atomic_t *)calloc(1, sizeof(pj_atomic_t));
    if (!rec)
        return PJ_ENOMEM;

    rec->value = initial;
    rec->malloced = 1;

    rc = pj_mutex_create(pool, "atomic", PJ_MUTEX_SIMPLE, &rec->mutex);
    if (rc != PJ_SUCCESS) {
        free(rec);
        return rc;
    }

    *atomic_var = rec;
    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_atomic_destroy(pj_atomic_t *atomic_var)
{
    if (!atomic_var)
        return PJ_EINVAL;

    pj_mutex_destroy(atomic_var->mutex);
    if (atomic_var->malloced)
        free(atomic_var);
    return PJ_SUCCESS;
}

PJ_DEF(void) pj_atomic_set(pj_atomic_t *atomic_var, pj_atomic_value_t value)
{
    pj_mutex_lock(atomic_var->mutex);
    atomic_var->value = value;
    pj_mutex_unlock(atomic_var->mutex);
}

PJ_DEF(pj_atomic_value_t) pj_atomic_get(pj_atomic_t *atomic_var)
{
    pj_atomic_value_t v;

    pj_mutex_lock(atomic_var->mutex);
    v = atomic_var->value;
    pj_mutex_unlock(atomic_var->mutex);
    return v;
}

PJ_DEF(pj_atomic_value_t) pj_atomic_inc_and_get(pj_atomic_t *atomic_var)
{
    pj_atomic_value_t v;

    pj_mutex_lock(atomic_var->mutex);
    v = ++atomic_var->value;
    pj_mutex_unlock(atomic_var->mutex);
    return v;
}

PJ_DEF(void) pj_atomic_inc(pj_atomic_t *atomic_var)
{
    pj_atomic_inc_and_get(atomic_var);
}

PJ_DEF(pj_atomic_value_t) pj_atomic_dec_and_get(pj_atomic_t *atomic_var)
{
    pj_atomic_value_t v;

    pj_mutex_lock(atomic_var->mutex);
    v = --atomic_var->value;
    pj_mutex_unlock(atomic_var->mutex);
    return v;
}

PJ_DEF(void) pj_atomic_dec(pj_atomic_t *atomic_var)
{
    pj_atomic_dec_and_get(atomic_var);
}

PJ_DEF(pj_atomic_value_t) pj_atomic_add_and_get(pj_atomic_t *atomic_var,
                                                pj_atomic_value_t value)
{
    pj_atomic_value_t v;

    pj_mutex_lock(atomic_var->mutex);
    v = atomic_var->value += value;
    pj_mutex_unlock(atomic_var->mutex);
    return v;
}

PJ_DEF(void) pj_atomic_add(pj_atomic_t *atomic_var, pj_atomic_value_t value)
{
    pj_atomic_add_and_get(atomic_var, value);
}

/* ------------------------------------------------------------------------
 * Critical sections
 */

PJ_DEF(void) pj_enter_critical_section(void)
{
    if (critical_section)
        pj_mutex_lock(critical_section);
}

PJ_DEF(void) pj_leave_critical_section(void)
{
    if (critical_section)
        pj_mutex_unlock(critical_section);
}

/* ------------------------------------------------------------------------
 * Mutexes
 */

static pj_status_t pj_mutex_init(pj_mutex_t *rec, const char *name, int type)
{
    (void)type;

    rec->handle = xSemaphoreCreateRecursiveMutex();
    if (!rec->handle)
        return PJ_ENOMEM;

    if (name)
        pj_ansi_strncpy(rec->obj_name, name, PJ_ARRAY_SIZE(rec->obj_name));
    else
        pj_ansi_strcpy(rec->obj_name, "mutex");

    rec->owner = NULL;
    rec->nesting = 0;

    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_mutex_create(pj_pool_t *pool, const char *name,
                                    int type, pj_mutex_t **mutex)
{
    pj_mutex_t *rec;
    pj_status_t rc;

    PJ_ASSERT_RETURN(mutex, PJ_EINVAL);

    rec = (pj_mutex_t *)calloc(1, sizeof(pj_mutex_t));
    if (!rec)
        return PJ_ENOMEM;
    rec->malloced = 1;

    rc = pj_mutex_init(rec, name, type);
    if (rc != PJ_SUCCESS) {
        free(rec);
        return rc;
    }

    *mutex = rec;
    (void)pool;
    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_mutex_create_simple(pj_pool_t *pool, const char *name,
                                           pj_mutex_t **mutex)
{
    return pj_mutex_create(pool, name, PJ_MUTEX_SIMPLE, mutex);
}

PJ_DEF(pj_status_t) pj_mutex_create_recursive(pj_pool_t *pool, const char *name,
                                              pj_mutex_t **mutex)
{
    return pj_mutex_create(pool, name, PJ_MUTEX_RECURSE, mutex);
}

PJ_DEF(pj_status_t) pj_mutex_lock(pj_mutex_t *mutex)
{
    if (!mutex)
        return PJ_EINVAL;

    if (xSemaphoreTakeRecursive(mutex->handle, portMAX_DELAY) != pdTRUE)
        return PJ_EUNKNOWN;

    /* IMPORTANT: bump nesting BEFORE calling pj_thread_this().
     * pj_thread_this() may re-enter pj_mutex_lock() on this same mutex
     * (via the thread-list lookup), so nesting must already be non-zero
     * to keep that re-entrant call from recursing forever. */
    if (mutex->nesting == 0) {
        mutex->nesting = 1;
        mutex->owner = pj_thread_this();
    } else {
        mutex->nesting++;
    }

    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_mutex_unlock(pj_mutex_t *mutex)
{
    if (!mutex)
        return PJ_EINVAL;

    if (mutex->nesting > 0) {
        mutex->nesting--;
        if (mutex->nesting == 0)
            mutex->owner = NULL;
    }

    if (xSemaphoreGiveRecursive(mutex->handle) != pdTRUE)
        return PJ_EUNKNOWN;

    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_mutex_trylock(pj_mutex_t *mutex)
{
    if (!mutex)
        return PJ_EINVAL;

    if (xSemaphoreTakeRecursive(mutex->handle, 0) != pdTRUE)
        return PJ_EBUSY;

    if (mutex->nesting == 0) {
        mutex->nesting = 1;
        mutex->owner = pj_thread_this();
    } else {
        mutex->nesting++;
    }

    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_mutex_destroy(pj_mutex_t *mutex)
{
    if (!mutex)
        return PJ_EINVAL;

    if (mutex->handle) {
        vSemaphoreDelete(mutex->handle);
        mutex->handle = NULL;
    }

    if (mutex->malloced)
        free(mutex);

    return PJ_SUCCESS;
}

PJ_DEF(pj_bool_t) pj_mutex_is_locked(pj_mutex_t *mutex)
{
    if (!mutex)
        return PJ_FALSE;
    if (mutex->nesting > 0 && mutex->owner == pj_thread_this())
        return PJ_TRUE;
    return PJ_FALSE;
}

/* ------------------------------------------------------------------------
 * Read-write mutexes (generic implementation lives in os_rwmutex.c)
 */

/* ------------------------------------------------------------------------
 * Semaphores
 */

#if defined(PJ_HAS_SEMAPHORE) && PJ_HAS_SEMAPHORE != 0
PJ_DEF(pj_status_t) pj_sem_create(pj_pool_t *pool, const char *name,
                                  unsigned initial, unsigned max,
                                  pj_sem_t **sem)
{
    pj_sem_t *rec;

    PJ_ASSERT_RETURN(sem && max > 0, PJ_EINVAL);

    rec = (pj_sem_t *)calloc(1, sizeof(pj_sem_t));
    if (!rec)
        return PJ_ENOMEM;
    rec->malloced = 1;

    rec->sem = xSemaphoreCreateCounting(max, initial);
    if (!rec->sem) {
        free(rec);
        return PJ_ENOMEM;
    }

    if (name)
        pj_ansi_strncpy(rec->obj_name, name, PJ_ARRAY_SIZE(rec->obj_name));
    else
        pj_ansi_strcpy(rec->obj_name, "sem");

    *sem = rec;
    (void)pool;
    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_sem_wait(pj_sem_t *sem)
{
    if (!sem)
        return PJ_EINVAL;

    if (xSemaphoreTake(sem->sem, portMAX_DELAY) != pdTRUE)
        return PJ_EUNKNOWN;

    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_sem_trywait(pj_sem_t *sem)
{
    if (!sem)
        return PJ_EINVAL;

    if (xSemaphoreTake(sem->sem, 0) != pdTRUE)
        return PJ_EBUSY;

    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_sem_post(pj_sem_t *sem)
{
    if (!sem)
        return PJ_EINVAL;

    if (xSemaphoreGive(sem->sem) != pdTRUE)
        return PJ_EUNKNOWN;

    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_sem_destroy(pj_sem_t *sem)
{
    if (!sem)
        return PJ_EINVAL;

    if (sem->sem) {
        vSemaphoreDelete(sem->sem);
        sem->sem = NULL;
    }

    if (sem->malloced)
        free(sem);

    return PJ_SUCCESS;
}
#endif  /* PJ_HAS_SEMAPHORE */

/* ------------------------------------------------------------------------
 * Event objects
 */

#if defined(PJ_HAS_EVENT_OBJ) && PJ_HAS_EVENT_OBJ != 0
PJ_DEF(pj_status_t) pj_event_create(pj_pool_t *pool, const char *name,
                                    pj_bool_t manual_reset,
                                    pj_bool_t initial,
                                    pj_event_t **event)
{
    pj_event_t *rec;
    pj_status_t rc;

    PJ_ASSERT_RETURN(event, PJ_EINVAL);

    rec = (pj_event_t *)calloc(1, sizeof(pj_event_t));
    if (!rec)
        return PJ_ENOMEM;
    rec->malloced = 1;

    rec->manual_reset = manual_reset;
    rec->is_set = initial;

    rc = pj_mutex_create(NULL, "evmut", PJ_MUTEX_SIMPLE, &rec->mutex);
    if (rc != PJ_SUCCESS) {
        free(rec);
        return rc;
    }

    rec->sem = xSemaphoreCreateCounting(0x7fff, 0);
    if (!rec->sem) {
        pj_mutex_destroy(rec->mutex);
        free(rec);
        return PJ_ENOMEM;
    }

    (void)name;
    (void)pool;
    *event = rec;
    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_event_wait(pj_event_t *event)
{
    if (!event)
        return PJ_EINVAL;

    pj_mutex_lock(event->mutex);

    while (!event->is_set) {
        event->waiters++;
        pj_mutex_unlock(event->mutex);

        xSemaphoreTake(event->sem, portMAX_DELAY);

        pj_mutex_lock(event->mutex);
        event->waiters--;
    }

    if (!event->manual_reset)
        event->is_set = PJ_FALSE;

    pj_mutex_unlock(event->mutex);
    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_event_trywait(pj_event_t *event)
{
    pj_status_t status = PJ_SUCCESS;

    if (!event)
        return PJ_EINVAL;

    pj_mutex_lock(event->mutex);

    if (event->is_set) {
        if (!event->manual_reset)
            event->is_set = PJ_FALSE;
    } else {
        status = PJ_EBUSY;
    }

    pj_mutex_unlock(event->mutex);
    return status;
}

PJ_DEF(pj_status_t) pj_event_set(pj_event_t *event)
{
    if (!event)
        return PJ_EINVAL;

    pj_mutex_lock(event->mutex);

    event->is_set = PJ_TRUE;

    if (event->manual_reset) {
        /* Release all current waiters. */
        while (event->waiters > 0)
            xSemaphoreGive(event->sem);
    } else if (!event->waiters) {
        /* No waiter yet: leave the state set so the next wait() returns. */
    }

    pj_mutex_unlock(event->mutex);
    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_event_pulse(pj_event_t *event)
{
    if (!event)
        return PJ_EINVAL;

    pj_mutex_lock(event->mutex);

    if (event->waiters > 0) {
        xSemaphoreGive(event->sem);
    } else {
        event->is_set = PJ_TRUE;
    }

    pj_mutex_unlock(event->mutex);
    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_event_reset(pj_event_t *event)
{
    if (!event)
        return PJ_EINVAL;

    pj_mutex_lock(event->mutex);
    event->is_set = PJ_FALSE;
    pj_mutex_unlock(event->mutex);
    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_event_destroy(pj_event_t *event)
{
    if (!event)
        return PJ_EINVAL;

    pj_mutex_destroy(event->mutex);
    if (event->sem)
        vSemaphoreDelete(event->sem);

    if (event->malloced)
        free(event);

    return PJ_SUCCESS;
}
#endif  /* PJ_HAS_EVENT_OBJ */

/* ------------------------------------------------------------------------
 * Barrier
 */

PJ_DEF(pj_status_t) pj_barrier_create(pj_pool_t *pool, unsigned count,
                                      pj_barrier_t **barrier)
{
    pj_barrier_t *rec;
    pj_status_t rc;

    PJ_ASSERT_RETURN(barrier && count > 0, PJ_EINVAL);

    rec = (pj_barrier_t *)calloc(1, sizeof(pj_barrier_t));
    if (!rec)
        return PJ_ENOMEM;
    rec->malloced = 1;

    rc = pj_mutex_create(NULL, "barrier", PJ_MUTEX_SIMPLE, &rec->mutex);
    if (rc != PJ_SUCCESS) {
        free(rec);
        return rc;
    }

    rec->sem = xSemaphoreCreateCounting(count, 0);
    if (!rec->sem) {
        pj_mutex_destroy(rec->mutex);
        free(rec);
        return PJ_ENOMEM;
    }

    rec->trip_count = count;
    rec->count = 0;

    *barrier = rec;
    (void)pool;
    return PJ_SUCCESS;
}

PJ_DEF(pj_int32_t) pj_barrier_wait(pj_barrier_t *barrier, pj_uint32_t flags)
{
    unsigned i;

    (void)flags;

    if (!barrier)
        return -1;

    pj_mutex_lock(barrier->mutex);
    barrier->count++;
    if (barrier->count >= barrier->trip_count) {
        barrier->count = 0;
        for (i = 0; i < barrier->trip_count; ++i)
            xSemaphoreGive(barrier->sem);
        pj_mutex_unlock(barrier->mutex);
        return 0;
    }
    pj_mutex_unlock(barrier->mutex);

    xSemaphoreTake(barrier->sem, portMAX_DELAY);
    return 1;
}

PJ_DEF(pj_status_t) pj_barrier_destroy(pj_barrier_t *barrier)
{
    if (!barrier)
        return PJ_EINVAL;

    pj_mutex_destroy(barrier->mutex);
    if (barrier->sem)
        vSemaphoreDelete(barrier->sem);

    if (barrier->malloced)
        free(barrier);

    return PJ_SUCCESS;
}

/* ------------------------------------------------------------------------
 * Terminal / misc
 */

PJ_DEF(pj_status_t) pj_term_set_color(pj_color_t color)
{
    (void)color;
    return PJ_SUCCESS;
}

PJ_DEF(pj_color_t) pj_term_get_color(void)
{
    return 0;
}

PJ_DEF(int) pj_run_app(pj_main_func_ptr main_func, int argc, char *argv[],
                       unsigned flags)
{
    int rc;

    (void)flags;
    rc = (*main_func)(argc, argv);
    pj_shutdown();
    return rc;
}

PJ_DEF(pj_status_t) pj_set_cloexec_flag(int fd)
{
    (void)fd;
    return PJ_ENOTSUP;
}
