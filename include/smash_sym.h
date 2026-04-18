/*
 * SMASH - Symbolic Execution Engine with Z3
 *
 * Bounded model checking for ChibiOS concurrency verification.
 * Encodes scheduling constraints, mutex exclusivity, and semaphore
 * ordering as SMT formulas for Z3 to solve.
 *
 * Architecture:
 *
 *  [ Symbolic State ]  --encodes-->  [ SMT Constraints ]
 *       |                                  |
 *       |  Thread states, PCs              |  Program order
 *       |  Resource ownership              |  Mutex exclusivity
 *       |  Priority relations              |  Semaphore bounds
 *       v                                  v
 *  [ Path Constraints ]  --combine-->  [ Z3 Solver ]  --sat/unsat-->
 *
 * Usage:
 *   1. Create symbolic engine with smash_sym_engine_init()
 *   2. Add path constraints for each step
 *   3. Add safety property (e.g., "no deadlock")
 *   4. Call smash_sym_check() to verify
 *   5. If SAT, extract counterexample schedule
 *
 * References:
 *   - Z3 C API: https://github.com/Z3Prover/z3
 *   - Bounded Model Checking: Biere et al., 1999
 */

#ifndef SMASH_SYM_H
#define SMASH_SYM_H

#include "smash.h"  /* For smash_scenario_t, smash_action_t, etc. */
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

#define SMASH_SYM_MAX_STEPS     100    /* Max symbolic steps per thread */
#define SMASH_SYM_MAX_THREADS   16     /* Must match SMASH_MAX_THREADS */
#define SMASH_SYM_MAX_RESOURCES 16     /* Must match SMASH_MAX_RESOURCES */

/*===========================================================================*/
/* Z3 context wrapper                                                        */
/*===========================================================================*/

/* Opaque handle to Z3 context. Actual Z3 types are in z3.h. */
typedef struct smash_sym_context smash_sym_context_t;

/*===========================================================================*/
/* Symbolic variables                                                        */
/*===========================================================================*/

/* Symbolic step variables: step_T<t>_<s> represents the global order
 * position of thread t's step s. These are Z3 integer variables. */
typedef struct {
    int tid;           /* Thread ID */
    int step;          /* Step index within thread */
    int z3_var_id;     /* Z3 variable identifier */
} smash_sym_step_t;

/* Symbolic resource state at step k. */
typedef struct {
    int resource_id;
    int step;
    int owner_var;     /* Symbolic owner (or -1 if free) */
    int count_var;     /* Symbolic semaphore count */
} smash_sym_resource_t;

/* Symbolic thread state at step k. */
typedef struct {
    int tid;
    int step;
    int state_var;     /* THREAD_READY, BLOCKED, etc. */
    int pc_var;        /* Program counter */
    int priority_var;  /* Current priority (with inheritance) */
} smash_sym_thread_t;

/*===========================================================================*/
/* Symbolic engine                                                           */
/*===========================================================================*/

typedef struct {
    /* Z3 context */
    smash_sym_context_t *ctx;

    /* Scenario being verified */
    const smash_scenario_t *scenario;

    /* Symbolic variables */
    smash_sym_step_t steps[SMASH_SYM_MAX_THREADS][SMASH_SYM_MAX_STEPS];
    smash_sym_thread_t thread_states[SMASH_SYM_MAX_THREADS][SMASH_SYM_MAX_STEPS];
    smash_sym_resource_t resource_states[SMASH_SYM_MAX_RESOURCES][SMASH_SYM_MAX_STEPS];

    /* Z3 assertions (constraints) */
    int assertion_count;

    /* Bounded model checking parameters */
    int bound_k;              /* Current unrolling depth */
    int max_bound;            /* Maximum unrolling depth */

    /* Verification result */
    bool is_sat;              /* SAT = bug found, UNSAT = no bug within bound */
    int counterexample[SMASH_SYM_MAX_THREADS * SMASH_SYM_MAX_STEPS];
    int counterexample_len;

    /* Error message */
    char error_msg[256];
} smash_sym_engine_t;

/*===========================================================================*/
/* Symbolic engine API                                                       */
/*===========================================================================*/

/* Initialize symbolic execution engine.
 * Returns true on success, false if Z3 initialization fails. */
bool smash_sym_engine_init(smash_sym_engine_t *engine,
                           const smash_scenario_t *scenario,
                           int max_bound);

/* Cleanup symbolic execution engine (releases Z3 resources). */
void smash_sym_engine_destroy(smash_sym_engine_t *engine);

/* Reset engine for new verification (keeps Z3 context). */
void smash_sym_engine_reset(smash_sym_engine_t *engine);

/* Encode program order constraints:
 *   step_T<t>_<s> < step_T<t>_<s+1> for all threads t and steps s. */
bool smash_sym_encode_program_order(smash_sym_engine_t *engine);

/* Encode mutex exclusivity constraints:
 *   For each mutex, lock-unlock intervals from different threads
 *   must not overlap. */
bool smash_sym_encode_mutex_exclusivity(smash_sym_engine_t *engine);

/* Encode semaphore ordering constraints:
 *   For each wait on an empty semaphore, at least one signal
 *   must precede it. */
bool smash_sym_encode_semaphore_ordering(smash_sym_engine_t *engine);

/* Encode priority inheritance constraints:
 *   If thread T_high blocks on mutex owned by T_low,
 *   then T_low's priority must be boosted >= T_high's priority. */
bool smash_sym_encode_priority_inheritance(smash_sym_engine_t *engine);

/* Encode safety property: no deadlock.
 *   At least one thread must be runnable at each step. */
bool smash_sym_encode_no_deadlock(smash_sym_engine_t *engine);

/* Encode safety property: no circular wait.
 *   Wait-for graph must be acyclic. */
bool smash_sym_encode_no_circular_wait(smash_sym_engine_t *engine);

/* Check satisfiability using Z3.
 * Returns:
 *   true  (SAT)  - Bug found, counterexample available
 *   false (UNSAT) - No bug within bound
 * Use smash_sym_get_counterexample() to retrieve the schedule. */
bool smash_sym_check(smash_sym_engine_t *engine);

/* Extract counterexample schedule from SAT model.
 * Returns pointer to schedule array, or NULL if no counterexample. */
const int* smash_sym_get_counterexample(smash_sym_engine_t *engine,
                                        int *out_len);

/* Export current constraints to SMT-LIB2 format (for debugging). */
bool smash_sym_export_smt2(smash_sym_engine_t *engine, const char *filename);

/*===========================================================================*/
/* High-level verification API                                               */
/*===========================================================================*/

/* Verify a scenario for deadlocks using bounded model checking.
 * Returns true if deadlock-free within bound, false if deadlock found.
 * If deadlock found, counterexample is stored in result_schedule. */
bool smash_verify_no_deadlock_bmc(const smash_scenario_t *scenario,
                                  int bound_k,
                                  int *result_schedule,
                                  int *result_len,
                                  char *error_msg,
                                  int error_msg_len);

/* Verify a scenario for priority inversion using BMC.
 * Returns true if no priority inversion, false if inversion found. */
bool smash_verify_no_priority_inversion_bmc(const smash_scenario_t *scenario,
                                            int bound_k,
                                            int *result_schedule,
                                            int *result_len,
                                            char *error_msg,
                                            int error_msg_len);

/* Verify all safety properties using BMC.
 * Returns true if all properties hold within bound. */
bool smash_verify_all_bmc(const smash_scenario_t *scenario,
                          int bound_k,
                          int *result_schedule,
                          int *result_len,
                          char *error_msg,
                          int error_msg_len);

#ifdef __cplusplus
}
#endif

#endif /* SMASH_SYM_H */
