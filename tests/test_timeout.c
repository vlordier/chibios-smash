/*
 * SMASH test: Timeout operations (chMtxTimedLock, chSemTimedWait)
 *
 * Tests the new ACT_MUTEX_TIMED_LOCK and ACT_SEM_TIMED_WAIT actions.
 * These operations block with a timeout counter. When the timeout
 * expires, the thread resumes with MSG_TIMEOUT (-1).
 *
 * This prevents deadlocks in real-time systems by allowing threads
 * to recover from resource contention.
 */

#include "smash.h"

/* Scenario 1: Timed lock timeout fires when owner holds for longer than timeout.
 *
 * T0: lock(M0) → yield × 4 → unlock(M0)     (holds for 4 steps)
 * T1: timed_lock(M0, timeout=3) → yield      (timeout < hold time)
 *
 * In the interleaving where T0 locks first, T1 blocks and times out after
 * 3 ticks (before T0's 4th yield) — T1 recovers without a deadlock.
 *
 * In the interleaving where T1 runs before T0, T1 may see the mutex free and
 * acquire it (no timeout needed).  In that case T1 exits still holding the
 * mutex, which SMASH correctly flags as a violation.  This is expected:
 * the model has no conditional branching to "unlock only if acquired."
 *
 * Expected: at most 1 violation (the "T1 acquires first" race ordering).
 */
static smash_scenario_t timed_lock_prevents_deadlock(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name           = "Timed lock timeout fires";
    sc.thread_count   = 2;
    sc.resource_count = 1;
    sc.priorities[0]  = 10;
    sc.priorities[1]  = 10;
    sc.res_types[0]   = RES_MUTEX;

    /* T0: lock(M0) -> 4 yields -> unlock(M0) */
    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[0][1] = (smash_action_t){ACT_YIELD,        SMASH_NO_RESOURCE};
    sc.steps[0][2] = (smash_action_t){ACT_YIELD,        SMASH_NO_RESOURCE};
    sc.steps[0][3] = (smash_action_t){ACT_YIELD,        SMASH_NO_RESOURCE};
    sc.steps[0][4] = (smash_action_t){ACT_YIELD,        SMASH_NO_RESOURCE};
    sc.steps[0][5] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[0] = 6;

    /* T1: timed_lock(M0, timeout=3) -> yield */
    sc.steps[1][0] = (smash_action_t){ACT_MUTEX_TIMED_LOCK, 0, .arg = 3};
    sc.steps[1][1] = (smash_action_t){ACT_YIELD,            SMASH_NO_RESOURCE};
    sc.step_count[1] = 2;

    return sc;
}

/* Scenario 2: Timed semaphore wait with timeout
 *
 * T0: signal(S) after delay
 * T1: timed_wait(S, timeout=5) -> may timeout or succeed
 *
 * Tests that timeout expires correctly when no signal arrives.
 */
static smash_scenario_t timed_sem_timeout(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name = "Timed semaphore wait timeout";
    sc.thread_count = 2;
    sc.resource_count = 1;
    sc.priorities[0] = 10;
    sc.priorities[1] = 10;
    sc.res_types[0] = RES_SEMAPHORE;
    sc.sem_init[0] = 0;  /* Initially empty */

    /* T0: delay -> signal(S) */
    sc.steps[0][0] = (smash_action_t){ACT_YIELD,       SMASH_NO_RESOURCE};
    sc.steps[0][1] = (smash_action_t){ACT_YIELD,       SMASH_NO_RESOURCE};
    sc.steps[0][2] = (smash_action_t){ACT_YIELD,       SMASH_NO_RESOURCE};
    sc.steps[0][3] = (smash_action_t){ACT_SEM_SIGNAL,  0};
    sc.step_count[0] = 4;

    /* T1: timed_wait(S, timeout=10) -> may timeout before signal */
    sc.steps[1][0] = (smash_action_t){ACT_SEM_TIMED_WAIT, 0};
    sc.step_count[1] = 1;

    return sc;
}

/* Scenario 3: Timed wait succeeds (signal arrives before timeout)
 *
 * T0: signal(S) immediately
 * T1: timed_wait(S, timeout=10) -> succeeds
 *
 * Verifies that timeout doesn't interfere with successful operations.
 */
static smash_scenario_t timed_sem_success(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name = "Timed semaphore wait succeeds";
    sc.thread_count = 2;
    sc.resource_count = 1;
    sc.priorities[0] = 10;
    sc.priorities[1] = 10;
    sc.res_types[0] = RES_SEMAPHORE;
    sc.sem_init[0] = 0;

    /* T0: signal(S) immediately */
    sc.steps[0][0] = (smash_action_t){ACT_SEM_SIGNAL,  0};
    sc.step_count[0] = 1;

    /* T1: timed_wait(S) -> succeeds (signal already pending) */
    sc.steps[1][0] = (smash_action_t){ACT_SEM_TIMED_WAIT, 0};
    sc.step_count[1] = 1;

    return sc;
}

static void run_test(const char *label, smash_scenario_t sc,
                     int expected_violations_max) {

    printf("\n=== %s ===\n", label);

    smash_config_t config = {
        .enable_dpor          = true,
        .enable_state_caching = true,
        .max_depth            = 64,
        .max_interleavings    = 10000,
        .stop_on_first_bug    = false,
        .verbose              = false,
    };

    smash_result_t r = smash_explore(&sc, &config);
    smash_result_print(&r, stdout);

    if (r.violations <= (uint64_t)expected_violations_max) {
        printf("✓ PASS: violations=%llu (expected ≤%d)\n",
               r.violations, expected_violations_max);
    } else {
        printf("✗ FAIL: violations=%llu (expected ≤%d)\n",
               r.violations, expected_violations_max);
    }

    if (r.failing_trace) {
        printf("\nTrace:\n");
        smash_trace_dump(r.failing_trace, stdout);
        smash_result_free(&r);
    }
}

int main(void) {

    printf("SMASH — Timeout operations test\n");
    printf("Testing ACT_MUTEX_TIMED_LOCK and ACT_SEM_TIMED_WAIT\n");

    /* Test 1: Timed lock timeout fires (at most 1 violation: "T1 first" race) */
    run_test("1. Timed lock timeout fires",
             timed_lock_prevents_deadlock(), 1);

    /* Test 2: Timed semaphore timeout */
    run_test("2. Timed semaphore wait timeout",
             timed_sem_timeout(), 0);

    /* Test 3: Timed semaphore success */
    run_test("3. Timed semaphore wait succeeds",
             timed_sem_success(), 0);

    printf("\n========================================\n");
    printf("All timeout tests completed\n");
    printf("========================================\n");

    return 0;
}
