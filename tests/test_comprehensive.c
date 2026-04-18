/*
 * SMASH - Comprehensive Test Suite
 *
 * Tests all major features:
 * - DPOR with sleep sets
 * - Circular wait detection
 * - Timeout operations
 * - Priority inheritance
 * - State caching
 * - All safety invariants
 */

#include "smash.h"
#include <stdio.h>
#include <string.h>

/* Test counter */
static int tests_run = 0;
static int tests_passed = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
        printf("  ✓ %s\n", msg); \
    } else { \
        printf("  ✗ FAIL: %s\n", msg); \
    } \
} while(0)

/*===========================================================================*/
/* Test 1: DPOR with sleep sets effectiveness                                */
/*===========================================================================*/

static void test_dpor_sleep_sets(void) {

    printf("\n=== Test 1: DPOR with Sleep Sets ===\n");

    /* Create scenario with independent threads */
    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.name = "Independent threads";
    sc.thread_count = 4;
    sc.resource_count = 2;
    sc.priorities[0] = 10; sc.priorities[1] = 10;
    sc.priorities[2] = 10; sc.priorities[3] = 10;
    sc.res_types[0] = RES_SEMAPHORE;
    sc.res_types[1] = RES_SEMAPHORE;
    sc.sem_init[0] = 0; sc.sem_init[1] = 0;

    /* T0, T1 operate on S0 */
    sc.steps[0][0] = (smash_action_t){ACT_SEM_SIGNAL, 0};
    sc.steps[0][1] = (smash_action_t){ACT_SEM_WAIT, 0};
    sc.step_count[0] = 2;

    sc.steps[1][0] = (smash_action_t){ACT_SEM_WAIT, 0};
    sc.steps[1][1] = (smash_action_t){ACT_SEM_SIGNAL, 0};
    sc.step_count[1] = 2;

    /* T2, T3 operate on S1 */
    sc.steps[2][0] = (smash_action_t){ACT_SEM_SIGNAL, 1};
    sc.steps[2][1] = (smash_action_t){ACT_SEM_WAIT, 1};
    sc.step_count[2] = 2;

    sc.steps[3][0] = (smash_action_t){ACT_SEM_WAIT, 1};
    sc.steps[3][1] = (smash_action_t){ACT_SEM_SIGNAL, 1};
    sc.step_count[3] = 2;

    smash_config_t config = {
        .enable_dpor = true,
        .enable_state_caching = true,
        .stop_on_first_bug = false,
        .verbose = false,
    };

    smash_result_t r = smash_explore(&sc, &config);

    TEST_ASSERT(r.states > 0, "Exploration completed");
    TEST_ASSERT(r.sleep_pruned > 0 || r.dpor_pruned > 0, "DPOR/sleep sets pruned states");
    /* Scenario may have deadlocks depending on semaphore balance */
    TEST_ASSERT(r.violations == 0, "No invariant violations");

    printf("  Stats: iters=%llu states=%llu dpor=%llu sleep=%llu\n",
           r.interleavings, r.states, r.dpor_pruned, r.sleep_pruned);
}

/*===========================================================================*/
/* Test 2: Circular wait detection                                           */
/*===========================================================================*/

static void test_circular_wait_detection(void) {

    printf("\n=== Test 2: Circular Wait Detection ===\n");

    /* 2-way circular wait (ABBA) */
    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.name = "ABBA deadlock";
    sc.thread_count = 2;
    sc.resource_count = 2;
    sc.priorities[0] = 10; sc.priorities[1] = 10;
    sc.res_types[0] = RES_MUTEX; sc.res_types[1] = RES_MUTEX;

    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK, 0};
    sc.steps[0][1] = (smash_action_t){ACT_MUTEX_LOCK, 1};
    sc.steps[0][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.steps[0][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[0] = 4;

    sc.steps[1][0] = (smash_action_t){ACT_MUTEX_LOCK, 1};
    sc.steps[1][1] = (smash_action_t){ACT_MUTEX_LOCK, 0};
    sc.steps[1][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.steps[1][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.step_count[1] = 4;

    smash_config_t config = {
        .enable_dpor = false,
        .enable_state_caching = true,
        .stop_on_first_bug = true,
        .verbose = false,
    };

    smash_result_t r = smash_explore(&sc, &config);

    TEST_ASSERT(r.violations > 0 || r.deadlocks > 0, "Detected circular wait");
    TEST_ASSERT(r.failing_trace != NULL, "Generated failing trace");

    if (r.failing_trace) {
        TEST_ASSERT(r.failing_trace->count > 0, "Trace has events");
    }
}

/*===========================================================================*/
/* Test 3: Timeout operations                                                */
/*===========================================================================*/

static void test_timeout_operations(void) {

    printf("\n=== Test 3: Timeout Operations ===\n");

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.name = "Timeout test";
    sc.thread_count = 2;
    sc.resource_count = 1;
    sc.priorities[0] = 10; sc.priorities[1] = 10;
    sc.res_types[0] = RES_MUTEX;

    /* T0 holds mutex */
    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK, 0};
    sc.steps[0][1] = (smash_action_t){ACT_YIELD, -1};
    sc.steps[0][2] = (smash_action_t){ACT_YIELD, -1};
    sc.steps[0][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[0] = 4;

    /* T1 tries timed lock */
    sc.steps[1][0] = (smash_action_t){ACT_MUTEX_TIMED_LOCK, 0};
    sc.steps[1][1] = (smash_action_t){ACT_YIELD, -1};
    sc.step_count[1] = 2;

    smash_config_t config = {
        .enable_dpor = true,
        .enable_state_caching = true,
        .stop_on_first_bug = false,
        .verbose = false,
    };

    smash_result_t r = smash_explore(&sc, &config);

    TEST_ASSERT(r.states > 0, "Exploration completed");
    /* Timeout should prevent deadlock */
    TEST_ASSERT(r.deadlocks == 0, "No deadlock with timeout");
}

/*===========================================================================*/
/* Test 4: Priority inheritance                                              */
/*===========================================================================*/

static void test_priority_inheritance(void) {

    printf("\n=== Test 4: Priority Inheritance ===\n");

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.name = "Priority inheritance";
    sc.thread_count = 3;
    sc.resource_count = 1;
    sc.priorities[0] = 5;   /* Low */
    sc.priorities[1] = 10;  /* Medium */
    sc.priorities[2] = 15;  /* High */
    sc.res_types[0] = RES_MUTEX;

    /* T0 (low) locks mutex */
    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK, 0};
    sc.steps[0][1] = (smash_action_t){ACT_YIELD, -1};
    sc.steps[0][2] = (smash_action_t){ACT_YIELD, -1};
    sc.steps[0][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[0] = 4;

    /* T1 (med) yields */
    sc.steps[1][0] = (smash_action_t){ACT_YIELD, -1};
    sc.steps[1][1] = (smash_action_t){ACT_YIELD, -1};
    sc.step_count[1] = 2;

    /* T2 (high) tries to lock - should boost T0 */
    sc.steps[2][0] = (smash_action_t){ACT_YIELD, -1};
    sc.steps[2][1] = (smash_action_t){ACT_MUTEX_LOCK, 0};
    sc.steps[2][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[2] = 3;

    smash_config_t config = {
        .enable_dpor = true,
        .enable_state_caching = true,
        .stop_on_first_bug = false,
        .verbose = false,
    };

    smash_result_t r = smash_explore(&sc, &config);

    TEST_ASSERT(r.interleavings > 0, "Exploration completed");
    TEST_ASSERT(r.violations == 0, "No priority inversion (inheritance works)");
}

/*===========================================================================*/
/* Test 5: State caching effectiveness                                       */
/*===========================================================================*/

static void test_state_caching(void) {

    printf("\n=== Test 5: State Caching ===\n");

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.name = "State caching test";
    sc.thread_count = 2;
    sc.resource_count = 1;
    sc.priorities[0] = 10; sc.priorities[1] = 10;
    sc.res_types[0] = RES_SEMAPHORE;
    sc.sem_init[0] = 1;

    sc.steps[0][0] = (smash_action_t){ACT_SEM_WAIT, 0};
    sc.steps[0][1] = (smash_action_t){ACT_SEM_SIGNAL, 0};
    sc.step_count[0] = 2;

    sc.steps[1][0] = (smash_action_t){ACT_SEM_WAIT, 0};
    sc.steps[1][1] = (smash_action_t){ACT_SEM_SIGNAL, 0};
    sc.step_count[1] = 2;

    /* Without caching */
    smash_config_t config_no_cache = {
        .enable_dpor = false,
        .enable_state_caching = false,
        .stop_on_first_bug = false,
        .verbose = false,
    };
    smash_result_t r1 = smash_explore(&sc, &config_no_cache);

    /* With caching */
    smash_config_t config_cache = {
        .enable_dpor = false,
        .enable_state_caching = true,
        .stop_on_first_bug = false,
        .verbose = false,
    };
    smash_result_t r2 = smash_explore(&sc, &config_cache);

    TEST_ASSERT(r2.cache_pruned > 0, "State caching pruned states");
    TEST_ASSERT(r2.states <= r1.states, "Caching reduced states");
}

/*===========================================================================*/
/* Test 6: All safety invariants                                             */
/*===========================================================================*/

static void test_safety_invariants(void) {

    printf("\n=== Test 6: Safety Invariants ===\n");

    /* Test scenario that should pass all invariants */
    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.name = "Safe scenario";
    sc.thread_count = 2;
    sc.resource_count = 2;
    sc.priorities[0] = 10; sc.priorities[1] = 10;
    sc.res_types[0] = RES_MUTEX; sc.res_types[1] = RES_MUTEX;

    /* Both threads use proper lock ordering */
    sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK, 0};
    sc.steps[0][1] = (smash_action_t){ACT_MUTEX_LOCK, 1};
    sc.steps[0][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.steps[0][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[0] = 4;

    sc.steps[1][0] = (smash_action_t){ACT_MUTEX_LOCK, 0};
    sc.steps[1][1] = (smash_action_t){ACT_MUTEX_LOCK, 1};
    sc.steps[1][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.steps[1][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.step_count[1] = 4;

    smash_config_t config = {
        .enable_dpor = true,
        .enable_state_caching = true,
        .stop_on_first_bug = false,
        .verbose = false,
    };

    smash_result_t r = smash_explore(&sc, &config);

    TEST_ASSERT(r.violations == 0, "No invariant violations");
    TEST_ASSERT(r.deadlocks == 0, "No deadlocks");
}

/*===========================================================================*/
/* Main                                                                      */
/*===========================================================================*/

int main(void) {

    printf("SMASH - Comprehensive Test Suite\n");
    printf("=================================\n");

    test_dpor_sleep_sets();
    test_circular_wait_detection();
    test_timeout_operations();
    test_priority_inheritance();
    test_state_caching();
    test_safety_invariants();

    printf("\n=================================\n");
    printf("Results: %d/%d tests passed\n", tests_passed, tests_run);
    printf("=================================\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
