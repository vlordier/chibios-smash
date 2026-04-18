/*
 * SMASH - ChibiOS primitive models
 * Models mutex (with priority inheritance) and semaphore behavior.
 */

#include "smash.h"

/*---------------------------------------------------------------------------*/
/* Mutex: models ChibiOS chMtxLock / chMtxUnlock with priority inheritance   */
/*---------------------------------------------------------------------------*/

bool smash_mutex_lock(smash_engine_t *engine, int tid, int res_id) {

    smash_resource_t *mtx = &engine->resources[res_id];
    smash_thread_t *t = &engine->threads[tid];

    smash_trace_log(&engine->trace, engine->step_counter,
                    EVT_MUTEX_LOCK_ATTEMPT, tid, res_id, 0);

    if (mtx->owner == -1) {
        /* Free: acquire immediately. */
        mtx->owner = tid;
        mtx->owner_orig_prio = t->priority;
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_MUTEX_LOCK_ACQUIRED, tid, res_id, 0);
        return true;
    }

    if (mtx->owner == tid) {
        /* Re-lock by same thread: ChibiOS does not allow this (undefined).
         * Flag as invariant violation. */
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d re-locked mutex %d (already owner)", tid, res_id);
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_INVARIANT_FAIL, tid, res_id, 0);
        return false;
    }

    /* Blocked: add to wait queue. */
    if (mtx->waiter_count < SMASH_MAX_WAITERS) {
        mtx->waiters[mtx->waiter_count++] = tid;
    }
    t->state = THREAD_BLOCKED_MUTEX;
    t->blocked_on = res_id;

    /* Priority inheritance: boost owner if waiter has higher priority.
     * ChibiOS: higher numeric priority = higher priority. */
    smash_thread_t *owner = &engine->threads[mtx->owner];
    if (t->priority > owner->priority) {
        owner->priority = t->priority;
    }

    smash_trace_log(&engine->trace, engine->step_counter,
                    EVT_MUTEX_LOCK_BLOCKED, tid, res_id, mtx->owner);

    return false;
}

void smash_mutex_unlock(smash_engine_t *engine, int tid, int res_id) {

    smash_resource_t *mtx = &engine->resources[res_id];
    smash_thread_t *owner = &engine->threads[tid];

    if (mtx->owner != tid) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d unlocked mutex %d owned by T%d", tid, res_id, mtx->owner);
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_INVARIANT_FAIL, tid, res_id, mtx->owner);
        return;
    }

    /* ChibiOS LIFO unlock constraint: the mutex being unlocked must be the
     * most recently locked one (top of the thread's owned-mutex stack).
     * chmtx.c L378: chDbgAssert(currtp->mtxlist == mp, "not next in list")
     *
     * We track the owned-mutex stack as a per-thread linked list.
     * Check that this mutex is at the top (it was the last one locked). */
    {
        /* Find this mutex in the thread's ownership chain.
         * The mutex 'owned_stack' is tracked implicitly by waiter ordering.
         * For the LIFO check we scan the resource array: the most recently
         * acquired mutex by this thread is the one locked at the highest
         * step counter. We flag out-of-order unlock if the thread owns
         * another mutex that was locked after this one. */
        int last_locked_step = -1;
        int last_locked_res  = -1;
        for (int r = 0; r < engine->scenario->resource_count; r++) {
            if (r == res_id) continue;
            if (engine->resources[r].type != RES_MUTEX) continue;
            if (engine->resources[r].owner != tid) continue;
            /* Thread owns resource r — it was locked before or after res_id.
             * We use the thread PC as a proxy: the mutex locked at a higher
             * step in the thread's program is the one that should be
             * unlocked first. Find which step each mutex was locked at. */
            int lock_step_this = -1, lock_step_other = -1;
            for (int s = 0; s < engine->scenario->step_count[tid]; s++) {
                smash_action_t a = engine->scenario->steps[tid][s];
                if (a.type == ACT_MUTEX_LOCK) {
                    if (a.resource_id == res_id)  lock_step_this  = s;
                    if (a.resource_id == r)        lock_step_other = s;
                }
            }
            if (lock_step_other > lock_step_this) {
                /* Another mutex (r) was locked AFTER res_id — it must be
                 * unlocked first. Unlocking res_id now is out of order. */
                last_locked_step = lock_step_other;
                last_locked_res  = r;
            }
        }
        if (last_locked_res >= 0) {
            engine->failed = true;
            snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                     "T%d: LIFO violation — unlocking mutex %d before mutex %d "
                     "(locked later at step %d). ChibiOS assertion: 'not next in list'",
                     tid, res_id, last_locked_res, last_locked_step);
            smash_trace_log(&engine->trace, engine->step_counter,
                            EVT_INVARIANT_FAIL, tid, res_id, last_locked_res);
            return;
        }
    }

    /* Restore original priority (undo inheritance). */
    owner->priority = mtx->owner_orig_prio;

    smash_trace_log(&engine->trace, engine->step_counter,
                    EVT_MUTEX_UNLOCK, tid, res_id, 0);

    /* Wake highest-priority waiter (ChibiOS priority-ordered). */
    if (mtx->waiter_count > 0) {
        int best = 0;
        for (int i = 1; i < mtx->waiter_count; i++) {
            int wi = mtx->waiters[i];
            int wb = mtx->waiters[best];
            if (engine->threads[wi].priority > engine->threads[wb].priority) {
                best = i;
            }
        }

        int woken = mtx->waiters[best];
        /* Remove from wait queue. */
        mtx->waiters[best] = mtx->waiters[--mtx->waiter_count];

        /* Transfer ownership. */
        mtx->owner = woken;
        mtx->owner_orig_prio = engine->threads[woken].priority;
        engine->threads[woken].state = THREAD_READY;
        engine->threads[woken].blocked_on = -1;
        engine->threads[woken].pc++; /* advance past the lock step */

        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_MUTEX_LOCK_ACQUIRED, woken, res_id, 0);
    } else {
        mtx->owner = -1;
        mtx->owner_orig_prio = -1;
    }
}

/*---------------------------------------------------------------------------*/
/* Semaphore: models ChibiOS chSemWait / chSemSignal                         */
/*---------------------------------------------------------------------------*/

bool smash_sem_wait(smash_engine_t *engine, int tid, int res_id) {

    smash_resource_t *sem = &engine->resources[res_id];
    smash_thread_t *t = &engine->threads[tid];

    smash_trace_log(&engine->trace, engine->step_counter,
                    EVT_SEM_WAIT, tid, res_id, sem->count);

    if (sem->count > 0) {
        sem->count--;
        return true;
    }

    /* Blocked. */
    if (sem->waiter_count < SMASH_MAX_WAITERS) {
        sem->waiters[sem->waiter_count++] = tid;
    }
    t->state = THREAD_BLOCKED_SEM;
    t->blocked_on = res_id;

    smash_trace_log(&engine->trace, engine->step_counter,
                    EVT_SEM_WAIT_BLOCKED, tid, res_id, 0);

    return false;
}

void smash_sem_signal(smash_engine_t *engine, int tid, int res_id) {

    smash_resource_t *sem = &engine->resources[res_id];

    smash_trace_log(&engine->trace, engine->step_counter,
                    EVT_SEM_SIGNAL, tid, res_id, sem->count);

    if (sem->waiter_count > 0) {
        /* Wake highest-priority waiter. */
        int best = 0;
        for (int i = 1; i < sem->waiter_count; i++) {
            int wi = sem->waiters[i];
            int wb = sem->waiters[best];
            if (engine->threads[wi].priority > engine->threads[wb].priority) {
                best = i;
            }
        }

        int woken = sem->waiters[best];
        sem->waiters[best] = sem->waiters[--sem->waiter_count];

        engine->threads[woken].state = THREAD_READY;
        engine->threads[woken].blocked_on = -1;
        engine->threads[woken].pc++; /* advance past the wait step */

        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_SEM_WAKEUP, woken, res_id, 0);
    } else {
        sem->count++;
    }
}
