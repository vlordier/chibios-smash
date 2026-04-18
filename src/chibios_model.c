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
