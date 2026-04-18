/*
 * SMASH test: Condition Variables and Mailboxes (ChibiOS v2.0 features)
 *
 * Tests the new chCond and chMB modeling:
 * - Condition variables: chCondWait, chCondSignal, chCondBroadcast
 * - Mailboxes: chMBPost, chMBPostFront, chMBFetch
 *
 * These are fundamental ChibiOS IPC primitives used in real-time systems.
 */

#include "smash.h"
#include <stdio.h>
#include <string.h>

/*===========================================================================*/
/* Test 1: Basic Condition Variable                                          */
/*===========================================================================*/

static smash_scenario_t basic_condvar(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name = "Basic condition variable (producer-consumer)";
    sc.thread_count = 2;
    sc.resource_count = 2;  /* M0 + C0 */
    sc.priorities[0] = 10;
    sc.priorities[1] = 10;
    sc.res_types[0] = RES_MUTEX;
    sc.res_types[1] = RES_CONDVAR;
    sc.associated_mutex[1] = 0;  /* C0 associated with M0 */

    /* T0 (producer): lock -> signal_cond -> unlock
     * Note: In real code, consumer would wait first. SMASH explores all interleavings
     * including the correct one where T1 waits before T0 signals. */
    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK, 0};
    sc.steps[0][1] = (smash_action_t){ACT_YIELD, -1};  /* Let T1 run first */
    sc.steps[0][2] = (smash_action_t){ACT_COND_SIGNAL, 1};
    sc.steps[0][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[0] = 4;

    /* T1 (consumer): lock -> wait_cond -> unlock */
    sc.steps[1][0] = (smash_action_t){ACT_MUTEX_LOCK, 0};
    sc.steps[1][1] = (smash_action_t){ACT_COND_WAIT, 1};
    sc.steps[1][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[1] = 3;

    return sc;
}

/*===========================================================================*/
/* Test 2: Condition Variable Broadcast                                      */
/*===========================================================================*/

static smash_scenario_t condvar_broadcast(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name = "Condition variable broadcast (1-to-many)";
    sc.thread_count = 4;
    sc.resource_count = 2;  /* M0 + C0 */
    sc.priorities[0] = 10;
    sc.priorities[1] = 10;
    sc.priorities[2] = 10;
    sc.priorities[3] = 10;
    sc.res_types[0] = RES_MUTEX;
    sc.res_types[1] = RES_CONDVAR;
    sc.associated_mutex[1] = 0;

    /* T0: lock -> broadcast -> unlock */
    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK, 0};
    sc.steps[0][1] = (smash_action_t){ACT_COND_BROADCAST, 1};
    sc.steps[0][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[0] = 3;

    /* T1, T2, T3: lock -> wait -> unlock */
    for (int t = 1; t <= 3; t++) {
        sc.steps[t][0] = (smash_action_t){ACT_MUTEX_LOCK, 0};
        sc.steps[t][1] = (smash_action_t){ACT_COND_WAIT, 1};
        sc.steps[t][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
        sc.step_count[t] = 3;
    }

    return sc;
}

/*===========================================================================*/
/* Test 3: Basic Mailbox (FIFO)                                              */
/*===========================================================================*/

static smash_scenario_t basic_mailbox(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name = "Basic mailbox (FIFO producer-consumer)";
    sc.thread_count = 2;
    sc.resource_count = 1;  /* MB0 */
    sc.priorities[0] = 10;
    sc.priorities[1] = 10;
    sc.res_types[0] = RES_MAILBOX;
    sc.mb_capacity[0] = 4;

    /* T0 (producer): post(msg1) -> post(msg2) */
    sc.steps[0][0] = (smash_action_t){ACT_MB_POST, 0, 1};  /* msg=1 */
    sc.steps[0][1] = (smash_action_t){ACT_MB_POST, 0, 2};  /* msg=2 */
    sc.step_count[0] = 2;

    /* T1 (consumer): fetch -> fetch */
    sc.steps[1][0] = (smash_action_t){ACT_MB_FETCH, 0};
    sc.steps[1][1] = (smash_action_t){ACT_MB_FETCH, 0};
    sc.step_count[1] = 2;

    return sc;
}

/*===========================================================================*/
/* Test 4: Mailbox with Priority Message (PostFront)                        */
/*===========================================================================*/

static smash_scenario_t mailbox_priority(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name = "Mailbox with priority message (PostFront)";
    sc.thread_count = 3;
    sc.resource_count = 1;
    sc.priorities[0] = 10;
    sc.priorities[1] = 10;
    sc.priorities[2] = 10;
    sc.res_types[0] = RES_MAILBOX;
    sc.mb_capacity[0] = 4;

    /* T0: post(normal_msg=1) */
    sc.steps[0][0] = (smash_action_t){ACT_MB_POST, 0, 1};
    sc.step_count[0] = 1;

    /* T1: post_front(priority_msg=99) */
    sc.steps[1][0] = (smash_action_t){ACT_MB_POST_FRONT, 0, 99};
    sc.step_count[1] = 1;

    /* T2: fetch (should get priority_msg=99 first due to PostFront) */
    sc.steps[2][0] = (smash_action_t){ACT_MB_FETCH, 0};
    sc.steps[2][1] = (smash_action_t){ACT_MB_FETCH, 0};
    sc.step_count[2] = 2;

    return sc;
}

/*===========================================================================*/
/* Test 5: Mailbox Blocking (Full Mailbox)                                  */
/*===========================================================================*/

static smash_scenario_t mailbox_blocking(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name = "Mailbox blocking (full mailbox)";
    sc.thread_count = 3;
    sc.resource_count = 1;
    sc.priorities[0] = 10;
    sc.priorities[1] = 10;
    sc.priorities[2] = 10;
    sc.res_types[0] = RES_MAILBOX;
    sc.mb_capacity[0] = 2;  /* Small capacity to force blocking */

    /* T0, T1: fill mailbox */
    for (int t = 0; t <= 1; t++) {
        sc.steps[t][0] = (smash_action_t){ACT_MB_POST, 0, t+1};
        sc.step_count[t] = 1;
    }

    /* T2: fetch (unblocks T0 or T1) */
    sc.steps[2][0] = (smash_action_t){ACT_MB_FETCH, 0};
    sc.step_count[2] = 1;

    return sc;
}

/*===========================================================================*/
/* Test Runner                                                               */
/*===========================================================================*/

static void run_test(const char *label, smash_scenario_t sc,
                     int expected_violations, int expected_deadlocks) {

    printf("\n=== %s ===\n", label);

    smash_config_t config = {
        .enable_dpor = true,
        .enable_state_caching = true,
        .max_depth = 64,
        .max_interleavings = 10000,
        .stop_on_first_bug = false,
        .verbose = false,
    };

    smash_result_t r = smash_explore(&sc, &config);
    smash_result_print(&r, stdout);

    int pass = 1;
    if (r.violations != (uint64_t)expected_violations) {
        printf("  ✗ FAIL: violations=%llu (expected %d)\n",
               r.violations, expected_violations);
        pass = 0;
    }
    if (r.deadlocks != (uint64_t)expected_deadlocks) {
        printf("  ✗ FAIL: deadlocks=%llu (expected %d)\n",
               r.deadlocks, expected_deadlocks);
        pass = 0;
    }

    if (pass) {
        printf("  ✓ PASS\n");
    }

    if (r.failing_trace) {
        printf("\nFirst failing trace:\n");
        smash_trace_dump(r.failing_trace, stdout);
        smash_result_free(&r);
    }
}

int main(void) {

    printf("SMASH v2.0 — Condition Variables and Mailboxes Test\n");
    printf("====================================================\n");

    /* Test 1: Basic condition variable */
    run_test("1. Basic Condition Variable",
             basic_condvar(), 0, 0);

    /* Test 2: Condition variable broadcast */
    run_test("2. Condition Variable Broadcast",
             condvar_broadcast(), 0, 0);

    /* Test 3: Basic mailbox */
    run_test("3. Basic Mailbox (FIFO)",
             basic_mailbox(), 0, 0);

    /* Test 4: Mailbox with priority message */
    run_test("4. Mailbox Priority Message (PostFront)",
             mailbox_priority(), 0, 0);

    /* Test 5: Mailbox blocking */
    run_test("5. Mailbox Blocking (Full)",
             mailbox_blocking(), 0, 0);

    printf("\n====================================================\n");
    printf("All ChibiOS v2.0 feature tests completed\n");
    printf("====================================================\n");

    return 0;
}
