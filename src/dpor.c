/*
 * SMASH - Dynamic Partial Order Reduction
 *
 * Implements simplified DPOR (CHESS-style) to prune equivalent interleavings.
 * Two operations are dependent iff they access the same resource and at least
 * one is a write/lock/unlock.
 */

#include "smash.h"

void smash_dpor_init(smash_dpor_t *dpor) {

    memset(dpor, 0, sizeof(*dpor));
    for (int i = 0; i < SMASH_MAX_RESOURCES; i++) {
        dpor->last_access[i] = -1;
    }
}

/* Reset history and backtrack set, preserving the struct allocation. */
void smash_dpor_reset(smash_dpor_t *dpor) {

    dpor->history_len    = 0;
    dpor->backtrack_count = 0;
    for (int i = 0; i < SMASH_MAX_RESOURCES; i++) {
        dpor->last_access[i] = -1;
    }
}

bool smash_dpor_dependent(smash_action_type_t a_type, int a_res,
                          smash_action_type_t b_type, int b_res) {

    /* Different resources: independent. */
    if (a_res != b_res || a_res < 0) return false;

    /* Two yields or NOPs: independent. */
    if ((a_type == ACT_YIELD || a_type == ACT_NOP) &&
        (b_type == ACT_YIELD || b_type == ACT_NOP)) {
        return false;
    }

    /* Same resource, at least one is a mutation: dependent. */
    return true;
}

void smash_dpor_record(smash_dpor_t *dpor, int tid, int resource_id,
                       smash_action_type_t type) {

    if (dpor->history_len >= SMASH_MAX_DEPTH) return;

    dpor->history[dpor->history_len].tid = tid;
    dpor->history[dpor->history_len].resource_id = resource_id;
    dpor->history[dpor->history_len].type = type;
    dpor->history_len++;

    if (resource_id >= 0 && resource_id < SMASH_MAX_RESOURCES) {
        dpor->last_access[resource_id] = tid;
    }
}

void smash_dpor_analyze(smash_dpor_t *dpor, const smash_engine_t *engine) {

    /* Walk history backwards. For each step, check if an alternative
     * runnable thread would have been dependent. If so, add a
     * backtracking point. */

    for (int i = dpor->history_len - 1; i >= 0; i--) {
        int executed_tid = dpor->history[i].tid;
        int executed_res = dpor->history[i].resource_id;
        smash_action_type_t executed_type = dpor->history[i].type;

        for (int t = 0; t < engine->scenario->thread_count; t++) {
            if (t == executed_tid) continue;

            /* Check if thread t was runnable at step i.
             * Approximation: thread is runnable if it's currently READY
             * or DONE (was ready at some point). For a proper impl we'd
             * store per-step runnable sets. */
            const smash_thread_t *th = &engine->threads[t];
            if (th->state == THREAD_BLOCKED_MUTEX ||
                th->state == THREAD_BLOCKED_SEM) {
                continue;
            }

            /* What would thread t do? */
            if (th->pc >= engine->scenario->step_count[t]) continue;

            smash_action_t alt_act = engine->scenario->steps[t][th->pc];

            if (smash_dpor_dependent(executed_type, executed_res,
                                     alt_act.type, alt_act.resource_id)) {
                /* Add backtrack point. */
                if (dpor->backtrack_count < SMASH_MAX_BACKTRACK) {
                    dpor->backtrack[dpor->backtrack_count].depth = i;
                    dpor->backtrack[dpor->backtrack_count].tid = t;
                    dpor->backtrack_count++;
                }
            }
        }
    }
}

bool smash_dpor_next_backtrack(smash_dpor_t *dpor, int *out_depth, int *out_tid) {

    if (dpor->backtrack_count == 0) return false;

    dpor->backtrack_count--;
    *out_depth = dpor->backtrack[dpor->backtrack_count].depth;
    *out_tid   = dpor->backtrack[dpor->backtrack_count].tid;
    return true;
}
