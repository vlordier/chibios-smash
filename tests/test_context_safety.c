/*
 * SMASH test: Execution context safety
 *
 * Tests that API context violations are caught:
 *   1. Blocking (mutex lock) inside a chSysLock() critical section
 *   2. Blocking (sem wait) inside an ISR handler
 *   3. Unbalanced chSysLock / chSysUnlock (extra unlock)
 *   4. Unbalanced ISR_ENTER / ISR_EXIT (extra exit)
 *   5. Clean scenario: proper lock/unlock balance, no blocking in bad context
 *
 * Each "bad" scenario is expected to produce exactly 1 violation.
 * The "clean" scenario is expected to produce 0 violations and 0 deadlocks.
 */

#include "smash.h"

/* Helper: run a scenario and check results. */
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

/* Scenario 1: mutex lock inside chSysLock() critical section.
 *
 * T0: SYS_LOCK -> MUTEX_LOCK (violation!) -> ...
 *
 * Blocking inside a locked section is a ChibiOS API violation.
 * chMtxLock is not an S-class function.
 */
static smash_scenario_t blocking_in_sys_lock(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name          = "Mutex lock inside SYS_LOCK";
    sc.thread_count  = 1;
    sc.resource_count = 1;
    sc.priorities[0] = 10;
    sc.res_types[0]  = RES_MUTEX;

    sc.steps[0][0] = (smash_action_t){ACT_SYS_LOCK,    SMASH_NO_RESOURCE};
    sc.steps[0][1] = (smash_action_t){ACT_MUTEX_LOCK,  0};  /* violation */
    sc.steps[0][2] = (smash_action_t){ACT_MUTEX_UNLOCK,0};
    sc.steps[0][3] = (smash_action_t){ACT_SYS_UNLOCK,  SMASH_NO_RESOURCE};
    sc.step_count[0] = 4;

    return sc;
}

/* Scenario 2: semaphore wait inside ISR handler.
 *
 * T0: ISR_ENTER -> SEM_WAIT (violation!) -> ...
 *
 * Blocking inside an ISR is a hard ChibiOS constraint: ISR handlers
 * may not call any normal-context (blocking) APIs.
 */
static smash_scenario_t blocking_in_isr(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name           = "Sem wait inside ISR";
    sc.thread_count   = 1;
    sc.resource_count = 1;
    sc.priorities[0]  = 10;
    sc.res_types[0]   = RES_SEMAPHORE;
    sc.sem_init[0]    = 1;  /* pre-signaled so it won't block — still violates */

    sc.steps[0][0] = (smash_action_t){ACT_ISR_ENTER,  SMASH_NO_RESOURCE};
    sc.steps[0][1] = (smash_action_t){ACT_SEM_WAIT,   0};  /* violation */
    sc.steps[0][2] = (smash_action_t){ACT_ISR_EXIT,   SMASH_NO_RESOURCE};
    sc.step_count[0] = 3;

    return sc;
}

/* Scenario 3: unbalanced chSysUnlock (no matching lock).
 *
 * T0: SYS_UNLOCK (violation! — depth already 0)
 */
static smash_scenario_t unbalanced_sys_unlock(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name          = "Unbalanced SYS_UNLOCK";
    sc.thread_count  = 1;
    sc.resource_count = 0;
    sc.priorities[0] = 10;

    sc.steps[0][0] = (smash_action_t){ACT_SYS_UNLOCK, SMASH_NO_RESOURCE};
    sc.step_count[0] = 1;

    return sc;
}

/* Scenario 4: unbalanced ISR_EXIT (no matching enter).
 *
 * T0: ISR_EXIT (violation! — depth already 0)
 */
static smash_scenario_t unbalanced_isr_exit(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name          = "Unbalanced ISR_EXIT";
    sc.thread_count  = 1;
    sc.resource_count = 0;
    sc.priorities[0] = 10;

    sc.steps[0][0] = (smash_action_t){ACT_ISR_EXIT, SMASH_NO_RESOURCE};
    sc.step_count[0] = 1;

    return sc;
}

/* Scenario 5: clean usage — proper lock/unlock balance, no blocking violations.
 *
 * T0: SYS_LOCK -> SYS_UNLOCK -> done      (no blocking, balanced)
 * T1: ISR_ENTER -> ISR_EXIT -> done        (no blocking, balanced)
 *
 * Expected: 0 violations, 0 deadlocks.
 */
static smash_scenario_t clean_context_usage(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name          = "Clean context transitions";
    sc.thread_count  = 2;
    sc.resource_count = 0;
    sc.priorities[0] = 10;
    sc.priorities[1] = 10;

    sc.steps[0][0] = (smash_action_t){ACT_SYS_LOCK,   SMASH_NO_RESOURCE};
    sc.steps[0][1] = (smash_action_t){ACT_SYS_UNLOCK, SMASH_NO_RESOURCE};
    sc.step_count[0] = 2;

    sc.steps[1][0] = (smash_action_t){ACT_ISR_ENTER,  SMASH_NO_RESOURCE};
    sc.steps[1][1] = (smash_action_t){ACT_ISR_EXIT,   SMASH_NO_RESOURCE};
    sc.step_count[1] = 2;

    return sc;
}

/* Scenario 6: unbalanced SYS_LOCK at thread exit.
 *
 * T0: SYS_LOCK -> ... exits without SYS_UNLOCK
 *
 * smash_check_context_safety fires when T0 reaches THREAD_DONE
 * with sys_lock_depth > 0.
 */
static smash_scenario_t sys_lock_not_released_on_exit(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name          = "SYS_LOCK not released before thread exit";
    sc.thread_count  = 1;
    sc.resource_count = 0;
    sc.priorities[0] = 10;

    sc.steps[0][0] = (smash_action_t){ACT_SYS_LOCK, SMASH_NO_RESOURCE};
    /* Thread ends without SYS_UNLOCK */
    sc.step_count[0] = 1;

    return sc;
}

int main(void) {

    printf("SMASH — Execution context safety tests\n");

    int failed = 0;

    /* Each "bad" scenario produces engine->failed=true → 1 violation. */
    failed += run("1. Mutex lock in SYS_LOCK context",
                  blocking_in_sys_lock(), 1, 0);

    failed += run("2. Sem wait in ISR context",
                  blocking_in_isr(), 1, 0);

    failed += run("3. Unbalanced SYS_UNLOCK",
                  unbalanced_sys_unlock(), 1, 0);

    failed += run("4. Unbalanced ISR_EXIT",
                  unbalanced_isr_exit(), 1, 0);

    failed += run("5. Clean context transitions",
                  clean_context_usage(), 0, 0);

    failed += run("6. SYS_LOCK not released on exit",
                  sys_lock_not_released_on_exit(), 1, 0);

    printf("\n========================================\n");
    if (failed == 0) {
        printf("All context safety tests PASSED\n");
    } else {
        printf("%d context safety test(s) FAILED\n", failed);
    }
    printf("========================================\n");

    return failed ? 1 : 0;
}
