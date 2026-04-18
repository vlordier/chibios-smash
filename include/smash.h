/*
 * SMASH - Stateless Model-checking And Schedule Hunting
 * A concurrency verification framework for ChibiOS
 *
 * Copyright 2026 - MIT License
 */

#ifndef SMASH_H
#define SMASH_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

#define SMASH_VERSION        "1.1.0"

#define SMASH_MAX_THREADS    16
#define SMASH_MAX_RESOURCES  16
#define SMASH_MAX_STEPS      255   /* max steps per thread (fits in uint8_t PC) */
#define SMASH_MAX_TRACE      65536
#define SMASH_MAX_DEPTH      512
#define SMASH_MAX_BACKTRACK  4096
#define SMASH_MAX_STATES     65536
/* Open-addressing hash table for visited state hashes; ~50% load factor. */
#define SMASH_STATE_HT_SIZE  (SMASH_MAX_STATES * 2U)
#define SMASH_MAX_WAITERS    SMASH_MAX_THREADS

/* Sentinel for "no resource applicable" in actions and trace events. */
#define SMASH_NO_RESOURCE    (-1)

/* Compile-time safety guards. */
_Static_assert(SMASH_MAX_THREADS <= 32,
    "SMASH_MAX_THREADS must fit in a uint32_t persistent-set bitmask");
_Static_assert(SMASH_MAX_STEPS <= 255,
    "SMASH_MAX_STEPS must fit in a uint8_t thread-PC snapshot");
_Static_assert((SMASH_STATE_HT_SIZE & (SMASH_STATE_HT_SIZE - 1U)) == 0,
    "SMASH_STATE_HT_SIZE must be a power of two for open-addressing");
_Static_assert(SMASH_MAX_DEPTH <= SMASH_MAX_TRACE,
    "Save-stack depth must not exceed trace event buffer size");

/*===========================================================================*/
/* Thread model                                                              */
/*===========================================================================*/

typedef enum {
    THREAD_READY,           /* runnable, waiting to be scheduled */
    THREAD_BLOCKED_MUTEX,   /* waiting for a mutex (chMtxLock) */
    THREAD_BLOCKED_SEM,     /* waiting for a semaphore (chSemWait) */
    THREAD_DONE             /* all steps completed */
} smash_thread_state_t;

typedef struct {
    int                   id;
    int                   base_priority;  /* immutable; set at init */
    int                   priority;       /* current, may be boosted by inheritance */
    smash_thread_state_t  state;
    int                   pc;             /* step index within scenario */
    int                   blocked_on;     /* resource id, -1 if none */
    /* LIFO owned-mutex stack (mirrors ChibiOS mtxlist).
     * Matches chmtx.c:378 chDbgAssert(currtp->mtxlist == mp). */
    int                   owned_mutex_stack[SMASH_MAX_RESOURCES];
    int                   owned_mutex_count;
} smash_thread_t;

/*===========================================================================*/
/* Resource model (mutexes, semaphores)                                      */
/*===========================================================================*/

typedef enum {
    RES_MUTEX,
    RES_SEMAPHORE
} smash_resource_type_t;

typedef struct {
    smash_resource_type_t type;
    int                   id;
    /* Mutex fields */
    int                   owner;        /* thread id, -1 if free */
    /* Semaphore fields */
    int                   count;
    /* Wait queue (shared) */
    int                   waiters[SMASH_MAX_WAITERS];
    int                   waiter_count;
} smash_resource_t;

/*===========================================================================*/
/* Actions (what threads do)                                                 */
/*===========================================================================*/

typedef enum {
    ACT_NOP,
    ACT_MUTEX_LOCK,
    ACT_MUTEX_UNLOCK,
    ACT_SEM_WAIT,
    ACT_SEM_SIGNAL,
    ACT_YIELD,
    ACT_DONE
} smash_action_type_t;

typedef struct {
    smash_action_type_t type;
    int                 resource_id;    /* which mutex/sem, -1 if N/A */
} smash_action_t;

/*===========================================================================*/
/* Scenario (test definition)                                                */
/*===========================================================================*/

typedef struct {
    const char   *name;
    int           thread_count;
    int           resource_count;
    int           priorities[SMASH_MAX_THREADS];
    smash_action_t steps[SMASH_MAX_THREADS][SMASH_MAX_STEPS];
    int           step_count[SMASH_MAX_THREADS];
    smash_resource_type_t res_types[SMASH_MAX_RESOURCES];
    int           sem_init[SMASH_MAX_RESOURCES]; /* initial sem count */
} smash_scenario_t;

/*===========================================================================*/
/* Trace events                                                              */
/*===========================================================================*/

typedef enum {
    EVT_SCHEDULE,
    EVT_MUTEX_LOCK_ATTEMPT,
    EVT_MUTEX_LOCK_ACQUIRED,
    EVT_MUTEX_LOCK_BLOCKED,
    EVT_MUTEX_UNLOCK,
    EVT_SEM_WAIT,
    EVT_SEM_WAIT_BLOCKED,
    EVT_SEM_SIGNAL,
    EVT_SEM_WAKEUP,
    EVT_YIELD,
    EVT_THREAD_DONE,
    EVT_DEADLOCK,
    EVT_INVARIANT_FAIL
} smash_event_type_t;

typedef struct {
    uint32_t           step;
    smash_event_type_t type;
    int                tid;
    int                resource_id;
    int                arg;     /* extra info: priority, count, etc. */
} smash_trace_event_t;

typedef struct {
    smash_trace_event_t events[SMASH_MAX_TRACE];
    int                 count;
    bool                truncated;  /* events[] was full; later events were dropped */
    int                 schedule[SMASH_MAX_DEPTH]; /* thread chosen at each step */
    int                 schedule_len;
} smash_trace_t;

/*===========================================================================*/
/* Execution state (snapshot for hashing/comparison)                         */
/*===========================================================================*/

typedef struct {
    uint8_t  thread_states[SMASH_MAX_THREADS];
    uint8_t  thread_pcs[SMASH_MAX_THREADS];
    uint8_t  thread_priorities[SMASH_MAX_THREADS]; /* boosted priority matters */
    int8_t   mutex_owners[SMASH_MAX_RESOURCES];
    int8_t   sem_counts[SMASH_MAX_RESOURCES];
    int      thread_count;
    int      resource_count;
} smash_state_snapshot_t;

/*===========================================================================*/
/* Engine context                                                            */
/*===========================================================================*/

typedef struct {
    /* Scenario */
    const smash_scenario_t *scenario;

    /* Live state */
    smash_thread_t    threads[SMASH_MAX_THREADS];
    smash_resource_t  resources[SMASH_MAX_RESOURCES];

    /* Trace */
    smash_trace_t     trace;

    /* Statistics */
    uint64_t          states_explored;
    uint64_t          interleavings_explored;
    uint64_t          interleavings_pruned;
    uint64_t          deadlocks_found;
    uint64_t          invariant_violations;
    uint64_t          max_depth_reached;

    /* DPOR */
    bool              dpor_enabled;

    /* Exploration limits */
    int               max_depth;
    uint64_t          max_interleavings;

    /* State table: open-addressing hash table (0 = empty slot).
     * FNV-1a with a non-zero seed never produces 0, so 0 is a safe sentinel. */
    uint64_t          state_ht[SMASH_STATE_HT_SIZE];
    int               state_hash_count;

    /* Global step counter */
    uint32_t          step_counter;

    /* Failure info */
    bool              failed;
    char              fail_msg[256];
} smash_engine_t;

/*===========================================================================*/
/* Invariant checker callback                                                */
/*===========================================================================*/

typedef bool (*smash_invariant_fn)(const smash_engine_t *engine,
                                   char *msg, int msg_len);

/*===========================================================================*/
/* Engine API (engine.c)                                                     */
/*===========================================================================*/

/* Validate scenario bounds before init. Returns true on success; writes
 * a human-readable error into msg on failure. */
bool smash_scenario_validate(const smash_scenario_t *scenario,
                              char *msg, size_t msg_len);

void smash_engine_init(smash_engine_t *engine, const smash_scenario_t *scenario);
void smash_engine_reset(smash_engine_t *engine);
int  smash_collect_runnable(const smash_engine_t *engine, int *out, int max);
bool smash_all_done(const smash_engine_t *engine);
bool smash_execute_step(smash_engine_t *engine, int tid);
bool smash_run_schedule(smash_engine_t *engine, const int *schedule, int len);
smash_state_snapshot_t smash_capture_state(const smash_engine_t *engine);

/*===========================================================================*/
/* ChibiOS model API (chibios_model.c)                                      */
/*===========================================================================*/

bool smash_mutex_lock(smash_engine_t *engine, int tid, int res_id);
void smash_mutex_unlock(smash_engine_t *engine, int tid, int res_id);
bool smash_sem_wait(smash_engine_t *engine, int tid, int res_id);
void smash_sem_signal(smash_engine_t *engine, int tid, int res_id);

/*===========================================================================*/
/* DPOR API (dpor.c)                                                        */
/*===========================================================================*/

typedef struct {
    int depth;
    int tid;
} smash_backtrack_t;

typedef struct {
    /* Backtracking set */
    smash_backtrack_t backtrack[SMASH_MAX_BACKTRACK];
    int               backtrack_count;
    /* Execution history for dependency analysis */
    struct {
        int tid;
        int resource_id;
        smash_action_type_t type;
    } history[SMASH_MAX_DEPTH];
    int history_len;
} smash_dpor_t;

void smash_dpor_init(smash_dpor_t *dpor);
void smash_dpor_reset(smash_dpor_t *dpor);   /* clear history & backtrack set */
bool smash_dpor_dependent(smash_action_type_t a_type, int a_res,
                          smash_action_type_t b_type, int b_res);
void smash_dpor_record(smash_dpor_t *dpor, int tid, int resource_id,
                       smash_action_type_t type);
/* NOTE: smash_dpor_analyze is NOT called by explore_dfs; the explorer uses
 * persistent-sets DPOR inline via compute_persistent_set().  This function
 * is available for external post-hoc analysis. */
void smash_dpor_analyze(smash_dpor_t *dpor, const smash_engine_t *engine);
bool smash_dpor_next_backtrack(smash_dpor_t *dpor, int *out_depth, int *out_tid);

/*===========================================================================*/
/* Trace API (trace.c)                                                       */
/*===========================================================================*/

void smash_trace_init(smash_trace_t *trace);
void smash_trace_log(smash_trace_t *trace, uint32_t step,
                     smash_event_type_t type, int tid, int res_id, int arg);
void smash_trace_dump(const smash_trace_t *trace, FILE *out);
void smash_trace_dump_json(const smash_trace_t *trace, FILE *out);
int  smash_trace_minimize(smash_trace_t *trace, smash_engine_t *engine,
                          const smash_scenario_t *scenario);

/*===========================================================================*/
/* State API (state.c)                                                       */
/*===========================================================================*/

uint64_t smash_state_hash(const smash_state_snapshot_t *snap);
bool     smash_state_equal(const smash_state_snapshot_t *a,
                           const smash_state_snapshot_t *b);
bool     smash_state_visited(smash_engine_t *engine, uint64_t hash);
/* Returns false if the hash table is full and the hash could not be stored. */
bool     smash_state_mark_visited(smash_engine_t *engine, uint64_t hash);

/*===========================================================================*/
/* Spec / invariant API (spec.c)                                             */
/*===========================================================================*/

bool smash_check_no_deadlock(const smash_engine_t *engine,
                             char *msg, int msg_len);
bool smash_check_mutex_integrity(const smash_engine_t *engine,
                                 char *msg, int msg_len);
bool smash_check_sem_integrity(const smash_engine_t *engine,
                               char *msg, int msg_len);
bool smash_check_priority_inversion(const smash_engine_t *engine,
                                    char *msg, int msg_len);
/* Verify owned_mutex_stack vs actual mutex ownership for every thread. */
bool smash_check_owned_mutex_integrity(const smash_engine_t *engine,
                                       char *msg, int msg_len);
/* Detect circular wait (wait-for graph cycle) - the fundamental deadlock mechanism. */
bool smash_check_circular_wait(const smash_engine_t *engine,
                               char *msg, int msg_len);
bool smash_check_all(const smash_engine_t *engine, char *msg, int msg_len);

/*===========================================================================*/
/* SMT export API (smt.c)                                                    */
/*===========================================================================*/

void smash_smt_export(const smash_trace_t *trace,
                      const smash_scenario_t *scenario, FILE *out);

/*===========================================================================*/
/* Explorer (top-level driver)                                               */
/*===========================================================================*/

typedef struct {
    bool     enable_dpor;
    bool     enable_state_caching;
    int      max_depth;
    uint64_t max_interleavings;
    bool     stop_on_first_bug;
    bool     verbose;
} smash_config_t;

typedef struct {
    uint64_t interleavings;
    uint64_t states;
    uint64_t cache_pruned;  /* states skipped by hash-based state caching */
    uint64_t dpor_pruned;   /* thread choices skipped by persistent-sets DPOR */
    uint64_t deadlocks;
    uint64_t violations;
    double   elapsed_secs;
    smash_trace_t *failing_trace;  /* first failure, or NULL */
    uint64_t max_depth_reached;     /* deepest DFS depth actually explored */
} smash_result_t;

/* Default config: DPOR + state caching enabled, liberal limits, first-bug stop. */
#define SMASH_CONFIG_DEFAULT ((smash_config_t){ \
    .enable_dpor          = true,                \
    .enable_state_caching = true,                \
    .max_depth            = SMASH_MAX_DEPTH,     \
    .max_interleavings    = UINT64_MAX,           \
    .stop_on_first_bug    = true,                \
    .verbose              = false,               \
})

smash_result_t smash_explore(const smash_scenario_t *scenario,
                             const smash_config_t *config);

void smash_result_print(const smash_result_t *result, FILE *out);

/* Free heap-allocated fields inside a result (the failing_trace pointer).
 * Safe to call with a zeroed or already-freed result. */
void smash_result_free(smash_result_t *result);

#endif /* SMASH_H */
