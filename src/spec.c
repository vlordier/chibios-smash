/*
 * SMASH - Invariant / spec checker
 *
 * Checks safety properties that must hold at every reachable state:
 *   - No deadlock (runnable threads exist unless all done)
 *   - Mutex integrity (at most one owner, valid ownership)
 *   - Semaphore integrity (count >= 0)
 *   - Priority inversion detection
 */

#include "smash.h"

bool smash_check_no_deadlock(const smash_engine_t *engine,
                             char *msg, int msg_len) {

    if (smash_all_done(engine)) return true;

    int runnable[SMASH_MAX_THREADS];
    int n = smash_collect_runnable(engine, runnable, SMASH_MAX_THREADS);

    if (n > 0) return true;

    /* No runnable threads and not all done: deadlock. */
    int blocked_count = 0;
    for (int i = 0; i < engine->scenario->thread_count; i++) {
        if (engine->threads[i].state == THREAD_BLOCKED_MUTEX ||
            engine->threads[i].state == THREAD_BLOCKED_SEM) {
            blocked_count++;
        }
    }
    snprintf(msg, (size_t)msg_len,
             "DEADLOCK: no runnable threads, %d/%d threads blocked",
             blocked_count, engine->scenario->thread_count);

    /* Report who is blocked on what. */
    int off = (int)strlen(msg);
    for (int i = 0; i < engine->scenario->thread_count; i++) {
        const smash_thread_t *t = &engine->threads[i];
        if (t->state == THREAD_BLOCKED_MUTEX || t->state == THREAD_BLOCKED_SEM) {
            off += snprintf(msg + off, (size_t)(msg_len - off),
                            "\n  T%d blocked on %s %d",
                            i,
                            t->state == THREAD_BLOCKED_MUTEX ? "mutex" : "sem",
                            t->blocked_on);
            if (off >= msg_len) break;
        }
    }
    return false;
}

bool smash_check_mutex_integrity(const smash_engine_t *engine,
                                 char *msg, int msg_len) {

    for (int i = 0; i < engine->scenario->resource_count; i++) {
        const smash_resource_t *r = &engine->resources[i];
        if (r->type != RES_MUTEX) continue;

        /* Owner must be valid thread or -1. */
        if (r->owner >= engine->scenario->thread_count) {
            snprintf(msg, (size_t)msg_len,
                     "MUTEX %d: invalid owner T%d", i, r->owner);
            return false;
        }

        /* Owner thread must not be done or in an impossible state.
         * NOTE: a thread MAY own a mutex while blocked waiting for another
         * mutex — that is the normal state in a deadlock scenario and is
         * intentional. Only flag impossible ownership states. */
        if (r->owner >= 0 &&
            engine->threads[r->owner].state == THREAD_DONE) {
            snprintf(msg, (size_t)msg_len,
                     "MUTEX %d: owner T%d has exited while holding mutex", i, r->owner);
            return false;
        }

        /* No thread should be waiting on a free mutex. */
        if (r->owner == -1 && r->waiter_count > 0) {
            snprintf(msg, (size_t)msg_len,
                     "MUTEX %d: free but has %d waiters", i, r->waiter_count);
            return false;
        }

        /* waiter_count must not exceed the queue capacity. */
        if (r->waiter_count < 0 || r->waiter_count > SMASH_MAX_WAITERS) {
            snprintf(msg, (size_t)msg_len,
                     "MUTEX %d: waiter_count %d out of range [0,%d]",
                     i, r->waiter_count, SMASH_MAX_WAITERS);
            return false;
        }

        /* Every waiter must be a valid thread ID in a blocked state. */
        for (int w = 0; w < r->waiter_count; w++) {
            int wid = r->waiters[w];
            if (wid < 0 || wid >= engine->scenario->thread_count) {
                snprintf(msg, (size_t)msg_len,
                         "MUTEX %d: waiters[%d]=%d invalid thread id", i, w, wid);
                return false;
            }
            if (engine->threads[wid].state != THREAD_BLOCKED_MUTEX) {
                snprintf(msg, (size_t)msg_len,
                         "MUTEX %d: waiter T%d is not in BLOCKED_MUTEX state",
                         i, wid);
                return false;
            }
        }
    }
    return true;
}

bool smash_check_sem_integrity(const smash_engine_t *engine,
                               char *msg, int msg_len) {

    for (int i = 0; i < engine->scenario->resource_count; i++) {
        const smash_resource_t *r = &engine->resources[i];
        if (r->type != RES_SEMAPHORE) continue;

        /* Count must not be negative. */
        if (r->count < 0) {
            snprintf(msg, (size_t)msg_len,
                     "SEM %d: negative count %d", i, r->count);
            return false;
        }

        /* waiter_count must not exceed the queue capacity. */
        if (r->waiter_count < 0 || r->waiter_count > SMASH_MAX_WAITERS) {
            snprintf(msg, (size_t)msg_len,
                     "SEM %d: waiter_count %d out of range [0,%d]",
                     i, r->waiter_count, SMASH_MAX_WAITERS);
            return false;
        }

        /* If count > 0, no waiters should exist (lost wakeup). */
        if (r->count > 0 && r->waiter_count > 0) {
            snprintf(msg, (size_t)msg_len,
                     "SEM %d: count=%d but %d waiters (lost wakeup)",
                     i, r->count, r->waiter_count);
            return false;
        }

        /* Every waiter must be a valid thread ID in a blocked state. */
        for (int w = 0; w < r->waiter_count; w++) {
            int wid = r->waiters[w];
            if (wid < 0 || wid >= engine->scenario->thread_count) {
                snprintf(msg, (size_t)msg_len,
                         "SEM %d: waiters[%d]=%d invalid thread id", i, w, wid);
                return false;
            }
            if (engine->threads[wid].state != THREAD_BLOCKED_SEM) {
                snprintf(msg, (size_t)msg_len,
                         "SEM %d: waiter T%d is not in BLOCKED_SEM state", i, wid);
                return false;
            }
        }
    }
    return true;
}

bool smash_check_priority_inversion(const smash_engine_t *engine,
                                    char *msg, int msg_len) {

    /* Detect unbounded priority inversion: a high-priority thread is blocked
     * on a mutex whose owner has a lower current priority.
     *
     * With correct (multi-hop) priority inheritance, the owner's current
     * priority should always be >= every waiter's priority.  If it is not,
     * the inheritance chain is broken and a high-priority thread can starve. */

    for (int i = 0; i < engine->scenario->resource_count; i++) {
        const smash_resource_t *r = &engine->resources[i];
        if (r->type != RES_MUTEX || r->owner < 0) continue;

        int owner_prio = engine->threads[r->owner].priority;

        for (int w = 0; w < r->waiter_count; w++) {
            int waiter      = r->waiters[w];
            int waiter_prio = engine->threads[waiter].priority;

            if (waiter_prio > owner_prio) {
                snprintf(msg, (size_t)msg_len,
                         "PRIORITY INVERSION: T%d (prio %d) blocked on mutex %d "
                         "owned by T%d (current prio %d, base %d) — "
                         "inheritance chain not propagated",
                         waiter, waiter_prio, i,
                         r->owner, owner_prio,
                         engine->threads[r->owner].base_priority);
                return false;
            }
        }
    }
    return true;
}

bool smash_check_owned_mutex_integrity(const smash_engine_t *engine,
                                       char *msg, int msg_len) {

    /* Cross-check: every mutex in a thread's owned_mutex_stack must have
     * that thread as its owner in engine->resources[].  Detects model
     * bugs where the stack and the resource ownership diverge. */
    for (int t = 0; t < engine->scenario->thread_count; t++) {
        const smash_thread_t *th = &engine->threads[t];
        for (int k = 0; k < th->owned_mutex_count; k++) {
            int res = th->owned_mutex_stack[k];
            if (res < 0 || res >= engine->scenario->resource_count) {
                snprintf(msg, (size_t)msg_len,
                         "T%d owned_mutex_stack[%d]=%d out of range", t, k, res);
                return false;
            }
            if (engine->resources[res].owner != t) {
                snprintf(msg, (size_t)msg_len,
                         "T%d owns mutex %d in stack but resource owner is T%d",
                         t, res, engine->resources[res].owner);
                return false;
            }
        }
    }
    return true;
}

bool smash_check_circular_wait(const smash_engine_t *engine,
                               char *msg, int msg_len) {

    /* Detect circular wait (Coffman condition #4) - the fundamental
     * mechanism behind deadlocks. Build a wait-for graph and check
     * for cycles.
     *
     * Wait-for graph: edge T_a -> T_b means T_a is blocked waiting
     * for a resource owned by T_b.
     *
     * A cycle in this graph indicates a deadlock. This check catches
     * circular waits BEFORE they manifest as "no runnable threads",
     * providing better diagnostic information. */

    /* Build adjacency matrix for the wait-for graph. */
    bool waits_for[SMASH_MAX_THREADS][SMASH_MAX_THREADS] = {{false}};

    for (int t = 0; t < engine->scenario->thread_count; t++) {
        const smash_thread_t *th = &engine->threads[t];
        if (th->state != THREAD_BLOCKED_MUTEX) continue;

        int res_id = th->blocked_on;
        if (res_id < 0 || res_id >= engine->scenario->resource_count) continue;

        int owner = engine->resources[res_id].owner;
        if (owner >= 0 && owner != t) {
            waits_for[t][owner] = true;
        }
    }

    /* Detect cycles using iterative DFS over the wait-for graph.
     * For each unvisited mutex-blocked thread, do a DFS and flag any
     * back-edge (node already in the current call stack). */
    bool visited[SMASH_MAX_THREADS]   = {false};
    bool in_stack[SMASH_MAX_THREADS]  = {false};
    int  cycle_path[SMASH_MAX_THREADS];
    int  dfs_stack[SMASH_MAX_THREADS];
    int  dfs_ptr[SMASH_MAX_THREADS];   /* next neighbour index to examine */
    int  top;

    for (int start = 0; start < engine->scenario->thread_count; start++) {
        if (visited[start] || engine->threads[start].state != THREAD_BLOCKED_MUTEX)
            continue;

        /* Push start node. */
        visited[start]    = true;
        in_stack[start]   = true;
        dfs_stack[0]      = start;
        dfs_ptr[0]        = 0;
        cycle_path[0]     = start;
        top               = 0;

        while (top >= 0) {
            int cur        = dfs_stack[top];
            bool found     = false;

            for (int next = dfs_ptr[top]; next < engine->scenario->thread_count; next++) {
                if (!waits_for[cur][next]) continue;
                dfs_ptr[top] = next + 1;   /* resume from next+1 on backtrack */

                if (in_stack[next]) {
                    /* Back-edge: cycle found.  Build diagnostic message. */
                    int off = snprintf(msg, (size_t)msg_len,
                                       "CIRCULAR WAIT (deadlock): ");
                    for (int p = 0; p <= top && off < msg_len; p++) {
                        off += snprintf(msg + off, (size_t)(msg_len - off),
                                        "T%d -> ", cycle_path[p]);
                    }
                    snprintf(msg + off, (size_t)(msg_len - off),
                             "T%d (blocks on T%d)", next, cycle_path[0]);
                    return false;
                }

                if (!visited[next]) {
                    visited[next]       = true;
                    in_stack[next]      = true;
                    top++;
                    dfs_stack[top]      = next;
                    dfs_ptr[top]        = 0;
                    cycle_path[top]     = next;
                    found = true;
                    break;
                }
            }

            if (!found) {
                in_stack[dfs_stack[top]] = false;
                top--;
            }
        }
    }

    return true;
}

bool smash_check_all(const smash_engine_t *engine, char *msg, int msg_len) {

    /* Structural integrity — checked after every step. */
    if (!smash_check_mutex_integrity(engine, msg, msg_len))        return false;
    if (!smash_check_sem_integrity(engine, msg, msg_len))          return false;
    if (!smash_check_owned_mutex_integrity(engine, msg, msg_len))  return false;
    /* Priority inheritance correctness — owner must be boosted to max-waiter
     * priority; if not, the inheritance chain is broken. */
    if (!smash_check_priority_inversion(engine, msg, msg_len))     return false;
    /* NOTE: circular-wait / deadlock checks must NOT be called here.
     * They are detected in explore_dfs via smash_collect_runnable()==0,
     * which increments result->deadlocks.  Including them here counts
     * deadlocks as violations and zeroes the deadlocks counter. */
    return true;
}
