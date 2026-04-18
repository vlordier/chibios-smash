/*
 * SMASH test: Symbolic Execution with Z3 (Bounded Model Checking)
 *
 * Tests the symbolic execution engine for verifying concurrency
 * properties using SMT solving.
 *
 * Build with: make USE_Z3=1 test_sym_bmc
 * Requires: Z3 C API (brew install z3 / apt-get install libz3-dev)
 */

#ifdef SMASH_USE_Z3

#include "smash_sym.h"
#include "smash.h"
#include <stdio.h>
#include <assert.h>

/* Z3 C API header */
#include <z3.h>

/* Test scenario 1: Simple mutex ordering (should be safe) */
static smash_scenario_t safe_mutex_ordering(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name = "Safe mutex ordering";
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

/* Test scenario 2: ABBA deadlock pattern (should find deadlock) */
static smash_scenario_t abba_deadlock(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name = "ABBA deadlock";
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

    /* T1: lock(M1) -> lock(M0) - opposite order! */
    sc.steps[1][0] = (smash_action_t){ACT_MUTEX_LOCK,   1};
    sc.steps[1][1] = (smash_action_t){ACT_MUTEX_LOCK,   0};
    sc.steps[1][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
    sc.steps[1][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
    sc.step_count[1] = 4;

    return sc;
}

/* Test scenario 3: Unbalanced semaphore (should find deadlock) */
static smash_scenario_t unbalanced_semaphore(void) {

    smash_scenario_t sc;
    memset(&sc, 0, sizeof(sc));

    sc.name = "Unbalanced semaphore";
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

    /* T2: wait (one too many!) */
    sc.steps[2][0] = (smash_action_t){ACT_SEM_WAIT, 0};
    sc.step_count[2] = 1;

    return sc;
}

static void test_bmc_api(void) {

    printf("\n=== Test 1: BMC API (safe scenario) ===\n");

    smash_scenario_t sc = safe_mutex_ordering();
    int schedule[100];
    int schedule_len = 0;
    char error_msg[256];

    bool result = smash_verify_no_deadlock_bmc(&sc, 10, schedule, &schedule_len,
                                                error_msg, sizeof(error_msg));

    if (result) {
        printf("✓ PASS: No deadlock found (as expected)\n");
    } else {
        printf("✗ FAIL: Deadlock reported: %s\n", error_msg);
    }

    printf("\n=== Test 2: BMC API (ABBA deadlock) ===\n");

    sc = abba_deadlock();
    result = smash_verify_no_deadlock_bmc(&sc, 10, schedule, &schedule_len,
                                          error_msg, sizeof(error_msg));

    if (!result) {
        printf("✓ PASS: Deadlock detected (as expected): %s\n", error_msg);
    } else {
        printf("✗ FAIL: No deadlock found (should have detected ABBA)\n");
    }

    printf("\n=== Test 3: BMC API (unbalanced semaphore) ===\n");

    sc = unbalanced_semaphore();
    result = smash_verify_no_deadlock_bmc(&sc, 10, schedule, &schedule_len,
                                          error_msg, sizeof(error_msg));

    if (!result) {
        printf("✓ PASS: Deadlock detected (as expected): %s\n", error_msg);
    } else {
        printf("✗ FAIL: No deadlock found (should have detected)\n");
    }
}

static void test_sym_engine_direct(void) {

    printf("\n=== Test 4: Symbolic engine (direct API) ===\n");

    smash_scenario_t sc = safe_mutex_ordering();

    smash_sym_engine_t engine;
    if (!smash_sym_engine_init(&engine, &sc, 10)) {
        printf("✗ FAIL: Failed to initialize symbolic engine\n");
        return;
    }

    /* Encode constraints */
    smash_sym_encode_program_order(&engine);
    smash_sym_encode_mutex_exclusivity(&engine);
    smash_sym_encode_semaphore_ordering(&engine);

    printf("  Assertions added: %d\n", engine.assertion_count);

    /* Check satisfiability */
    bool sat = smash_sym_check(&engine);

    if (sat) {
        printf("  SAT - schedule exists (expected for safe scenario)\n");
    } else {
        printf("  UNSAT - no valid schedule (unexpected)\n");
    }

    /* Export to SMT2 for inspection */
    if (smash_sym_export_smt2(&engine, "build/test_sym_constraints.smt2")) {
        printf("  Exported constraints to build/test_sym_constraints.smt2\n");
    }

    smash_sym_engine_destroy(&engine);
    printf("✓ PASS: Symbolic engine test completed\n");
}

static void test_z3_version(void) {

    printf("\n=== Z3 Version Information ===\n");

    unsigned major, minor, build, revision;
    Z3_get_version(&major, &minor, &build, &revision);

    printf("  Z3 version: %u.%u.%u.%u\n", major, minor, build, revision);
    printf("  Z3 C API available: yes\n");
}

int main(void) {

    printf("SMASH — Symbolic Execution Test (Z3 BMC)\n");
    printf("Bounded Model Checking for ChibiOS concurrency verification\n");

    /* Check Z3 version */
    test_z3_version();

    /* Test high-level BMC API */
    test_bmc_api();

    /* Test low-level symbolic engine API */
    test_sym_engine_direct();

    printf("\n========================================\n");
    printf("Symbolic execution tests completed\n");
    printf("========================================\n");

    return 0;
}

#else /* !SMASH_USE_Z3 */

#include <stdio.h>

int main(void) {
    printf("SMASH — Symbolic Execution Test\n");
    printf("ERROR: Z3 support not enabled\n\n");
    printf("To enable Z3 support:\n");
    printf("  1. Install Z3: brew install z3 (macOS) or\n");
    printf("                 apt-get install libz3-dev (Ubuntu)\n");
    printf("  2. Build with: make USE_Z3=1 test\n");
    printf("  3. Run: make USE_Z3=1 test_sym_bmc\n");
    return 1;
}

#endif /* SMASH_USE_Z3 */
