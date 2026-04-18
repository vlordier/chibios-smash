/*
 * SMASH - ChibiOS primitive models
 *
 * Models mutex (with full priority inheritance) and semaphore behavior,
 * matching the semantics of chmtx.c and chsem.c.
 */

#include "smash.h"

/*---------------------------------------------------------------------------*/
/* Internal helpers                                                          */
/*---------------------------------------------------------------------------*/

/* Push a mutex onto the thread's owned-mutex LIFO stack.
 * Mirrors ChibiOS mtxlist (mp->next = currtp->mtxlist; currtp->mtxlist = mp). */
static void owned_push(smash_thread_t *t, int res_id) {

    if (t->owned_mutex_count < SMASH_MAX_RESOURCES) {
        t->owned_mutex_stack[t->owned_mutex_count++] = res_id;
    }
}

/* Pop the top of the owned-mutex stack and verify it matches expected res_id.
 * Returns true if the stack top matches (LIFO order respected).
 * Mirrors chmtx.c:378 chDbgAssert(currtp->mtxlist == mp, "not next in list"). */
static bool owned_pop(smash_thread_t *t, int res_id) {

    if (t->owned_mutex_count == 0) return false;
    if (t->owned_mutex_stack[t->owned_mutex_count - 1] != res_id) return false;
    t->owned_mutex_count--;
    return true;
}

/* Recalculate the thread's working priority after releasing a mutex.
 * Scans all remaining owned mutexes for the highest-priority waiter.
 * Matches chmtx.c L391-406 "recalculates the optimal thread priority". */
static int recalc_priority(const smash_engine_t *engine, int tid,
                            int released_res) {

    int p = engine->threads[tid].base_priority;

    for (int r = 0; r < engine->scenario->resource_count; r++) {
        if (r == released_res) continue;
        if (engine->resources[r].type != RES_MUTEX) continue;
        if (engine->resources[r].owner != tid) continue;

        const smash_resource_t *m = &engine->resources[r];
        for (int w = 0; w < m->waiter_count; w++) {
            int wp = engine->threads[m->waiters[w]].priority;
            if (wp > p) p = wp;
        }
    }
    return p;
}

/*---------------------------------------------------------------------------*/
/* Mutex: models ChibiOS chMtxLock / chMtxUnlock                            */
/*---------------------------------------------------------------------------*/

bool smash_mutex_lock(smash_engine_t *engine, int tid, int res_id) {

    smash_resource_t *mtx = &engine->resources[res_id];
    smash_thread_t   *t   = &engine->threads[tid];

    smash_trace_log(&engine->trace, engine->step_counter,
                    EVT_MUTEX_LOCK_ATTEMPT, tid, res_id, 0);

    if (mtx->owner == -1) {
        /* Free: acquire. */
        mtx->owner = tid;
        owned_push(t, res_id);
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_MUTEX_LOCK_ACQUIRED, tid, res_id, 0);
        return true;
    }

    if (mtx->owner == tid) {
        /* Re-lock by same thread: not allowed without CH_CFG_USE_MUTEXES_RECURSIVE. */
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d re-locked mutex %d (already owner) — "
                 "recursive mutexes not enabled", tid, res_id);
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_INVARIANT_FAIL, tid, res_id, 0);
        return false;
    }

    /* Blocked: add to wait queue. */
    if (mtx->waiter_count >= SMASH_MAX_WAITERS) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "mutex %d wait queue overflow (>%d waiters)", res_id,
                 SMASH_MAX_WAITERS);
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_INVARIANT_FAIL, tid, res_id, 0);
        return false;
    }
    mtx->waiters[mtx->waiter_count++] = tid;
    t->state      = THREAD_BLOCKED_MUTEX;
    t->blocked_on = res_id;

    /* Priority inheritance: boost owner if this thread has higher priority.
     * ChibiOS chmtx.c L198-247: follows the mutex chain upwards. */
    smash_thread_t *owner = &engine->threads[mtx->owner];
    if (t->priority > owner->priority) {
        owner->priority = t->priority;
    }

    smash_trace_log(&engine->trace, engine->step_counter,
                    EVT_MUTEX_LOCK_BLOCKED, tid, res_id, mtx->owner);

    return false;
}

void smash_mutex_unlock(smash_engine_t *engine, int tid, int res_id) {

    smash_resource_t *mtx   = &engine->resources[res_id];
    smash_thread_t   *owner = &engine->threads[tid];

    if (mtx->owner != tid) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d unlocked mutex %d owned by T%d", tid, res_id, mtx->owner);
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_INVARIANT_FAIL, tid, res_id, mtx->owner);
        return;
    }

    /* LIFO constraint: must unlock in reverse acquisition order.
     * chmtx.c:378 chDbgAssert(currtp->mtxlist == mp, "not next in list") */
    if (!owned_pop(owner, res_id)) {
        int top = (owner->owned_mutex_count > 0)
                  ? owner->owned_mutex_stack[owner->owned_mutex_count - 1]
                  : -1;
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d: LIFO violation — unlocking mutex %d but top of owned "
                 "stack is mutex %d. ChibiOS: 'not next in list'",
                 tid, res_id, top);
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_INVARIANT_FAIL, tid, res_id, top);
        return;
    }

    smash_trace_log(&engine->trace, engine->step_counter,
                    EVT_MUTEX_UNLOCK, tid, res_id, 0);

    /* Recalculate owner priority from base + remaining owned mutexes.
     * Matches chmtx.c L391-406. */
    owner->priority = recalc_priority(engine, tid, res_id);

    /* Wake highest-priority waiter. */
    if (mtx->waiter_count > 0) {
        int best = 0;
        for (int i = 1; i < mtx->waiter_count; i++) {
            if (engine->threads[mtx->waiters[i]].priority >
                engine->threads[mtx->waiters[best]].priority) {
                best = i;
            }
        }

        int woken                              = mtx->waiters[best];
        mtx->waiters[best]                     = mtx->waiters[--mtx->waiter_count];
        mtx->owner                             = woken;
        engine->threads[woken].state           = THREAD_READY;
        engine->threads[woken].blocked_on      = -1;
        engine->threads[woken].pc++;           /* lock succeeded; advance past it */
        owned_push(&engine->threads[woken], res_id);

        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_MUTEX_LOCK_ACQUIRED, woken, res_id, 0);
    } else {
        mtx->owner = -1;
    }
}

/*---------------------------------------------------------------------------*/
/* Semaphore: models ChibiOS chSemWait / chSemSignal                         */
/*---------------------------------------------------------------------------*/

bool smash_sem_wait(smash_engine_t *engine, int tid, int res_id) {

    smash_resource_t *sem = &engine->resources[res_id];
    smash_thread_t   *t   = &engine->threads[tid];

    smash_trace_log(&engine->trace, engine->step_counter,
                    EVT_SEM_WAIT, tid, res_id, sem->count);

    if (sem->count > 0) {
        sem->count--;
        return true;
    }

    if (sem->waiter_count >= SMASH_MAX_WAITERS) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "semaphore %d wait queue overflow (>%d waiters)", res_id,
                 SMASH_MAX_WAITERS);
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_INVARIANT_FAIL, tid, res_id, 0);
        return false;
    }
    sem->waiters[sem->waiter_count++] = tid;
    t->state      = THREAD_BLOCKED_SEM;
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
        /* Wake highest-priority waiter (matches CH_CFG_USE_SEMAPHORES_PRIORITY). */
        int best = 0;
        for (int i = 1; i < sem->waiter_count; i++) {
            if (engine->threads[sem->waiters[i]].priority >
                engine->threads[sem->waiters[best]].priority) {
                best = i;
            }
        }

        int woken                         = sem->waiters[best];
        sem->waiters[best]                = sem->waiters[--sem->waiter_count];
        engine->threads[woken].state      = THREAD_READY;
        engine->threads[woken].blocked_on = -1;
        engine->threads[woken].pc++;      /* wait succeeded; advance past it */

        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_SEM_WAKEUP, woken, res_id, 0);
    } else {
        sem->count++;
    }
}
