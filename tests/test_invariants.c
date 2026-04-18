/*
 * SMASH test: Circular wait detection
 *
 * Tests the wait-for graph cycle detection invariant.
 * This catches deadlocks earlier with better diagnostics than
 * just "no runnable threads".
 *
 * Three-way circular wait scenario:
 *   T0: lock(M0) -> lock(M1)
 *   T1: lock(M1) -> lock(M2)
 *   T2: lock(M2) -> lock(M0)
 *
 * This creates a 3-way cycle: T0 -> T1 -> T2 -> T0
 */

#include "smash.h"

static smash_scenario_t three_way_circular_wait(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name = "Three-way circular wait (T0->T1->T2->T0)";
    sc.thread_count = 3;
    sc.resource_count = 3;
    sc.priorities[0] = 10;
    sc.priorities[1] = 10;
    sc.priorities[2] = 10;
    sc.res_types[0] = RES_MUTEX;
    sc.res_types[1] = RES_MUTEX;
    sc.res_types[2] = RES_MUTEX;

    /* T0: lock(M0) -> lock(M1) -> unlock(M1) -> unlock(M0) */
    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[0][1] = (smash_action_t){ACT_MUTEX_LOCK,   1};
    sc.steps[0][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.steps[0][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[0] = 4;

    /* T1: lock(M1) -> lock(M2) -> unlock(M2) -> unlock(M1) */
    sc.steps[1][0] = (smash_action_t){ACT_MUTEX_LOCK,   1};
    sc.steps[1][1] = (smash_action_t){ACT_MUTEX_LOCK,   2};
    sc.steps[1][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 2};
    sc.steps[1][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.step_count[1] = 4;

    /* T2: lock(M2) -> lock(M0) -> unlock(M0) -> unlock(M2) */
    sc.steps[2][0] = (smash_action_t){ACT_MUTEX_LOCK,   2};
    sc.steps[2][1] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[2][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.steps[2][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 2};
    sc.step_count[2] = 4;

    return sc;
}

static smash_scenario_t two_way_circular_wait(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name = "Two-way circular wait (classic ABBA)";
    sc.thread_count = 2;
    sc.resource_count = 2;
    sc.priorities[0] = 10;
    sc.priorities[1] = 10;
    sc.res_types[0] = RES_MUTEX;
    sc.res_types[1] = RES_MUTEX;

    /* T0: lock(M0) -> lock(M1) */
    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[0][1] = (smash_action_t){ACT_MUTEX_LOCK,   1};
    sc.steps[0][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.steps[0][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[0] = 4;

    /* T1: lock(M1) -> lock(M0) */
    sc.steps[1][0] = (smash_action_t){ACT_MUTEX_LOCK,   1};
    sc.steps[1][1] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[1][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.steps[1][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.step_count[1] = 4;

    return sc;
}

static smash_scenario_t safe_ordering(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name = "Safe ordering (no circular wait)";
    sc.thread_count = 2;
    sc.resource_count = 2;
    sc.priorities[0] = 10;
    sc.priorities[1] = 10;
    sc.res_types[0] = RES_MUTEX;
    sc.res_types[1] = RES_MUTEX;

    /* Both threads lock in same order: M0 then M1 */
    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[0][1] = (smash_action_t){ACT_MUTEX_LOCK,   1};
    sc.steps[0][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.steps[0][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[0] = 4;

    sc.steps[1][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[1][1] = (smash_action_t){ACT_MUTEX_LOCK,   1};
    sc.steps[1][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.steps[1][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[1] = 4;

    return sc;
}

int main(void) {

    smash_config_t config = {
        .enable_dpor          = false,
        .enable_state_caching = true,
        .max_depth            = 64,
        .max_interleavings    = 100000,
        .stop_on_first_bug    = false,
        .verbose              = true
    };

    int tests_passed = 0;
    int tests_total = 0;

    /* Test 1: Two-way circular wait */
    printf("=== Test 1: Two-way circular wait (ABBA) ===\n");
    tests_total++;
    smash_scenario_t sc1 = two_way_circular_wait();
    smash_result_t r1 = smash_explore(&sc1, &config);
    smash_result_print(&r1, stdout);

    if (r1.deadlocks > 0 || r1.violations > 0) {
        printf("✓ Correctly detected circular wait\n");
        tests_passed++;
    } else {
        printf("✗ FAILED to detect circular wait\n");
    }

    if (r1.failing_trace) {
        printf("\nFirst failing trace:\n");
        smash_trace_dump(r1.failing_trace, stdout);
        free(r1.failing_trace);
    }

    /* Test 2: Three-way circular wait */
    printf("\n=== Test 2: Three-way circular wait (T0->T1->T2->T0) ===\n");
    tests_total++;
    smash_scenario_t sc2 = three_way_circular_wait();
    smash_result_t r2 = smash_explore(&sc2, &config);
    smash_result_print(&r2, stdout);

    if (r2.deadlocks > 0 || r2.violations > 0) {
        printf("✓ Correctly detected 3-way circular wait\n");
        tests_passed++;
    } else {
        printf("✗ FAILED to detect 3-way circular wait\n");
    }

    if (r2.failing_trace) {
        printf("\nFirst failing trace:\n");
        smash_trace_dump(r2.failing_trace, stdout);
        free(r2.failing_trace);
    }

    /* Test 3: Safe ordering (should NOT detect circular wait) */
    printf("\n=== Test 3: Safe ordering (no circular wait expected) ===\n");
    tests_total++;
    smash_scenario_t sc3 = safe_ordering();
    smash_result_t r3 = smash_explore(&sc3, &config);
    smash_result_print(&r3, stdout);

    if (r3.deadlocks == 0 && r3.violations == 0) {
        printf("✓ Correctly found no circular wait\n");
        tests_passed++;
    } else {
        printf("✗ FALSE POSITIVE - reported circular wait in safe scenario\n");
    }

    smash_result_free(&r3);

    /* Summary */
    printf("\n========================================\n");
    printf("SUMMARY: %d/%d tests passed\n", tests_passed, tests_total);
    printf("========================================\n");

    return (tests_passed == tests_total) ? 0 : 1;
}
