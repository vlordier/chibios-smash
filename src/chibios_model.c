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

/* Remove the waiter at position idx from a resource's wait queue.
 * Uses swap-with-last to maintain a compact array in O(1). */
static void waiter_remove(smash_resource_t *res, int idx) {

    res->waiters[idx] = res->waiters[--res->waiter_count];
}

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

    /* Use-after-free: operating on a destroyed mutex. */
    if (!mtx->alive) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d mutex_lock on destroyed resource %d (use-after-free)",
                 tid, res_id);
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_USE_AFTER_FREE, tid, res_id, 0);
        return false;
    }

    /* Context check: blocking is forbidden in ISR and SYS_LOCK contexts.
     * chMtxLock is a normal-context-only API (not I-class, not S-class). */
    if (t->exec_ctx != SMASH_CTX_THREAD) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d mutex_lock called from %s context — "
                 "blocking not permitted outside thread context",
                 tid,
                 t->exec_ctx == SMASH_CTX_ISR ? "ISR" : "SYS_LOCK");
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_CONTEXT_VIOLATION, tid, res_id, (int)t->exec_ctx);
        return false;
    }

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

    /* Multi-hop priority inheritance: follow the wait-for chain upward.
     *
     * ChibiOS chmtx.c L198-247: the kernel walks up the mtxlist/WTMTX chain
     * with 'continue' until it finds a thread that doesn't need boosting or
     * is blocked on something other than a mutex (WTSEM, READY, etc.).
     *
     * Example 3-hop chain: T_high → M_A (owned by T_mid, blocked on M_B)
     * → M_B (owned by T_low). Both T_mid and T_low must be boosted.
     *
     * We stop boosting when:
     *   - current owner already has priority >= blocker's priority, OR
     *   - current owner is not blocked on a mutex (chain ends). */
    int   boost_prio = t->priority;
    int   cur_tid    = mtx->owner;
    while (cur_tid >= 0) {
        smash_thread_t *cur = &engine->threads[cur_tid];
        if (boost_prio <= cur->priority) break;   /* already high enough */
        cur->priority = boost_prio;
        if (cur->state != THREAD_BLOCKED_MUTEX) break;  /* chain ends */
        int next_mtx = cur->blocked_on;
        if (next_mtx < 0 || next_mtx >= engine->scenario->resource_count) break;
        cur_tid = engine->resources[next_mtx].owner;
    }

    smash_trace_log(&engine->trace, engine->step_counter,
                    EVT_MUTEX_LOCK_BLOCKED, tid, res_id, mtx->owner);

    return false;
}

void smash_mutex_unlock(smash_engine_t *engine, int tid, int res_id) {

    smash_resource_t *mtx   = &engine->resources[res_id];
    smash_thread_t   *owner = &engine->threads[tid];

    if (!mtx->alive) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d mutex_unlock on destroyed resource %d (use-after-free)",
                 tid, res_id);
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_USE_AFTER_FREE, tid, res_id, 0);
        return;
    }

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

        int woken = mtx->waiters[best];
        waiter_remove(mtx, best);
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

    /* Use-after-free: operating on a destroyed semaphore. */
    if (!sem->alive) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d sem_wait on destroyed resource %d (use-after-free)",
                 tid, res_id);
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_USE_AFTER_FREE, tid, res_id, 0);
        return false;
    }

    /* Context check: chSemWait is forbidden in ISR/SYS_LOCK contexts. */
    if (t->exec_ctx != SMASH_CTX_THREAD) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d sem_wait called from %s context — "
                 "blocking not permitted outside thread context",
                 tid,
                 t->exec_ctx == SMASH_CTX_ISR ? "ISR" : "SYS_LOCK");
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_CONTEXT_VIOLATION, tid, res_id, (int)t->exec_ctx);
        return false;
    }

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

    if (!sem->alive) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d sem_signal on destroyed resource %d (use-after-free)",
                 tid, res_id);
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_USE_AFTER_FREE, tid, res_id, 0);
        return;
    }

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

        int woken = sem->waiters[best];
        waiter_remove(sem, best);
        engine->threads[woken].state      = THREAD_READY;
        engine->threads[woken].blocked_on = -1;
        engine->threads[woken].pc++;      /* wait succeeded; advance past it */

        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_USE_AFTER_FREE, tid, res_id, 0);
        return;
    }

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

        int woken = sem->waiters[best];
        waiter_remove(sem, best);
        engine->threads[woken].state      = THREAD_READY;
        engine->threads[woken].blocked_on = -1;
        engine->threads[woken].pc++;      /* wait succeeded; advance past it */

        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_SEM_WAKEUP, woken, res_id, 0);
    } else {
        sem->count++;
    }
}

/*---------------------------------------------------------------------------*/
/* Condition Variable: models ChibiOS chCond operations                     */
/*---------------------------------------------------------------------------*/

/* chCondWait: atomically release mutex and wait on condition.
 * When signaled, re-acquire mutex before returning.
 * Matches chcond.c semantics. */
bool smash_cond_wait(smash_engine_t *engine, int tid, int cond_id) {

    smash_resource_t *cnt = &engine->resources[cond_id];
    smash_thread_t   *t   = &engine->threads[tid];

    if (cnt->type != RES_CONDVAR) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d: cond_wait on non-condvar resource %d", tid, cond_id);
        return false;
    }

    int mutex_id = cnt->associated_mutex;
    if (mutex_id < 0 || mutex_id >= engine->scenario->resource_count) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d: cond %d has invalid associated_mutex %d", tid, cond_id, mutex_id);
        return false;
    }

    /* Must own the associated mutex */
    if (engine->resources[mutex_id].owner != tid) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d: cond_wait without owning associated mutex %d", tid, mutex_id);
        return false;
    }

    /* Atomically: unlock mutex + block on condition */
    /* 1. Pop mutex from owned stack */
    if (!owned_pop(t, mutex_id)) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d: cond_wait LIFO violation on mutex %d", tid, mutex_id);
        return false;
    }

    /* 2. Release mutex ownership */
    engine->resources[mutex_id].owner = -1;

    /* 3. Block on condition variable */
    if (cnt->waiter_count >= SMASH_MAX_WAITERS) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "cond %d wait queue overflow", cond_id);
        return false;
    }
    cnt->waiters[cnt->waiter_count++] = tid;
    t->state      = THREAD_BLOCKED_MUTEX;  /* Reuse BLOCKED_MUTEX state */
    t->blocked_on = cond_id;

    smash_trace_log(&engine->trace, engine->step_counter,
                    EVT_SEM_WAIT_BLOCKED, tid, cond_id, 0);
    return false;  /* Thread blocked */
}

/* chCondSignal: wake one waiter on condition.
 * Woken thread will re-acquire associated mutex. */
void smash_cond_signal(smash_engine_t *engine, int tid, int cond_id) {

    smash_resource_t *cnt = &engine->resources[cond_id];
    (void)tid;  /* Signaler doesn't block */

    if (cnt->type != RES_CONDVAR) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d: cond_signal on non-condvar resource %d", tid, cond_id);
        return;
    }

    if (cnt->waiter_count > 0) {
        /* Wake highest-priority waiter */
        int best = 0;
        for (int i = 1; i < cnt->waiter_count; i++) {
            if (engine->threads[cnt->waiters[i]].priority >
                engine->threads[cnt->waiters[best]].priority) {
                best = i;
            }
        }

        int woken = cnt->waiters[best];
        waiter_remove(cnt, best);
        engine->threads[woken].state      = THREAD_READY;
        engine->threads[woken].blocked_on = -1;
        /* Note: woken thread will re-acquire mutex when it resumes */

        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_SEM_WAKEUP, woken, cond_id, 0);
    }
    /* If no waiters, signal is lost (ChibiOS behavior) */
}

/* chCondBroadcast: wake all waiters on condition. */
void smash_cond_broadcast(smash_engine_t *engine, int tid, int cond_id) {

    smash_resource_t *cnt = &engine->resources[cond_id];
    (void)tid;

    if (cnt->type != RES_CONDVAR) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d: cond_broadcast on non-condvar resource %d", tid, cond_id);
        return;
    }

    /* Wake all waiters */
    while (cnt->waiter_count > 0) {
        int woken = cnt->waiters[0];
        waiter_remove(cnt, 0);
        engine->threads[woken].state      = THREAD_READY;
        engine->threads[woken].blocked_on = -1;

        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_SEM_WAKEUP, woken, cond_id, 0);
    }
}

/*---------------------------------------------------------------------------*/
/* Mailbox: models ChibiOS chMB operations                                  */
/*---------------------------------------------------------------------------*/

/* chMBPost: post message to mailbox (FIFO).
 * Blocks if mailbox is full. */
bool smash_mb_post(smash_engine_t *engine, int tid, int mb_id, int msg) {

    smash_resource_t *mb = &engine->resources[mb_id];
    smash_thread_t   *t  = &engine->threads[tid];

    if (mb->type != RES_MAILBOX) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d: mb_post on non-mailbox resource %d", tid, mb_id);
        return false;
    }

    /* Check if mailbox is full */
    if (mb->mb_count >= mb->mb_capacity) {
        /* Block sender - store message in waiter queue as negative value */
        if (mb->waiter_count >= SMASH_MAX_WAITERS) {
            engine->failed = true;
            snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                     "mailbox %d wait queue overflow", mb_id);
            return false;
        }
        /* Encode message in waiter entry: use negative values for messages */
        mb->waiters[mb->waiter_count++] = tid;
        t->state      = THREAD_BLOCKED_SEM;
        t->blocked_on = mb_id;

        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_SEM_WAIT_BLOCKED, tid, mb_id, msg);
        return false;  /* Thread blocked */
    }

    /* Post message (FIFO: add at tail) */
    mb->mb_messages[mb->mb_tail] = msg;
    mb->mb_tail = (mb->mb_tail + 1) % mb->mb_capacity;
    mb->mb_count++;

    smash_trace_log(&engine->trace, engine->step_counter,
                    EVT_SEM_SIGNAL, tid, mb_id, msg);
    return true;  /* Success */
}

/* chMBPostFront: post message to front of mailbox (priority/LIFO). */
bool smash_mb_post_front(smash_engine_t *engine, int tid, int mb_id, int msg) {

    smash_resource_t *mb = &engine->resources[mb_id];
    smash_thread_t   *t  = &engine->threads[tid];

    if (mb->type != RES_MAILBOX) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d: mb_post_front on non-mailbox resource %d", tid, mb_id);
        return false;
    }

    /* Check if mailbox is full */
    if (mb->mb_count >= mb->mb_capacity) {
        if (mb->waiter_count >= SMASH_MAX_WAITERS) {
            engine->failed = true;
            snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                     "mailbox %d wait queue overflow", mb_id);
            return false;
        }
        mb->waiters[mb->waiter_count++] = tid;
        t->state      = THREAD_BLOCKED_SEM;
        t->blocked_on = mb_id;

        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_SEM_WAIT_BLOCKED, tid, mb_id, msg);
        return false;
    }

    /* Post at front (LIFO: add at head) */
    mb->mb_head = (mb->mb_head - 1 + mb->mb_capacity) % mb->mb_capacity;
    mb->mb_messages[mb->mb_head] = msg;
    mb->mb_count++;

    smash_trace_log(&engine->trace, engine->step_counter,
                    EVT_SEM_SIGNAL, tid, mb_id, msg);
    return true;
}

/* chMBFetch: fetch message from mailbox (FIFO).
 * Blocks if mailbox is empty. */
bool smash_mb_fetch(smash_engine_t *engine, int tid, int mb_id, int *out_msg) {

    smash_resource_t *mb = &engine->resources[mb_id];
    smash_thread_t   *t  = &engine->threads[tid];

    if (mb->type != RES_MAILBOX) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d: mb_fetch on non-mailbox resource %d", tid, mb_id);
        return false;
    }

    /* Check if mailbox is empty */
    if (mb->mb_count == 0) {
        /* Block receiver */
        if (mb->waiter_count >= SMASH_MAX_WAITERS) {
            engine->failed = true;
            snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                     "mailbox %d wait queue overflow", mb_id);
            return false;
        }
        mb->waiters[mb->waiter_count++] = tid;
        t->state      = THREAD_BLOCKED_SEM;
        t->blocked_on = mb_id;

        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_SEM_WAIT_BLOCKED, tid, mb_id, 0);
        return false;  /* Thread blocked */
    }

    /* Fetch message (FIFO: remove from head) */
    *out_msg = mb->mb_messages[mb->mb_head];
    mb->mb_head = (mb->mb_head + 1) % mb->mb_capacity;
    mb->mb_count--;

    /* Wake blocked sender if any */
    if (mb->waiter_count > 0) {
        int woken = mb->waiters[0];
        waiter_remove(mb, 0);
        engine->threads[woken].state      = THREAD_READY;
        engine->threads[woken].blocked_on = -1;
        /* Woken sender will post its message */

        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_SEM_WAKEUP, woken, mb_id, 0);
    }

    smash_trace_log(&engine->trace, engine->step_counter,
                    EVT_SEM_WAKEUP, tid, mb_id, *out_msg);
    return true;  /* Success */
}
