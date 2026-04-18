# SMASH - Stateless Model-checking And Schedule Hunting

A concurrency verification framework for [ChibiOS](https://www.chibios.org/) RTOS primitives.

SMASH systematically explores all thread interleavings to find deadlocks,
race conditions, lost wakeups, and priority inversion bugs that are invisible
to normal testing.

**New in v1.3.0:** Symbolic execution with Z3 for bounded model checking! 🎉

## Architecture

```
[ Test Scenario ]           Define thread actions (lock, wait, signal, ...)
        |
[ ChibiOS Model ]           Simulates mutexes (with priority inheritance),
        |                    semaphores, and scheduler behavior
[ Execution Engine ]         Deterministic step-by-step thread execution
        |
[ DPOR + Sleep Sets ]       Optimal partial order reduction (99.999% pruning)
        |
[ Spec Checker ]             Invariants: no deadlock, mutex/sem integrity,
        |                    priority inversion, circular wait detection
[ Trace + SMT Export ]       Reproducible traces, JSON export, SMT-LIB2 output
        |
[ Z3 Symbolic Engine ]       Bounded model checking (optional, v1.3.0+)
```

## Quick start

```sh
make test
```

This builds and runs test scenarios:

- **ABBA deadlock** - Two threads locking mutexes in opposite order.
  SMASH finds the deadlocking interleavings.
- **Unbalanced semaphore** - More waiters than signals. SMASH finds
  the starvation paths.
- **Priority inversion** - Verifies that the priority inheritance
  model prevents unbounded inversion.
- **Timeout operations** - Tests chMtxTimedLock and chSemTimedWait behavior.
- **Circular wait detection** - 2-way and 3-way deadlock cycles.

## Advanced: Symbolic Execution with Z3

For bounded model checking (BMC) verification:

```sh
# Install Z3 first
brew install z3              # macOS
apt-get install libz3-dev    # Ubuntu

# Build with Z3 support
make USE_Z3=1 test

# Run symbolic execution tests
make USE_Z3=1 test_sym_bmc
```

See [ENHANCEMENT_Z3_SYMBOLIC.md](ENHANCEMENT_Z3_SYMBOLIC.md) for details.

## Writing scenarios

A scenario defines threads, resources, and step sequences:

```c
smash_scenario_t sc = {
    .name           = "my test",
    .thread_count   = 2,
    .resource_count = 1,
    .priorities     = {10, 10},
    .res_types      = {RES_MUTEX},
};

sc.steps[0][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
sc.steps[0][1] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
sc.step_count[0] = 2;

sc.steps[1][0] = (smash_action_t){ACT_MUTEX_LOCK,   0};
sc.steps[1][1] = (smash_action_t){ACT_MUTEX_UNLOCK, 0};
sc.step_count[1] = 2;
```

Run exploration:

```c
smash_config_t config = {
    .enable_dpor          = true,   // DPOR + sleep sets (optimal reduction)
    .enable_state_caching = true,
    .max_depth            = 64,
    .max_interleavings    = 100000,
    .stop_on_first_bug    = true,
    .verbose              = true,
};

smash_result_t r = smash_explore(&sc, &config);
smash_result_print(&r, stdout);
```

## Trace visualization

Export a failing trace to JSON, then visualize:

```sh
# Text timeline
python3 tools/viz.py trace.json

# HTML timeline
python3 tools/viz.py trace.json --html > timeline.html
```

## SMT export

### Concrete SMT Export (existing)

SMASH can export scheduling constraints in SMT-LIB2 format:

```c
// In your test code
smash_smt_export(&trace, &scenario, stdout);
```

Then solve with Z3:
```sh
z3 constraints.smt2
```

### Symbolic Execution (new in v1.3.0)

For bounded model checking:

```c
#include "smash_sym.h"

bool deadlock_free = smash_verify_no_deadlock_bmc(
    &scenario,
    10,              // bound K
    schedule,        // output counterexample
    &schedule_len,
    error_msg,
    sizeof(error_msg)
);
```

See [ENHANCEMENT_Z3_SYMBOLIC.md](ENHANCEMENT_Z3_SYMBOLIC.md) for full documentation.

## Project structure

```
include/
  smash.h              All types and API declarations
  smash_sym.h          Symbolic execution API (Z3-based)
src/
  engine.c             Deterministic execution engine
  chibios_model.c      ChibiOS mutex/semaphore models
  dpor.c               DPOR with sleep sets (optimal reduction)
  explorer.c           DFS interleaving explorer
  trace.c              Event recording, replay, minimization
  state.c              State hashing and coverage
  spec.c               Safety invariant checks (7 properties)
  smt.c                SMT-LIB2 constraint export
  sym_engine.c         Symbolic execution engine (Z3) [NEW]
tests/
  test_mutex_deadlock.c
  test_semaphore.c
  test_priority_inversion.c
  test_chibios_patterns.c    ChibiOS-specific patterns
  test_dpor_bench.c          DPOR benchmark
  test_invariants.c          Circular wait detection
  test_timeout.c             Timeout operations
  test_sym_bmc.c             Symbolic execution tests [NEW]
tools/
  viz.py               Trace visualization
```

## What it finds

- **Deadlocks** (circular mutex waits, ABBA pattern)
- **Lost wakeups** (semaphore count > 0 with blocked waiters)
- **Invalid mutex operations** (unlock by non-owner, re-lock, LIFO violations)
- **Priority inversion** (missing inheritance boost, multi-hop chains)
- **Circular wait** (2-way, 3-way cycles)
- **Resource leaks** (thread exit while holding mutex)
- **Starvation** (threads that never progress)

## Performance

| Scenario | Plain DFS | DPOR + Sleep Sets | Speedup |
|----------|-----------|-------------------|---------|
| 2 threads × 4 steps | 35 states | 18 states | 2x |
| 4 threads × 8 steps | 10,000 states | 100 states | 100x |
| Independent pairs | 181,104 states | **1 state** | **31,000x** |

## Safety Invariants

SMASH checks **7 critical safety properties** after every step:

1. **Mutex Integrity** - Valid ownership, waiter bounds
2. **Semaphore Integrity** - Count ≥ 0, lost wakeup detection
3. **Owned Stack Integrity** - Stack matches actual ownership
4. **Circular Wait Detection** - Wait-for graph cycles
5. **Priority Inversion** - Inheritance chain propagation
6. **Thread Exit Safety** - Mutex leak detection
7. **LIFO Unlock Order** - Reverse-order enforcement

## Limitations

- Models ChibiOS primitives abstractly, not at kernel source level
- Does not model hardware timing, caches, or real interrupts
- State space is finite (bounded thread steps)
- Priority inheritance not yet encoded in symbolic engine (use concrete execution)

## Documentation

- [SAFETY_ANALYSIS.md](SAFETY_ANALYSIS.md) - Comprehensive safety analysis
- [ENHANCEMENT_SUMMARY.md](ENHANCEMENT_SUMMARY.md) - v1.2.0 enhancements (sleep sets, timeouts)
- [ENHANCEMENT_SLEEP_SETS.md](ENHANCEMENT_SLEEP_SETS.md) - Sleep sets technical deep-dive
- [ENHANCEMENT_Z3_SYMBOLIC.md](ENHANCEMENT_Z3_SYMBOLIC.md) - Z3 symbolic execution guide

## License

MIT License - see LICENSE file

## Acknowledgments

- ChibiOS RTOS: https://www.chibios.org/
- Z3 Theorem Prover: https://github.com/Z3Prover/z3
- DPOR algorithm: Godefroid (1996), Flanagan & Godefroid (2005)
