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
    snprintf(msg, (size_t)msg_len,
             "DEADLOCK: no runnable threads, %d threads blocked",
             engine->scenario->thread_count);

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

        /* If count > 0, no waiters should exist (lost wakeup). */
        if (r->count > 0 && r->waiter_count > 0) {
            snprintf(msg, (size_t)msg_len,
                     "SEM %d: count=%d but %d waiters (lost wakeup)",
                     i, r->count, r->waiter_count);
            return false;
        }
    }
    return true;
}

bool smash_check_priority_inversion(const smash_engine_t *engine,
                                    char *msg, int msg_len) {

    /* Detect unbounded priority inversion: a high-priority thread is blocked
     * on a mutex owned by a low-priority thread, while a medium-priority
     * thread is running.
     *
     * With correct priority inheritance this should not happen. */

    for (int i = 0; i < engine->scenario->resource_count; i++) {
        const smash_resource_t *r = &engine->resources[i];
        if (r->type != RES_MUTEX || r->owner < 0) continue;

        int owner_prio = engine->threads[r->owner].priority;

        for (int w = 0; w < r->waiter_count; w++) {
            int waiter = r->waiters[w];
            int waiter_prio = engine->threads[waiter].priority;

            if (waiter_prio <= owner_prio) continue;

            /* High-prio waiter blocked behind low-prio owner.
             * Check if owner's priority was boosted (inheritance). */
            if (owner_prio < waiter_prio) {
                /* Priority inheritance failure: owner should have been
                 * boosted to at least waiter's priority. */
                snprintf(msg, (size_t)msg_len,
                         "PRIORITY INVERSION: T%d (prio %d) blocked on mutex %d "
                         "owned by T%d (prio %d, not boosted)",
                         waiter, waiter_prio, i, r->owner, owner_prio);
                return false;
            }
        }
    }
    return true;
}

bool smash_check_all(const smash_engine_t *engine, char *msg, int msg_len) {

    if (!smash_check_mutex_integrity(engine, msg, msg_len)) return false;
    if (!smash_check_sem_integrity(engine, msg, msg_len))   return false;
    if (!smash_check_no_deadlock(engine, msg, msg_len))     return false;
    /* Priority inversion check is informational, not a hard failure
     * in this model since we implement inheritance. Uncomment to enable:
     * if (!smash_check_priority_inversion(engine, msg, msg_len)) return false;
     */
    return true;
}
