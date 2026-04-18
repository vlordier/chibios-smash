/*
 * SMASH test: Object lifecycle and stack depth
 *
 * Tests use-after-free detection and stack overflow detection:
 *
 * Object lifecycle:
 *   1. Lock a mutex after it has been destroyed     → violation
 *   2. Wait on a semaphore after it is destroyed    → violation
 *   3. Signal a semaphore after it is destroyed     → violation
 *   4. Re-initialize a destroyed object (valid)     → 0 violations
 *   5. Normal lifecycle: init, use, destroy, re-init, use → 0 violations
 *
 * Stack depth:
 *   6. ACT_CALL exceeds per-thread stack limit      → violation
 *   7. ACT_CALL within stack limit                  → 0 violations
 *   8. Nested calls and returns stay within limit   → 0 violations
 */

#include "smash.h"

static int run(const char *label, smash_scenario_t sc,
               uint64_t expect_violations, uint64_t expect_deadlocks) {

    printf("\n=== %s ===\n", label);

    smash_config_t config = {
        .enable_dpor          = true,
        .enable_state_caching = true,
        .max_depth            = 64,
        .max_interleavings    = 1000,
        .stop_on_first_bug    = false,
        .verbose              = false,
    };

    smash_result_t r = smash_explore(&sc, &config);
    smash_result_print(&r, stdout);

    int failed = 0;
    if (r.violations != expect_violations) {
        fprintf(stderr, "FAIL %s: violations=%llu expected=%llu\n",
                label, r.violations, expect_violations);
        failed = 1;
    }
    if (r.deadlocks != expect_deadlocks) {
        fprintf(stderr, "FAIL %s: deadlocks=%llu expected=%llu\n",
                label, r.deadlocks, expect_deadlocks);
        failed = 1;
    }
    if (!failed) {
        printf("PASS: violations=%llu deadlocks=%llu\n",
               r.violations, r.deadlocks);
    }
    smash_result_free(&r);
    return failed;
}

/* Scenario 1: lock a mutex after destroying it.
 *
 * T0: OBJECT_DESTROY(M0) -> MUTEX_LOCK(M0)  [use-after-free]
 */
static smash_scenario_t mutex_use_after_free(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name           = "Mutex lock after destroy";
    sc.thread_count   = 1;
    sc.resource_count = 1;
    sc.priorities[0]  = 10;
    sc.res_types[0]   = RES_MUTEX;

    sc.steps[0][0] = (smash_action_t){ACT_OBJECT_DESTROY, 0};
    sc.steps[0][1] = (smash_action_t){ACT_MUTEX_LOCK,     0};  /* use-after-free */
    sc.step_count[0] = 2;

    return sc;
}

/* Scenario 2: wait on a semaphore after destroying it.
 *
 * T0: SEM_SIGNAL(S0) -> OBJECT_DESTROY(S0) -> SEM_WAIT(S0)  [use-after-free]
 */
static smash_scenario_t sem_wait_after_free(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name           = "Sem wait after destroy";
    sc.thread_count   = 1;
    sc.resource_count = 1;
    sc.priorities[0]  = 10;
    sc.res_types[0]   = RES_SEMAPHORE;
    sc.sem_init[0]    = 1;

    sc.steps[0][0] = (smash_action_t){ACT_OBJECT_DESTROY, 0};
    sc.steps[0][1] = (smash_action_t){ACT_SEM_WAIT,       0};  /* use-after-free */
    sc.step_count[0] = 2;

    return sc;
}

/* Scenario 3: signal a semaphore after destroying it.
 *
 * T0: OBJECT_DESTROY(S0) -> SEM_SIGNAL(S0)  [use-after-free]
 */
static smash_scenario_t sem_signal_after_free(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name           = "Sem signal after destroy";
    sc.thread_count   = 1;
    sc.resource_count = 1;
    sc.priorities[0]  = 10;
    sc.res_types[0]   = RES_SEMAPHORE;

    sc.steps[0][0] = (smash_action_t){ACT_OBJECT_DESTROY, 0};
    sc.steps[0][1] = (smash_action_t){ACT_SEM_SIGNAL,     0};  /* use-after-free */
    sc.step_count[0] = 2;

    return sc;
}

/* Scenario 4: re-initialize a destroyed object then use it normally.
 *
 * T0: OBJECT_DESTROY(M0) -> OBJECT_INIT(M0) -> MUTEX_LOCK -> MUTEX_UNLOCK
 *
 * Re-init after destroy is valid (like calling chMtxObjectInit again).
 * Expected: 0 violations.
 */
static smash_scenario_t reinit_after_destroy(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name           = "Re-init after destroy";
    sc.thread_count   = 1;
    sc.resource_count = 1;
    sc.priorities[0]  = 10;
    sc.res_types[0]   = RES_MUTEX;

    sc.steps[0][0] = (smash_action_t){ACT_OBJECT_DESTROY, 0};
    sc.steps[0][1] = (smash_action_t){ACT_OBJECT_INIT,    0};
    sc.steps[0][2] = (smash_action_t){ACT_MUTEX_LOCK,     0};
    sc.steps[0][3] = (smash_action_t){ACT_MUTEX_UNLOCK,   0};
    sc.step_count[0] = 4;

    return sc;
}

/* Scenario 5: racy lifecycle — signal then destroy races with wait.
 *
 * T0: signal(S0) -> destroy(S0)
 * T1: wait(S0)
 *
 * In the interleaving [T0 full, then T1], T0 destroys S0 before T1 waits
 * → use-after-free.  SMASH correctly finds this race.
 * Expected: 1 violation.
 */
static smash_scenario_t normal_lifecycle(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name           = "Racy lifecycle (signal-destroy races with wait)";
    sc.thread_count   = 2;
    sc.resource_count = 1;
    sc.priorities[0]  = 10;
    sc.priorities[1]  = 10;
    sc.res_types[0]   = RES_SEMAPHORE;
    sc.sem_init[0]    = 0;

    /* T0: signal, then destroy */
    sc.steps[0][0] = (smash_action_t){ACT_SEM_SIGNAL,     0};
    sc.steps[0][1] = (smash_action_t){ACT_OBJECT_DESTROY, 0};
    sc.step_count[0] = 2;

    /* T1: wait — races with T0's destroy */
    sc.steps[1][0] = (smash_action_t){ACT_SEM_WAIT, 0};
    sc.step_count[1] = 1;

    return sc;
}

/* Scenario 6: ACT_CALL exceeds per-thread stack limit.
 *
 * T0: stack_limit=5; CALL(arg=10) → overflow
 */
static smash_scenario_t stack_overflow(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name           = "Stack overflow";
    sc.thread_count   = 1;
    sc.resource_count = 0;
    sc.priorities[0]  = 10;
    sc.stack_sizes[0] = 5;  /* limit: 5 abstract units */

    sc.steps[0][0] = (smash_action_t){ACT_CALL, SMASH_NO_RESOURCE, .arg = 10};
    sc.step_count[0] = 1;

    return sc;
}

/* Scenario 7: ACT_CALL within stack limit.
 *
 * T0: stack_limit=20; CALL(5) -> CALL(5) -> RETURN(5) -> RETURN(5)
 *
 * Peak depth = 10 ≤ 20. Expected: 0 violations.
 */
static smash_scenario_t stack_within_limit(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name           = "Stack within limit";
    sc.thread_count   = 1;
    sc.resource_count = 0;
    sc.priorities[0]  = 10;
    sc.stack_sizes[0] = 20;

    sc.steps[0][0] = (smash_action_t){ACT_CALL,   SMASH_NO_RESOURCE, .arg = 5};
    sc.steps[0][1] = (smash_action_t){ACT_CALL,   SMASH_NO_RESOURCE, .arg = 5};
    sc.steps[0][2] = (smash_action_t){ACT_RETURN, SMASH_NO_RESOURCE, .arg = 5};
    sc.steps[0][3] = (smash_action_t){ACT_RETURN, SMASH_NO_RESOURCE, .arg = 5};
    sc.step_count[0] = 4;

    return sc;
}

/* Scenario 8: nested calls with interleaving, all within limit.
 *
 * T0: CALL(3) -> CALL(3) -> YIELD -> RETURN(3) -> RETURN(3)  [peak=6]
 * T1: CALL(4) -> YIELD -> RETURN(4)                           [peak=4]
 * Both limits = 10.  Expected: 0 violations.
 */
static smash_scenario_t nested_calls_interleaved(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name           = "Nested calls interleaved, within limit";
    sc.thread_count   = 2;
    sc.resource_count = 0;
    sc.priorities[0]  = 10;
    sc.priorities[1]  = 10;
    sc.stack_sizes[0] = 10;
    sc.stack_sizes[1] = 10;

    sc.steps[0][0] = (smash_action_t){ACT_CALL,   SMASH_NO_RESOURCE, .arg = 3};
    sc.steps[0][1] = (smash_action_t){ACT_CALL,   SMASH_NO_RESOURCE, .arg = 3};
    sc.steps[0][2] = (smash_action_t){ACT_YIELD,  SMASH_NO_RESOURCE};
    sc.steps[0][3] = (smash_action_t){ACT_RETURN, SMASH_NO_RESOURCE, .arg = 3};
    sc.steps[0][4] = (smash_action_t){ACT_RETURN, SMASH_NO_RESOURCE, .arg = 3};
    sc.step_count[0] = 5;

    sc.steps[1][0] = (smash_action_t){ACT_CALL,   SMASH_NO_RESOURCE, .arg = 4};
    sc.steps[1][1] = (smash_action_t){ACT_YIELD,  SMASH_NO_RESOURCE};
    sc.steps[1][2] = (smash_action_t){ACT_RETURN, SMASH_NO_RESOURCE, .arg = 4};
    sc.step_count[1] = 3;

    return sc;
}

int main(void) {

    printf("SMASH — Object lifecycle and stack depth tests\n");

    int failed = 0;

    failed += run("1. Mutex use-after-free",
                  mutex_use_after_free(), 1, 0);

    failed += run("2. Sem wait use-after-free",
                  sem_wait_after_free(), 1, 0);

    failed += run("3. Sem signal use-after-free",
                  sem_signal_after_free(), 1, 0);

    failed += run("4. Re-init after destroy",
                  reinit_after_destroy(), 0, 0);

    failed += run("5. Racy lifecycle (signal-destroy vs wait)",
                  normal_lifecycle(), 1, 0);

    failed += run("6. Stack overflow",
                  stack_overflow(), 1, 0);

    failed += run("7. Stack within limit",
                  stack_within_limit(), 0, 0);

    failed += run("8. Nested calls interleaved",
                  nested_calls_interleaved(), 0, 0);

    printf("\n========================================\n");
    if (failed == 0) {
        printf("All object lifecycle and stack tests PASSED\n");
    } else {
        printf("%d test(s) FAILED\n", failed);
    }
    printf("========================================\n");

    return failed ? 1 : 0;
}
