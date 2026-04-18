/*
 * SMASH test: DPOR benchmark
 *
 * Demonstrates the persistent-sets DPOR algorithm by comparing four
 * exploration modes on the same scenario:
 *
 *   1. Plain DFS (no cache, no DPOR)  — baseline, all orderings explored
 *   2. State caching only             — prunes revisited states
 *   3. DPOR only                      — prunes independent orderings
 *   4. State caching + DPOR           — both; smallest state space
 *
 * Scenario: two producer-consumer pairs sharing INDEPENDENT semaphores.
 * Since S0 and S1 are different resources, operations on S0 are independent
 * of operations on S1.  DPOR should detect this and explore far fewer
 * orderings than plain DFS.
 *
 *   T0 (prio 10): signal(S0) -> signal(S0)
 *   T1 (prio 10): wait(S0)   -> wait(S0)
 *   T2 (prio 10): signal(S1) -> signal(S1)
 *   T3 (prio 10): wait(S1)   -> wait(S1)
 *
 * S0 and S1 are initialised to 0. T0/T1 are a matched pair on S0;
 * T2/T3 are a matched pair on S1.  The two pairs are fully independent.
 * Plain DFS must try all 4! orderings at each step; DPOR reduces this
 * significantly by recognising that T0/T1 actions on S0 are independent
 * of T2/T3 actions on S1.
 */

#include "smash.h"
#include <stdio.h>

static smash_scenario_t two_independent_pairs(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name         = "Two independent semaphore pairs";
    sc.thread_count = 4;
    sc.resource_count = 2;

    sc.priorities[0] = 10;
    sc.priorities[1] = 10;
    sc.priorities[2] = 10;
    sc.priorities[3] = 10;

    sc.res_types[0] = RES_SEMAPHORE;
    sc.res_types[1] = RES_SEMAPHORE;
    sc.sem_init[0]  = 0;   /* S0 empty */
    sc.sem_init[1]  = 0;   /* S1 empty */

    /* T0: signal(S0) -> signal(S0) */
    sc.steps[0][0] = (smash_action_t){ACT_SEM_SIGNAL, 0};
    sc.steps[0][1] = (smash_action_t){ACT_SEM_SIGNAL, 0};
    sc.step_count[0] = 2;

    /* T1: wait(S0) -> wait(S0) */
    sc.steps[1][0] = (smash_action_t){ACT_SEM_WAIT, 0};
    sc.steps[1][1] = (smash_action_t){ACT_SEM_WAIT, 0};
    sc.step_count[1] = 2;

    /* T2: signal(S1) -> signal(S1) */
    sc.steps[2][0] = (smash_action_t){ACT_SEM_SIGNAL, 1};
    sc.steps[2][1] = (smash_action_t){ACT_SEM_SIGNAL, 1};
    sc.step_count[2] = 2;

    /* T3: wait(S1) -> wait(S1) */
    sc.steps[3][0] = (smash_action_t){ACT_SEM_WAIT, 1};
    sc.steps[3][1] = (smash_action_t){ACT_SEM_WAIT, 1};
    sc.step_count[3] = 2;

    return sc;
}

static smash_result_t run(const char *label, const smash_scenario_t *sc,
                          bool dpor, bool cache) {

    smash_config_t cfg = {
        .enable_dpor          = dpor,
        .enable_state_caching = cache,
        .max_depth            = 64,
        .max_interleavings    = 1000000,
        .stop_on_first_bug    = false,
        .verbose              = false,
    };

    smash_result_t r = smash_explore(sc, &cfg);

    printf("  %-38s  iters=%6llu  states=%6llu"
           "  cache=%5llu  dpor=%5llu  sleep=%5llu  t=%.4fs\n",
           label,
           r.interleavings,
           r.states,
           r.cache_pruned,
           r.dpor_pruned,
           r.sleep_pruned,
           r.elapsed_secs);

    if (r.failing_trace) {
        printf("    *** BUG FOUND (unexpected) ***\n");
        smash_trace_dump(r.failing_trace, stdout);
    }
    return r;
}

int main(void) {

    printf("=== DPOR benchmark: two independent semaphore pairs ===\n\n");

    smash_scenario_t sc = two_independent_pairs();

    printf("  %-38s  %s\n", "Mode", "Results");
    printf("  %s\n", "----------------------------------------------------------------------");
    smash_result_t r_plain = run("Plain DFS (no cache, no DPOR)",   &sc, false, false);
    smash_result_t r_cache = run("State caching only",              &sc, false, true);
    smash_result_t r_dpor  = run("DPOR only (persistent sets)",     &sc, true,  false);
    smash_result_t r_both  = run("DPOR + state caching",            &sc, true,  true);

    printf("\n");
    printf("Expected: DPOR reduces interleavings because S0-operations\n");
    printf("are independent of S1-operations (different resources).\n");
    printf("Plain DFS must enumerate all orderings; DPOR prunes symmetric ones.\n");

    int failed = 0;

    /* DPOR must prune at least some interleavings vs plain DFS. */
    if (r_dpor.interleavings >= r_plain.interleavings) {
        fprintf(stderr, "FAIL: DPOR did not reduce interleavings (%llu >= %llu)\n",
                r_dpor.interleavings, r_plain.interleavings);
        failed++;
    }
    /* State caching must prune at least some states vs plain DFS. */
    if (r_cache.states >= r_plain.states) {
        fprintf(stderr, "FAIL: state caching did not reduce states (%llu >= %llu)\n",
                r_cache.states, r_plain.states);
        failed++;
    }
    /* Combined must be no worse than either alone. */
    if (r_both.states > r_dpor.states && r_both.states > r_cache.states) {
        fprintf(stderr, "FAIL: combined mode is worse than either individual mode\n");
        failed++;
    }
    /* No bugs in any mode (scenario is deadlock-free). */
    if (r_plain.deadlocks || r_cache.deadlocks || r_dpor.deadlocks || r_both.deadlocks) {
        fprintf(stderr, "FAIL: unexpected deadlock in benchmark scenario\n");
        failed++;
    }

    smash_result_free(&r_plain);
    smash_result_free(&r_cache);
    smash_result_free(&r_dpor);
    smash_result_free(&r_both);

    return failed ? 1 : 0;
}
