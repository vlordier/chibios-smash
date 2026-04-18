/*
 * SMASH test: Classic mutex deadlock (ABBA pattern)
 *
 * T0: lock(M0) -> lock(M1) -> unlock(M1) -> unlock(M0)
 * T1: lock(M1) -> lock(M0) -> unlock(M0) -> unlock(M1)
 *
 * Some interleavings will deadlock. SMASH should find them.
 */

#include "smash.h"

static smash_scenario_t abba_deadlock(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name = "ABBA mutex deadlock";
    sc.thread_count = 2;
    sc.resource_count = 2;
    sc.priorities[0] = 10;
    sc.priorities[1] = 10;
    sc.res_types[0] = RES_MUTEX;
    sc.res_types[1] = RES_MUTEX;

    /* T0: lock(M0) -> lock(M1) -> unlock(M1) -> unlock(M0) */
    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[0][1] = (smash_action_t){ACT_MUTEX_LOCK,   1};
    sc.steps[0][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.steps[0][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[0] = 4;

    /* T1: lock(M1) -> lock(M0) -> unlock(M0) -> unlock(M1) */
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

    sc.name = "Safe mutex ordering (no deadlock)";
    sc.thread_count = 2;
    sc.resource_count = 2;
    sc.priorities[0] = 10;
    sc.priorities[1] = 10;
    sc.res_types[0] = RES_MUTEX;
    sc.res_types[1] = RES_MUTEX;

    /* Both threads lock in the same order: M0 then M1. */
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

    /* Test 1: ABBA deadlock - should find deadlocks. */
    printf("=== Test 1: ABBA deadlock scenario ===\n");
    smash_scenario_t sc1 = abba_deadlock();
    smash_result_t r1 = smash_explore(&sc1, &config);
    smash_result_print(&r1, stdout);

    if (r1.failing_trace) {
        printf("\nJSON trace:\n");
        smash_trace_dump_json(r1.failing_trace, stdout);
        free(r1.failing_trace);
    }

    /* Test 2: Safe ordering - should find no deadlocks. */
    printf("\n=== Test 2: Safe ordering scenario ===\n");
    smash_scenario_t sc2 = safe_ordering();
    smash_result_t r2 = smash_explore(&sc2, &config);
    smash_result_print(&r2, stdout);
    free(r2.failing_trace);

    /* Export SMT for the deadlock scenario. */
    printf("\n=== SMT-LIB2 export (ABBA) ===\n");
    smash_trace_t empty;
    smash_trace_init(&empty);
    smash_smt_export(&empty, &sc1, stdout);

    return (r1.deadlocks > 0 && r2.deadlocks == 0) ? 0 : 1;
}
