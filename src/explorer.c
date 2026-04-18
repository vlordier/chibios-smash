/*
 * SMASH - Top-level exploration engine
 *
 * DFS exploration of all thread interleavings with optional DPOR pruning
 * (persistent-sets algorithm) and state caching. Reports deadlocks and
 * invariant violations.
 *
 * DPOR algorithm: persistent sets (Godefroid 1996).
 * At each DFS node, compute the minimal "persistent set" — the smallest
 * subset of runnable threads such that every thread NOT in the set has a
 * next action that is independent (accesses a different resource) of every
 * thread IN the set.  Exploring only the persistent set is sound: any
 * omitted ordering is provably equivalent to one that is explored.
 */

#include "smash.h"
#include <time.h>

/* Saved engine state for backtracking. */
typedef struct {
    smash_thread_t   threads[SMASH_MAX_THREADS];
    smash_resource_t resources[SMASH_MAX_RESOURCES];
    smash_trace_t    trace;
    uint32_t         step_counter;
} saved_state_t;

static saved_state_t save_stack[SMASH_MAX_DEPTH];

static void save_engine(const smash_engine_t *engine, int depth) {

    saved_state_t *s = &save_stack[depth];
    memcpy(s->threads, engine->threads, sizeof(engine->threads));
    memcpy(s->resources, engine->resources, sizeof(engine->resources));
    s->trace = engine->trace;
    s->step_counter = engine->step_counter;
}

static void restore_engine(smash_engine_t *engine, int depth) {

    const saved_state_t *s = &save_stack[depth];
    memcpy(engine->threads, s->threads, sizeof(engine->threads));
    memcpy(engine->resources, s->resources, sizeof(engine->resources));
    engine->trace = s->trace;
    engine->step_counter = s->step_counter;
    engine->failed = false;
    engine->fail_msg[0] = '\0';
}

/* Return the next action thread tid will execute (ACT_DONE if finished). */
static smash_action_t thread_next_action(const smash_engine_t *engine, int tid) {

    int pc = engine->threads[tid].pc;
    if (pc < engine->scenario->step_count[tid]) {
        return engine->scenario->steps[tid][pc];
    }
    return (smash_action_t){ ACT_DONE, -1 };
}

/*
 * Compute the persistent set for the current state.
 *
 * Returns a bitmask over thread IDs (bit i set ↔ thread i must be explored).
 * Thread IDs are ≤ 15, so uint16_t is sufficient; uint32_t used for safety.
 *
 * Algorithm (fixpoint):
 *   1. Seed the set with runnable[0] — any single runnable thread works.
 *   2. For each thread q NOT yet in the set: if q's next action is dependent
 *      with ANY action of a thread already in the set, add q.
 *   3. Repeat until the set stops growing.
 *
 * Correctness: a set S is persistent iff for every execution starting at
 * this state using only threads NOT in S, all those transitions are
 * independent of every transition in S.  The fixpoint above enforces this.
 */
static uint32_t compute_persistent_set(const smash_engine_t *engine,
                                       const int *runnable, int n) {

    uint32_t set = (uint32_t)(1U << runnable[0]);
    bool changed = true;

    while (changed) {
        changed = false;
        for (int i = 0; i < n; i++) {
            int q = runnable[i];
            if (set & (uint32_t)(1U << q)) continue;   /* already in set */

            smash_action_t q_act = thread_next_action(engine, q);

            for (int j = 0; j < n; j++) {
                int p = runnable[j];
                if (!(set & (uint32_t)(1U << p))) continue;

                smash_action_t p_act = thread_next_action(engine, p);

                if (smash_dpor_dependent(p_act.type, p_act.resource_id,
                                        q_act.type, q_act.resource_id)) {
                    set |= (uint32_t)(1U << q);
                    changed = true;
                    break;
                }
            }
        }
    }
    return set;
}

/* Recursive DFS explorer. */
static void explore_dfs(smash_engine_t *engine,
                        const smash_config_t *config,
                        smash_result_t *result,
                        int depth) {

    if (result->failing_trace && config->stop_on_first_bug) return;
    if (result->interleavings >= config->max_interleavings) return;
    if (depth >= SMASH_MAX_DEPTH) return;  /* hard guard on save-stack bounds */
    if (depth >= config->max_depth) {
        if ((uint64_t)depth > engine->max_depth_reached) {
            engine->max_depth_reached = (uint64_t)depth;
        }
        return;
    }

    /* Check if all threads are done. */
    if (smash_all_done(engine)) {
        result->interleavings++;
        return;
    }

    /* Collect runnable threads. */
    int runnable[SMASH_MAX_THREADS];
    int n = smash_collect_runnable(engine, runnable, SMASH_MAX_THREADS);

    if (n == 0) {
        /* Deadlock. */
        result->deadlocks++;
        char msg[256];
        smash_check_no_deadlock(engine, msg, sizeof(msg));

        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_DEADLOCK, -1, -1, 0);

        if (config->verbose) {
            fprintf(stderr, "DEADLOCK at depth %d: %s\n", depth, msg);
        }

        if (!result->failing_trace) {
            result->failing_trace = malloc(sizeof(smash_trace_t));
            if (result->failing_trace) {
                *result->failing_trace = engine->trace;
            }
        }
        return;
    }

    /* State caching: skip if already visited. */
    if (config->enable_state_caching) {
        smash_state_snapshot_t snap = smash_capture_state(engine);
        uint64_t hash = smash_state_hash(&snap);

        if (smash_state_visited(engine, hash)) {
            result->cache_pruned++;
            return;
        }
        smash_state_mark_visited(engine, hash);
    }

    result->states++;

    /*
     * Determine which threads to explore.
     *
     * DPOR (persistent sets): compute the minimal set of threads whose
     * exploration covers all distinct outcomes.  Threads whose next action
     * is independent of every thread in the set are provably redundant and
     * can be skipped at this depth — they will be explored in the correct
     * order deeper in the DFS tree.
     *
     * Plain DFS: explore every runnable thread.
     */
    uint32_t explore_set;
    if (config->enable_dpor) {
        explore_set = compute_persistent_set(engine, runnable, n);
    } else {
        /* All runnable threads. */
        explore_set = 0;
        for (int i = 0; i < n; i++) {
            explore_set |= (uint32_t)(1U << runnable[i]);
        }
    }

    /* Try each thread in the explore set. */
    for (int i = 0; i < n; i++) {
        int tid = runnable[i];

        if (!(explore_set & (uint32_t)(1U << tid))) {
            result->dpor_pruned++;
            continue;
        }

        save_engine(engine, depth);

        /* Record schedule decision. */
        if (engine->trace.schedule_len < SMASH_MAX_DEPTH) {
            engine->trace.schedule[engine->trace.schedule_len++] = tid;
        }

        /* Execute one step. */
        smash_execute_step(engine, tid);

        /* Check engine-level invariant failures (e.g. re-lock, LIFO). */
        if (engine->failed) {
            result->violations++;
            if (config->verbose) {
                fprintf(stderr, "VIOLATION at depth %d: %s\n",
                        depth, engine->fail_msg);
            }
            if (!result->failing_trace) {
                result->failing_trace = malloc(sizeof(smash_trace_t));
                if (result->failing_trace) {
                    *result->failing_trace = engine->trace;
                }
            }
            restore_engine(engine, depth);
            if (config->stop_on_first_bug) return;
            continue;
        }

        /* Run custom / structural invariants. */
        char inv_msg[256] = {0};
        if (!smash_check_all(engine, inv_msg, sizeof(inv_msg))) {
            result->violations++;
            if (config->verbose) {
                fprintf(stderr, "INVARIANT at depth %d: %s\n", depth, inv_msg);
            }
            if (!result->failing_trace) {
                result->failing_trace = malloc(sizeof(smash_trace_t));
                if (result->failing_trace) {
                    *result->failing_trace = engine->trace;
                }
            }
            restore_engine(engine, depth);
            if (config->stop_on_first_bug) return;
            continue;
        }

        /* Recurse. */
        explore_dfs(engine, config, result, depth + 1);

        restore_engine(engine, depth);

        if (result->failing_trace && config->stop_on_first_bug) return;
    }
}

smash_result_t smash_explore(const smash_scenario_t *scenario,
                             const smash_config_t *config) {

    smash_result_t result;
    memset(&result, 0, sizeof(result));

    smash_engine_t engine;
    smash_engine_init(&engine, scenario);
    engine.dpor_enabled = config->enable_dpor;
    engine.max_depth = config->max_depth;
    engine.max_interleavings = config->max_interleavings;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    explore_dfs(&engine, config, &result, 0);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    result.elapsed_secs = (double)(t1.tv_sec - t0.tv_sec) +
                          (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    return result;
}

void smash_result_print(const smash_result_t *result, FILE *out) {

    fprintf(out, "\n");
    fprintf(out, "========================================\n");
    fprintf(out, "  SMASH exploration results\n");
    fprintf(out, "========================================\n");
    fprintf(out, "  Interleavings explored : %llu\n", result->interleavings);
    fprintf(out, "  States visited         : %llu\n", result->states);
    fprintf(out, "  Pruned by cache        : %llu\n", result->cache_pruned);
    fprintf(out, "  Pruned by DPOR         : %llu\n", result->dpor_pruned);
    fprintf(out, "  Deadlocks found        : %llu\n", result->deadlocks);
    fprintf(out, "  Invariant violations   : %llu\n", result->violations);
    fprintf(out, "  Elapsed                : %.3f s\n", result->elapsed_secs);
    fprintf(out, "========================================\n");

    if (result->failing_trace) {
        fprintf(out, "\nFirst failing trace:\n");
        smash_trace_dump(result->failing_trace, out);
    } else {
        fprintf(out, "\nNo bugs found.\n");
    }
}
