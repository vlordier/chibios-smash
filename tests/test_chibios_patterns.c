/*
 * SMASH - ChibiOS-specific pattern tests
 *
 * Scenarios derived directly from reading chmtx.c and chsem.c.
 * Each test targets a specific kernel behavior or known edge case.
 */

#include "smash.h"

/* -------------------------------------------------------------------------
 * SCENARIO 1: Multi-hop priority inheritance chain
 *
 * ChibiOS chmtx.c L198-246: while (tp->prio < currtp->prio) follows the
 * WTMTX chain with 'continue', but breaks on other states (READY, WTSEM).
 * Verify that all interleavings of a 3-hop chain are correctly handled:
 *
 *   T0 (prio 5):  lock(M0) → lock(M1) → unlock(M1) → unlock(M0)
 *   T1 (prio 10): lock(M1) [may boost T0]
 *   T2 (prio 15): lock(M0) [must boost T0 through the chain]
 *
 * Invariant: T0's priority must always be >= max(waiting threads on its mutexes).
 * -------------------------------------------------------------------------*/
static smash_scenario_t sc_priority_chain(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.name = "Multi-hop priority inheritance chain";
    sc.thread_count = 3;
    sc.resource_count = 2;
    sc.priorities[0] = 5;
    sc.priorities[1] = 10;
    sc.priorities[2] = 15;
    sc.res_types[0] = RES_MUTEX;
    sc.res_types[1] = RES_MUTEX;

    /* T0 (low): lock(M0) → lock(M1) → unlock(M1) → unlock(M0) */
    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[0][1] = (smash_action_t){ACT_MUTEX_LOCK,   1};
    sc.steps[0][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.steps[0][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[0] = 4;

    /* T1 (med): lock(M1) — may have to wait, boosting T0 to prio 10 */
    sc.steps[1][0] = (smash_action_t){ACT_MUTEX_LOCK,   1};
    sc.steps[1][1] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.step_count[1] = 2;

    /* T2 (high): lock(M0) — must boost T0 to prio 15 */
    sc.steps[2][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[2][1] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[2] = 2;

    return sc;
}

/* -------------------------------------------------------------------------
 * SCENARIO 2: Semaphore inside mutex critical section
 *
 * From chsem.c: threads blocked on semaphore can still hold mutexes.
 * ChibiOS's priority chain STOPS at WTSEM state (L213-224 in chmtx.c).
 * This means priority cannot propagate through a semaphore — so:
 *
 *   T0 (low  5): lock(M) → wait(S) → unlock(M)
 *   T1 (high 15): lock(M)          [boosts T0, but T0 is blocked on S]
 *   T2 (med  10): signal(S)        [wakes T0 so T1 can eventually get M]
 *
 * SMASH should find no deadlock only if T2 signals before T1 starves.
 * In interleavings where T1 locks M before T0 waits on S → no deadlock.
 * In interleavings where T0 grabs M, waits on S, then T1 blocks on M
 * and T2 never signals → deadlock.
 * -------------------------------------------------------------------------*/
static smash_scenario_t sc_sem_inside_mutex(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.name = "Semaphore wait inside mutex (priority chain breaks at SEM)";
    sc.thread_count = 3;
    sc.resource_count = 2;
    sc.priorities[0] = 5;
    sc.priorities[1] = 15;
    sc.priorities[2] = 10;
    sc.res_types[0] = RES_MUTEX;
    sc.res_types[1] = RES_SEMAPHORE;
    sc.sem_init[1] = 0;

    /* T0 (low): lock(M) → wait(S) → unlock(M) */
    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[0][1] = (smash_action_t){ACT_SEM_WAIT,     1};
    sc.steps[0][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[0] = 3;

    /* T1 (high): lock(M) → unlock(M) */
    sc.steps[1][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[1][1] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[1] = 2;

    /* T2 (med): signal(S) */
    sc.steps[2][0] = (smash_action_t){ACT_SEM_SIGNAL,   1};
    sc.step_count[2] = 1;

    return sc;
}

/* -------------------------------------------------------------------------
 * SCENARIO 3: chMtxUnlockAllS priority restoration
 *
 * chmtx.c L526-554: chMtxUnlockAllS() unlocks ALL mutexes and then
 * restores priority to realprio in ONE shot (L551), unlike individual
 * unlock which recalculates incrementally.
 *
 * In our model this is simulated by sequential unlocks. Verify that
 * doing M1 then M0 vs M0 then M1 produces the same final state.
 *
 *   T0 (prio 5): lock(M0) → lock(M1) → unlock(M1) → unlock(M0)
 *   T1 (prio 10): lock(M0)
 *   T2 (prio 20): lock(M1)
 *
 * Bug hunt: after T0 unlocks M1, T0 should keep prio=10 (T1 still waits
 * on M0). After T0 unlocks M0, T0 should drop to prio=5.
 * -------------------------------------------------------------------------*/
static smash_scenario_t sc_unlock_priority_restore(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.name = "Priority restoration after multi-mutex unlock";
    sc.thread_count = 3;
    sc.resource_count = 2;
    sc.priorities[0] = 5;
    sc.priorities[1] = 10;
    sc.priorities[2] = 20;
    sc.res_types[0] = RES_MUTEX;
    sc.res_types[1] = RES_MUTEX;

    /* T0 (low): lock(M0) → lock(M1) → unlock(M1) → unlock(M0) */
    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[0][1] = (smash_action_t){ACT_MUTEX_LOCK,   1};
    sc.steps[0][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.steps[0][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[0] = 4;

    /* T1 (med): lock(M0) → unlock(M0) */
    sc.steps[1][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[1][1] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[1] = 2;

    /* T2 (high): lock(M1) → unlock(M1) */
    sc.steps[2][0] = (smash_action_t){ACT_MUTEX_LOCK,   1};
    sc.steps[2][1] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.step_count[2] = 2;

    return sc;
}

/* -------------------------------------------------------------------------
 * SCENARIO 4: chSemSignalWait atomicity
 *
 * chsem.c L402-431: chSemSignalWait atomically signals one semaphore
 * and waits on another. This must not create a lost wakeup even if the
 * signal would wake a waiter.
 *
 * Model: T0 and T1 both do signal(Sout) → wait(Sin) in a pipeline.
 * T2 is the initial trigger. Verify no thread starves.
 * -------------------------------------------------------------------------*/
static smash_scenario_t sc_signal_wait_pipeline(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.name = "chSemSignalWait pipeline (atomicity)";
    sc.thread_count = 3;
    sc.resource_count = 2;
    sc.priorities[0] = 10;
    sc.priorities[1] = 10;
    sc.priorities[2] = 10;
    sc.res_types[0] = RES_SEMAPHORE;
    sc.res_types[1] = RES_SEMAPHORE;
    sc.sem_init[0] = 0;  /* S0: initially empty */
    sc.sem_init[1] = 0;  /* S1: initially empty */

    /* T0: wait(S0) → signal(S1) — consumer of S0, producer of S1 */
    sc.steps[0][0] = (smash_action_t){ACT_SEM_WAIT,   0};
    sc.steps[0][1] = (smash_action_t){ACT_SEM_SIGNAL, 1};
    sc.step_count[0] = 2;

    /* T1: wait(S1) — consumer of S1 */
    sc.steps[1][0] = (smash_action_t){ACT_SEM_WAIT,   1};
    sc.step_count[1] = 1;

    /* T2: signal(S0) — initial trigger */
    sc.steps[2][0] = (smash_action_t){ACT_SEM_SIGNAL, 0};
    sc.step_count[2] = 1;

    return sc;
}

/* -------------------------------------------------------------------------
 * SCENARIO 5: Thread exits holding mutex (undefined behavior in ChibiOS)
 *
 * ChibiOS has no automatic mutex release on thread exit. If a thread exits
 * while holding a mutex, the mutex is permanently locked (no owner cleanup).
 * chthreads.c does NOT call chMtxUnlockAllS() on exit unless the thread
 * explicitly does so.
 *
 * Model: T0 locks M then exits (ACT_DONE) without unlocking.
 * T1 tries to lock M → deadlock.
 * -------------------------------------------------------------------------*/
static smash_scenario_t sc_exit_with_mutex(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.name = "Thread exits holding mutex (resource leak → deadlock)";
    sc.thread_count = 2;
    sc.resource_count = 1;
    sc.priorities[0] = 10;
    sc.priorities[1] = 10;
    sc.res_types[0] = RES_MUTEX;

    /* T0: lock(M) → exit WITHOUT unlocking */
    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK, 0};
    sc.steps[0][1] = (smash_action_t){ACT_DONE,      SMASH_NO_RESOURCE};
    sc.step_count[0] = 2;

    /* T1: lock(M) → unlock(M) — will deadlock if T0 exits holding M */
    sc.steps[1][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[1][1] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[1] = 2;

    return sc;
}

/* -------------------------------------------------------------------------
 * SCENARIO 6: chMtxUnlock requires LIFO order
 *
 * chmtx.c L378: chDbgAssert(currtp->mtxlist == mp, "not next in list")
 * ChibiOS REQUIRES mutexes to be unlocked in reverse-lock order.
 * Unlocking out of order triggers an assertion.
 *
 * Model: T0 locks M0 then M1, then tries to unlock M0 first (wrong order).
 * This must be caught as an invariant violation.
 * -------------------------------------------------------------------------*/
static smash_scenario_t sc_unlock_wrong_order(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.name = "Mutex unlock out of LIFO order (ChibiOS assertion violation)";
    sc.thread_count = 1;
    sc.resource_count = 2;
    sc.priorities[0] = 10;
    sc.res_types[0] = RES_MUTEX;
    sc.res_types[1] = RES_MUTEX;

    /* T0: lock(M0) → lock(M1) → unlock(M0) [wrong! M1 should be first] */
    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[0][1] = (smash_action_t){ACT_MUTEX_LOCK,   1};
    sc.steps[0][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 0}; /* out of order */
    sc.steps[0][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.step_count[0] = 4;

    return sc;
}

/* -------------------------------------------------------------------------
 * SCENARIO 7: True 2-hop priority inheritance chain
 *
 * Validates multi-hop inheritance: T2 (high) blocks on M0 owned by T0 (low),
 * while T0 is ITSELF blocked on M1 owned by T1 (medium).
 * ChibiOS must transitively boost T1 to T2's priority (chmtx.c L198-247).
 *
 * Without multi-hop: T0 boosted to 20, T1 stays at 10.
 *   → Priority inversion: T0 (prio 20) waiting on M1 owned by T1 (prio 10).
 * With multi-hop: T0 and T1 both boosted to 20.
 *   → No inversion.
 *
 * The critical interleaving:
 *   1. T1 locks M1  (T1 owns M1)
 *   2. T0 locks M0  (T0 owns M0)
 *   3. T0 tries M1  → blocked on T1 (chain: T0 holds M0, blocked on M1)
 *   4. T2 tries M0  → blocked on T0 (T2 boosts T0; T0 must boost T1)
 *
 * T1 only needs M1 (no ABBA deadlock — T0/T1 form no circular wait).
 * -------------------------------------------------------------------------*/
static smash_scenario_t sc_multihop_inheritance(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.name = "2-hop priority inheritance chain (multi-hop must propagate)";
    sc.thread_count = 3;
    sc.resource_count = 2;
    sc.priorities[0] = 5;    /* low   */
    sc.priorities[1] = 10;   /* medium */
    sc.priorities[2] = 20;   /* high  */
    sc.res_types[0] = RES_MUTEX;
    sc.res_types[1] = RES_MUTEX;

    /* T0 (low):  lock(M0) → lock(M1) → unlock(M1) → unlock(M0)
     * In key interleaving: T0 owns M0 then blocks waiting for M1. */
    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[0][1] = (smash_action_t){ACT_MUTEX_LOCK,   1};
    sc.steps[0][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.steps[0][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[0] = 4;

    /* T1 (medium): lock(M1) → unlock(M1)
     * Does NOT acquire M0 — avoids ABBA deadlock. */
    sc.steps[1][0] = (smash_action_t){ACT_MUTEX_LOCK,   1};
    sc.steps[1][1] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.step_count[1] = 2;

    /* T2 (high): lock(M0) → unlock(M0)
     * Blocks on T0; boost must propagate T0 → T1 transitively. */
    sc.steps[2][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[2][1] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[2] = 2;

    return sc;
}

/* -------------------------------------------------------------------------*/

static void run(const char *label, smash_scenario_t sc, bool stop_first) {

    printf("\n=== %s ===\n", label);

    smash_config_t config = {
        .enable_dpor          = true,
        .enable_state_caching = true,
        .max_depth            = 128,
        .max_interleavings    = 500000,
        .stop_on_first_bug    = stop_first,
        .verbose              = false,  /* reduce noise; summary is enough */
    };

    smash_result_t r = smash_explore(&sc, &config);
    smash_result_print(&r, stdout);

    if (r.failing_trace) {
        printf("\nFirst failing trace:\n");
        smash_trace_dump(r.failing_trace, stdout);

        /* Save JSON for viz.py */
        char fname[128];
        snprintf(fname, sizeof(fname), "build/%s.json",
                 sc.name);
        /* Replace spaces with underscores for filename. */
        for (char *p = fname + 6; *p; p++) {
            if (*p == ' ' || *p == '(' || *p == ')') *p = '_';
        }
        FILE *f = fopen(fname, "w");
        if (f) {
            smash_trace_dump_json(r.failing_trace, f);
            fclose(f);
            printf("JSON trace written to %s\n", fname);
        }

        free(r.failing_trace);
    }
}

int main(void) {

    printf("SMASH — ChibiOS pattern verification\n");
    printf("Scenarios derived from chmtx.c and chsem.c\n");

    run("1. Multi-hop priority inheritance",
        sc_priority_chain(), false);

    run("2. Semaphore inside mutex (priority chain breaks at WTSEM)",
        sc_sem_inside_mutex(), false);

    run("3. Priority restoration after multi-mutex unlock",
        sc_unlock_priority_restore(), false);

    run("4. chSemSignalWait pipeline atomicity",
        sc_signal_wait_pipeline(), false);

    run("5. Thread exits holding mutex",
        sc_exit_with_mutex(), true);

    run("6. Mutex unlock out of LIFO order",
        sc_unlock_wrong_order(), true);

    run("7. 2-hop priority inheritance chain (multi-hop propagation)",
        sc_multihop_inheritance(), false);

    return 0;
}
