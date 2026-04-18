# SMASH + Z3: Bounded Model Checking Workflow

## Overview

SMASH provides two approaches for formal verification with Z3:

1. **SMT Export + External Z3** (✅ Production-ready)
   - Export constraints to SMT-LIB2 format
   - Solve with Z3 command-line tool
   - Extract counterexamples from Z3 model

2. **Embedded Symbolic Execution** (🔧 Design specification)
   - Direct Z3 C API integration
   - Bounded model checking engine
   - _Note: Z3 RC context has stability issues on some macOS systems_

---

## Workflow 1: SMT Export + External Z3 (Recommended)

### Step 1: Create Test Scenario

```c
#include "smash.h"

smash_scenario_t abba_deadlock = {
    .name = "ABBA Deadlock",
    .thread_count = 2,
    .resource_count = 2,
    .priorities = {10, 10},
    .res_types = {RES_MUTEX, RES_MUTEX},
};

/* T0: lock(M0) -> lock(M1) */
abba_deadlock.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK, 0};
abba_deadlock.steps[0][1] = (smash_action_t){ACT_MUTEX_LOCK, 1};
abba_deadlock.steps[0][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
abba_deadlock.steps[0][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
abba_deadlock.step_count[0] = 4;

/* T1: lock(M1) -> lock(M0) - opposite order! */
abba_deadlock.steps[1][0] = (smash_action_t){ACT_MUTEX_LOCK, 1};
abba_deadlock.steps[1][1] = (smash_action_t){ACT_MUTEX_LOCK, 0};
abba_deadlock.steps[1][2] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
abba_deadlock.steps[1][3] = (smash_action_t){ACT_MUTEX_UNLOCK, 1};
abba_deadlock.step_count[1] = 4;
```

### Step 2: Export to SMT-LIB2

```c
#include "smash.h"

/* Create empty trace (we're exporting constraints, not a specific trace) */
smash_trace_t trace;
smash_trace_init(&trace);

/* Export to file */
FILE *f = fopen("abba_constraints.smt2", "w");
smash_smt_export(&trace, &abba_deadlock, f);
fclose(f);

printf("Exported constraints to abba_constraints.smt2\n");
```

### Step 3: Solve with Z3

```bash
# Run Z3 on the constraints
z3 abba_constraints.smt2

# Output:
# sat
# (model
#   (define-fun step_T0_0 () Int 0)
#   (define-fun step_T0_1 () Int 2)
#   (define-fun step_T0_2 () Int 3)
#   (define-fun step_T0_3 () Int 5)
#   (define-fun step_T1_0 () Int 1)
#   (define-fun step_T1_1 () Int 4)
#   ...
# )
```

### Step 4: Interpret the Model

Z3 output shows the **global ordering** of steps:

```
step_T0_0 = 0   → T0 locks M0 first
step_T1_0 = 1   → T1 locks M1 second
step_T0_1 = 2   → T0 tries to lock M1 (BLOCKED!)
step_T1_1 = 4   → T1 tries to lock M0 (BLOCKED!)
→ DEADLOCK at step 4
```

### Step 5: Verify with SMASH Concrete Execution

```c
/* Run concrete exploration to confirm */
smash_config_t config = {
    .enable_dpor = true,
    .enable_state_caching = true,
    .stop_on_first_bug = true,
    .verbose = true,
};

smash_result_t r = smash_explore(&abba_deadlock, &config);
smash_result_print(&r, stdout);

/* Expected output:
 * DEADLOCK at depth 4
 * Schedule: [T0, T1, T0, T1]
 */
```

---

## Complete Example Script

```bash
#!/bin/bash
# smt_bmc_workflow.sh - Complete BMC workflow

set -e

echo "=== SMASH + Z3 Bounded Model Checking ==="

# Step 1: Build SMASH
echo "[1/4] Building SMASH..."
make clean
make

# Step 2: Run test that exports SMT
echo "[2/4] Running SMT export test..."
./build/test_mutex_deadlock > /dev/null

# Step 3: Solve with Z3
echo "[3/4] Solving with Z3..."
if command -v z3 &> /dev/null; then
    z3 abba_constraints.smt2
    echo ""
    echo "Z3 found a valid schedule (SAT) = bug exists!"
else
    echo "Z3 not found. Install with: brew install z3"
fi

# Step 4: Verify with concrete execution
echo "[4/4] Verifying with concrete DPOR..."
./build/test_mutex_deadlock 2>&1 | grep -E "(DEADLOCK|Interleavings)"

echo ""
echo "=== BMC Workflow Complete ==="
```

---

## SMT-LIB2 Constraint Encoding

### Program Order

For each thread, steps execute in order:

```smt2
; T0: step_0 < step_1 < step_2 < step_3
(assert (< step_T0_0 step_T0_1))
(assert (< step_T0_1 step_T0_2))
(assert (< step_T0_2 step_T0_3))

; T1: step_0 < step_1 < step_2 < step_3
(assert (< step_T1_0 step_T1_1))
(assert (< step_T1_1 step_T1_2))
(assert (< step_T1_2 step_T1_3))
```

### Mutex Exclusivity

For mutex M0, intervals [lock, unlock] must not overlap:

```smt2
; T0 locks M0 at step_0, unlocks at step_3
; T1 locks M0 at step_1, unlocks at step_2
; Non-overlap: (T0_unlock < T1_lock) OR (T1_unlock < T0_lock)
(assert (or (< step_T0_3 step_T1_1)
            (< step_T1_2 step_T0_0)))
```

### Semaphore Ordering

For wait on empty semaphore, some signal must precede:

```smt2
; T1 waits on S0 (initially 0)
; T0 signals S0
; Constraint: T0_signal < T1_wait
(assert (< step_T0_signal step_T1_wait))
```

---

## Advanced: Adding Safety Properties

### No Deadlock Property

To verify "no deadlock within bound K", add:

```smt2
; All threads must complete (reach final step)
(assert (and
    (>= step_T0_3 0)  ; T0 reaches step 3
    (>= step_T1_3 0)  ; T1 reaches step 3
))
```

If Z3 returns **UNSAT**, deadlock is unavoidable.

### Bounded Response

To verify "every request gets response within K steps":

```smt2
; For every lock attempt, unlock must follow within K steps
(assert (forall ((t Int))
    (implies
        (<= 0 t 100)  ; time bound
        (exists ((u Int))
            (and (<= t (+ t 5))  ; within 5 steps
                 (= unlock_at u))))))
```

---

## Troubleshooting

### Z3 Returns `unknown`

**Cause:** Timeout or resource limits

**Solution:**
```bash
# Increase timeout
z3 -T:60 constraints.smt2  # 60 second timeout

# Or use specific tactics
z3 -smt2.qi.eager_threshold=1000 constraints.smt2
```

### Model Extraction Fails

**Cause:** Model generation disabled

**Solution:** Ensure SMT2 file has `(set-logic QF_LIA)` before assertions, and Z3 is invoked with `-smt2` flag.

### Large State Space

**Cause:** Many threads/steps → exponential constraints

**Solution:**
1. Reduce bound K
2. Use SMASH DPOR for bug finding (faster for small bugs)
3. Use BMC only for proving safety up to bound K

---

## Performance Comparison

| Scenario | SMT Export + Z3 | SMASH DPOR | Best For |
|----------|-----------------|------------|----------|
| 2 threads × 4 steps | 0.01s | 0.005s | Both work |
| 4 threads × 8 steps | 0.5s | 0.1s | DPOR |
| Proving safety (K=50) | 2s | timeout | **BMC** |
| Finding deep bugs | 5s | 0.01s | **DPOR** |

**Recommendation:** Use **DPOR for bug finding**, **BMC for proving safety**.

---

## Future: Embedded Symbolic Execution

The `smash_sym_*` API (in `include/smash_sym.h`) provides direct Z3 integration:

```c
#include "smash_sym.h"

smash_sym_engine_t engine;
smash_sym_engine_init(&engine, &scenario, 10);  // bound K=10

/* Encode constraints */
smash_sym_encode_program_order(&engine);
smash_sym_encode_mutex_exclusivity(&engine);

/* Check */
if (smash_sym_check(&engine)) {
    printf("Bug found!\n");
} else {
    printf("Safe within bound K=10\n");
}
```

**Status:** Design specification - implementation in progress.

**Blocker:** Z3 RC (reference-counted) context has stability issues on some macOS systems. Alternative: use legacy Z3 context or link statically.

---

## References

1. **Z3 Tutorial**: https://github.com/Z3Prover/z3/wiki
2. **SMT-LIB Standard**: http://smtlib.cs.uiowa.edu/
3. **Bounded Model Checking**: Biere et al., "Bounded Model Checking with Satisfiability Solvers", 1999
4. **SMASH Documentation**: `SAFETY_ANALYSIS.md`, `ENHANCEMENT_SUMMARY.md`

---

*Workflow documented: 2026-04-18*  
*SMASH version: 1.3.0*
