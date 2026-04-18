/*
 * SMASH - Top-level exploration engine
 *
 * DFS exploration of all thread interleavings with optional DPOR pruning
 * and state caching. Reports deadlocks and invariant violations.
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

/* Recursive DFS explorer. */
static void explore_dfs(smash_engine_t *engine,
                        const smash_config_t *config,
                        smash_result_t *result,
                        smash_dpor_t *dpor,
                        int depth) {

    if (result->failing_trace && config->stop_on_first_bug) return;
    if (result->interleavings >= config->max_interleavings) return;
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

    /* Check for deadlock. */
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
            result->pruned++;
            return;
        }
        smash_state_mark_visited(engine, hash);
    }

    result->states++;

    /* Try each runnable thread. */
    for (int i = 0; i < n; i++) {
        int tid = runnable[i];

        save_engine(engine, depth);

        /* Record schedule decision. */
        if (engine->trace.schedule_len < SMASH_MAX_DEPTH) {
            engine->trace.schedule[engine->trace.schedule_len++] = tid;
        }

        /* Execute one step. */
        smash_execute_step(engine, tid);

        /* DPOR: record this action for dependency analysis. */
        if (config->enable_dpor && dpor) {
            int pc = save_stack[depth].threads[tid].pc;
            if (pc < engine->scenario->step_count[tid]) {
                smash_action_t act = engine->scenario->steps[tid][pc];
                smash_dpor_record(dpor, tid, act.resource_id, act.type);
            }
        }

        /* Check invariants. */
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

        /* Run custom invariants. */
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
        explore_dfs(engine, config, result, dpor, depth + 1);

        restore_engine(engine, depth);

        if (result->failing_trace && config->stop_on_first_bug) return;
    }

    /* DPOR: after exploring all children, analyze for backtracking. */
    if (config->enable_dpor && dpor) {
        smash_dpor_analyze(dpor, engine);
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

    smash_dpor_t dpor;
    smash_dpor_init(&dpor);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    explore_dfs(&engine, config, &result, &dpor, 0);

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
    fprintf(out, "  Pruned (cached/DPOR)   : %llu\n", result->pruned);
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
