/*
 * SMASH test: Priority inversion scenario
 *
 * T0 (low,  prio 5):  lock(M0) -> yield -> yield -> unlock(M0)
 * T1 (med,  prio 10): yield -> yield -> yield
 * T2 (high, prio 15): yield -> lock(M0) -> unlock(M0)
 *
 * Without priority inheritance, T1 (medium) can preempt T0 (low) while
 * T2 (high) is blocked on M0 owned by T0 — classic unbounded inversion.
 *
 * With ChibiOS priority inheritance, T0 should be boosted to T2's priority
 * when T2 blocks, preventing T1 from preempting.
 */

#include "smash.h"

static smash_scenario_t priority_inversion(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name = "Priority inversion (3 threads)";
    sc.thread_count = 3;
    sc.resource_count = 1;
    sc.priorities[0] = 5;   /* low */
    sc.priorities[1] = 10;  /* medium */
    sc.priorities[2] = 15;  /* high */
    sc.res_types[0] = RES_MUTEX;

    /* T0 (low): lock(M0) -> yield -> yield -> unlock(M0) */
    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[0][1] = (smash_action_t){ACT_YIELD,       SMASH_NO_RESOURCE};
    sc.steps[0][2] = (smash_action_t){ACT_YIELD,       SMASH_NO_RESOURCE};
    sc.steps[0][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[0] = 4;

    /* T1 (med): yield -> yield -> yield */
    sc.steps[1][0] = (smash_action_t){ACT_YIELD, SMASH_NO_RESOURCE};
    sc.steps[1][1] = (smash_action_t){ACT_YIELD, SMASH_NO_RESOURCE};
    sc.steps[1][2] = (smash_action_t){ACT_YIELD, SMASH_NO_RESOURCE};
    sc.step_count[1] = 3;

    /* T2 (high): yield -> lock(M0) -> unlock(M0) */
    sc.steps[2][0] = (smash_action_t){ACT_YIELD,       SMASH_NO_RESOURCE};
    sc.steps[2][1] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[2][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[2] = 3;

    return sc;
}

int main(void) {

    smash_config_t config = {
        .enable_dpor          = true,
        .enable_state_caching = true,
        .max_depth            = 64,
        .max_interleavings    = 100000,
        .stop_on_first_bug    = false,
        .verbose              = true
    };

    printf("=== Test: Priority inversion ===\n");
    smash_scenario_t sc = priority_inversion();
    smash_result_t r = smash_explore(&sc, &config);
    smash_result_print(&r, stdout);

    if (r.failing_trace) {
        printf("\nJSON trace:\n");
        smash_trace_dump_json(r.failing_trace, stdout);
    }
    smash_result_free(&r);

    return 0;
}
