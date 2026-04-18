/*
 * SMASH test: Semaphore producer-consumer with potential lost wakeup
 *
 * T0 (producer): signal(S) -> signal(S)
 * T1 (consumer): wait(S) -> wait(S)
 * T2 (consumer): wait(S)
 *
 * With sem initially 0 and 2 signals / 3 waits, one consumer must block.
 * SMASH should detect deadlock in some interleavings (3 waits, only 2 signals).
 */

#include "smash.h"

static smash_scenario_t producer_consumer(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name = "Producer-consumer (unbalanced)";
    sc.thread_count = 3;
    sc.resource_count = 1;
    sc.priorities[0] = 10;
    sc.priorities[1] = 10;
    sc.priorities[2] = 10;
    sc.res_types[0] = RES_SEMAPHORE;
    sc.sem_init[0] = 0;

    /* T0: signal, signal */
    sc.steps[0][0] = (smash_action_t){ACT_SEM_SIGNAL, 0};
    sc.steps[0][1] = (smash_action_t){ACT_SEM_SIGNAL, 0};
    sc.step_count[0] = 2;

    /* T1: wait, wait */
    sc.steps[1][0] = (smash_action_t){ACT_SEM_WAIT, 0};
    sc.steps[1][1] = (smash_action_t){ACT_SEM_WAIT, 0};
    sc.step_count[1] = 2;

    /* T2: wait */
    sc.steps[2][0] = (smash_action_t){ACT_SEM_WAIT, 0};
    sc.step_count[2] = 1;

    return sc;
}

static smash_scenario_t balanced_semaphore(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name = "Balanced producer-consumer";
    sc.thread_count = 2;
    sc.resource_count = 1;
    sc.priorities[0] = 10;
    sc.priorities[1] = 10;
    sc.res_types[0] = RES_SEMAPHORE;
    sc.sem_init[0] = 0;

    /* T0: signal */
    sc.steps[0][0] = (smash_action_t){ACT_SEM_SIGNAL, 0};
    sc.step_count[0] = 1;

    /* T1: wait */
    sc.steps[1][0] = (smash_action_t){ACT_SEM_WAIT, 0};
    sc.step_count[1] = 1;

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

    printf("=== Test: Unbalanced producer-consumer ===\n");
    smash_scenario_t sc1 = producer_consumer();
    smash_result_t r1 = smash_explore(&sc1, &config);
    smash_result_print(&r1, stdout);
    free(r1.failing_trace);

    printf("\n=== Test: Balanced producer-consumer ===\n");
    smash_scenario_t sc2 = balanced_semaphore();
    smash_result_t r2 = smash_explore(&sc2, &config);
    smash_result_print(&r2, stdout);
    free(r2.failing_trace);

    return 0;
}
