/*
 * SMASH - Execution engine
 * Deterministic scheduler + step execution
 */

#include "smash.h"

void smash_engine_init(smash_engine_t *engine, const smash_scenario_t *scenario) {

    /* Validate scenario before touching anything — catches OOB resource IDs
     * and other mis-configurations before they cause silent corruption. */
    char val_msg[256];
    if (!smash_scenario_validate(scenario, val_msg, sizeof(val_msg))) {
        /* Scenario is invalid: mark engine as failed immediately so the
         * caller can detect the problem via engine->failed. */
        memset(engine, 0, sizeof(*engine));
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "smash_engine_init: invalid scenario: %s", val_msg);
        return;
    }

    memset(engine, 0, sizeof(*engine));
    engine->scenario = scenario;
    engine->max_depth = SMASH_MAX_DEPTH;
    engine->max_interleavings = UINT64_MAX;

    smash_engine_reset(engine);
}

void smash_engine_reset(smash_engine_t *engine) {

    const smash_scenario_t *sc = engine->scenario;

    /* Init threads. */
    for (int i = 0; i < sc->thread_count; i++) {
        engine->threads[i].id                = i;
        engine->threads[i].base_priority     = sc->priorities[i];
        engine->threads[i].priority          = sc->priorities[i];
        engine->threads[i].state             = THREAD_READY;
        engine->threads[i].pc                = 0;
        engine->threads[i].blocked_on        = -1;
        engine->threads[i].timeout_ticks     = -1;  /* -1 = infinite wait */
        engine->threads[i].owned_mutex_count = 0;
        engine->threads[i].exec_ctx          = SMASH_CTX_THREAD;
        engine->threads[i].sys_lock_depth    = 0;
        engine->threads[i].isr_depth         = 0;
        engine->threads[i].stack_depth       = 0;
        engine->threads[i].stack_watermark   = 0;
    }

    /* Init resources. */
    for (int i = 0; i < sc->resource_count; i++) {
        engine->resources[i].type         = sc->res_types[i];
        engine->resources[i].id           = i;
        engine->resources[i].owner        = -1;
        engine->resources[i].count        = sc->sem_init[i];
        engine->resources[i].waiter_count = 0;
        engine->resources[i].alive        = true;
    }

    smash_trace_init(&engine->trace);
    engine->step_counter = 0;
    engine->failed = false;
    engine->fail_msg[0] = '\0';

    /* Reset state hash table so re-calling smash_explore on the same engine
     * does not carry stale visited-state records from a previous run. */
    memset(engine->state_ht, 0, sizeof(engine->state_ht));
    engine->state_hash_count = 0;
}

int smash_collect_runnable(const smash_engine_t *engine, int *out, int max) {

    int n = 0;
    for (int i = 0; i < engine->scenario->thread_count && n < max; i++) {
        if (engine->threads[i].state == THREAD_READY) {
            out[n++] = i;
        }
    }
    return n;
}

bool smash_all_done(const smash_engine_t *engine) {

    for (int i = 0; i < engine->scenario->thread_count; i++) {
        if (engine->threads[i].state != THREAD_DONE) {
            return false;
        }
    }
    return true;
}

bool smash_execute_step(smash_engine_t *engine, int tid) {

    /* Process timeout ticks for all blocked threads with active timeouts.
     * This simulates the passage of time in ChibiOS's tick-based timeout system.
     * When a timeout expires, the thread resumes with MSG_TIMEOUT (-1). */
    for (int i = 0; i < engine->scenario->thread_count; i++) {
        smash_thread_t *bt = &engine->threads[i];
        if ((bt->state == THREAD_BLOCKED_MUTEX || bt->state == THREAD_BLOCKED_SEM)
            && bt->timeout_ticks > 0) {
            bt->timeout_ticks--;
            if (bt->timeout_ticks == 0) {
                /* Timeout expired! Resume thread with MSG_TIMEOUT. */
                bt->state = THREAD_READY;
                bt->blocked_on = -1;
                bt->pc++;  /* Advance past the timed wait/lock */
                bt->timeout_ticks = -1;

                smash_trace_log(&engine->trace, engine->step_counter,
                               bt->state == THREAD_BLOCKED_MUTEX ?
                                   EVT_MUTEX_TIMEOUT_EXPIRED :
                                   EVT_SEM_TIMEOUT_EXPIRED,
                               i, -1, 0);
            }
        }
    }

    smash_thread_t *t = &engine->threads[tid];

    if (t->state != THREAD_READY) {
        return false;
    }

    if (t->pc >= engine->scenario->step_count[tid]) {
        t->state = THREAD_DONE;
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_THREAD_DONE, tid, -1, 0);
        return true;
    }

    smash_action_t act = engine->scenario->steps[tid][t->pc];

    /* Validate resource_id before dispatching to model functions. */
    if (act.resource_id != SMASH_NO_RESOURCE &&
        (act.resource_id < 0 ||
         act.resource_id >= engine->scenario->resource_count)) {
        engine->failed = true;
        snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                 "T%d step %d: resource_id %d out of range [0,%d)",
                 tid, t->pc, act.resource_id, engine->scenario->resource_count);
        return false;
    }

    smash_trace_log(&engine->trace, engine->step_counter,
                    EVT_SCHEDULE, tid, -1, t->priority);

    switch (act.type) {
    case ACT_MUTEX_LOCK:
        if (smash_mutex_lock(engine, tid, act.resource_id)) {
            t->pc++;
        }
        /* If blocked, pc stays (will retry when unblocked). */
        break;

    case ACT_MUTEX_TIMED_LOCK: {
        /* Timed mutex lock (chMtxTimedLock).
         * act.arg carries the timeout in abstract ticks (must be > 0). */
        int ticks = (act.arg > 0) ? act.arg : 10;
        if (smash_mutex_lock(engine, tid, act.resource_id)) {
            t->pc++;
        } else if (t->state == THREAD_BLOCKED_MUTEX) {
            t->timeout_ticks = ticks;
        }
        break;
    }

    case ACT_MUTEX_UNLOCK:
        smash_mutex_unlock(engine, tid, act.resource_id);
        t->pc++;
        break;

    case ACT_SEM_WAIT:
        if (smash_sem_wait(engine, tid, act.resource_id)) {
            t->pc++;
        }
        break;

    case ACT_SEM_TIMED_WAIT: {
        /* Timed semaphore wait (chSemTimedWait).
         * act.arg carries the timeout in abstract ticks (must be > 0). */
        int ticks = (act.arg > 0) ? act.arg : 10;
        if (smash_sem_wait(engine, tid, act.resource_id)) {
            t->pc++;
        } else if (t->state == THREAD_BLOCKED_SEM) {
            t->timeout_ticks = ticks;
        }
        break;
    }

    case ACT_SEM_SIGNAL:
        smash_sem_signal(engine, tid, act.resource_id);
        t->pc++;
        break;

    case ACT_YIELD:
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_YIELD, tid, -1, 0);
        t->pc++;
        break;

    case ACT_DONE:
        /* ChibiOS does not release mutexes on thread exit — detect the leak. */
        if (t->owned_mutex_count > 0) {
            engine->failed = true;
            snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                     "T%d exited while holding %d mutex(es) — "
                     "ChibiOS does not auto-release on exit",
                     tid, t->owned_mutex_count);
            smash_trace_log(&engine->trace, engine->step_counter,
                            EVT_INVARIANT_FAIL, tid, -1, t->owned_mutex_count);
        }
        t->state = THREAD_DONE;
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_THREAD_DONE, tid, -1, 0);
        break;

    case ACT_NOP:
        t->pc++;
        break;

    case ACT_SYS_LOCK:
        /* chSysLock(): enter system-locked context. */
        t->sys_lock_depth++;
        t->exec_ctx = SMASH_CTX_SYS_LOCK;
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_SYS_LOCK, tid, -1, t->sys_lock_depth);
        t->pc++;
        break;

    case ACT_SYS_UNLOCK:
        /* chSysUnlock(): exit system-locked context.
         * Unbalanced unlock (depth already 0) is a violation. */
        if (t->sys_lock_depth <= 0) {
            engine->failed = true;
            snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                     "T%d ACT_SYS_UNLOCK with sys_lock_depth=0 (unbalanced)", tid);
            smash_trace_log(&engine->trace, engine->step_counter,
                            EVT_CONTEXT_VIOLATION, tid, -1, 0);
            return false;
        }
        t->sys_lock_depth--;
        t->exec_ctx = (t->sys_lock_depth > 0) ? SMASH_CTX_SYS_LOCK
                                               : SMASH_CTX_THREAD;
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_SYS_UNLOCK, tid, -1, t->sys_lock_depth);
        t->pc++;
        break;

    case ACT_ISR_ENTER:
        /* Entering an ISR: push context. */
        if (t->isr_depth >= SMASH_MAX_ISR_DEPTH) {
            engine->failed = true;
            snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                     "T%d ISR nesting depth %d exceeds SMASH_MAX_ISR_DEPTH=%d",
                     tid, t->isr_depth, SMASH_MAX_ISR_DEPTH);
            return false;
        }
        t->isr_depth++;
        t->exec_ctx = SMASH_CTX_ISR;
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_ISR_ENTER, tid, -1, t->isr_depth);
        t->pc++;
        break;

    case ACT_ISR_EXIT:
        /* Exiting an ISR: pop context. */
        if (t->isr_depth <= 0) {
            engine->failed = true;
            snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                     "T%d ACT_ISR_EXIT with isr_depth=0 (unbalanced)", tid);
            smash_trace_log(&engine->trace, engine->step_counter,
                            EVT_CONTEXT_VIOLATION, tid, -1, 0);
            return false;
        }
        t->isr_depth--;
        if (t->isr_depth == 0) {
            t->exec_ctx = (t->sys_lock_depth > 0) ? SMASH_CTX_SYS_LOCK
                                                   : SMASH_CTX_THREAD;
        }
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_ISR_EXIT, tid, -1, t->isr_depth);
        t->pc++;
        break;

    case ACT_OBJECT_INIT: {
        /* Mark resource as alive (idempotent re-init is allowed). */
        int rid = act.resource_id;
        engine->resources[rid].alive = true;
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_OBJECT_INIT, tid, rid, 0);
        t->pc++;
        break;
    }

    case ACT_OBJECT_DESTROY: {
        /* Mark resource as dead.  Any subsequent operation on it is
         * a use-after-free — detected by smash_check_object_lifecycle. */
        int rid = act.resource_id;
        if (!engine->resources[rid].alive) {
            engine->failed = true;
            snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                     "T%d ACT_OBJECT_DESTROY on already-dead resource %d", tid, rid);
            smash_trace_log(&engine->trace, engine->step_counter,
                            EVT_CONTEXT_VIOLATION, tid, rid, 0);
            return false;
        }
        engine->resources[rid].alive = false;
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_OBJECT_DESTROY, tid, rid, 0);
        t->pc++;
        break;
    }

    case ACT_CALL: {
        /* Simulate a function call consuming act.arg abstract stack units. */
        int units = (act.arg > 0) ? act.arg : 1;
        t->stack_depth += units;
        if (t->stack_depth > t->stack_watermark) {
            t->stack_watermark = t->stack_depth;
        }
        int limit = engine->scenario->stack_sizes[tid];
        if (limit > 0 && t->stack_depth > limit) {
            engine->failed = true;
            snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                     "T%d stack overflow: depth=%d limit=%d",
                     tid, t->stack_depth, limit);
            smash_trace_log(&engine->trace, engine->step_counter,
                            EVT_STACK_OVERFLOW, tid, -1, t->stack_depth);
            return false;
        }
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_CALL, tid, -1, t->stack_depth);
        t->pc++;
        break;
    }

    case ACT_RETURN: {
        /* Return from a function call, releasing act.arg stack units. */
        int units = (act.arg > 0) ? act.arg : 1;
        t->stack_depth -= units;
        if (t->stack_depth < 0) {
            t->stack_depth = 0;  /* clamp: unbalanced CALL/RETURN in scenario */
        }
        smash_trace_log(&engine->trace, engine->step_counter,
                        EVT_RETURN, tid, -1, t->stack_depth);
        t->pc++;
        break;
    }
    }

    engine->step_counter++;
    return true;
}

bool smash_run_schedule(smash_engine_t *engine, const int *schedule, int len) {

    smash_engine_reset(engine);

    for (int i = 0; i < len; i++) {
        if (smash_all_done(engine)) break;

        int tid = schedule[i];
        if (tid < 0 || tid >= engine->scenario->thread_count) {
            engine->failed = true;
            snprintf(engine->fail_msg, sizeof(engine->fail_msg),
                     "smash_run_schedule: schedule[%d]=%d out of range", i, tid);
            return false;
        }

        engine->trace.schedule[engine->trace.schedule_len++] = tid;

        if (!smash_execute_step(engine, tid)) {
            return false;
        }
        if (engine->failed) {
            return false;
        }
    }
    return true;
}

smash_state_snapshot_t smash_capture_state(const smash_engine_t *engine) {

    smash_state_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));

    snap.thread_count   = engine->scenario->thread_count;
    snap.resource_count = engine->scenario->resource_count;

    for (int i = 0; i < snap.thread_count; i++) {
        snap.thread_states[i]         = (uint8_t)engine->threads[i].state;
        snap.thread_pcs[i]            = (uint8_t)engine->threads[i].pc;
        snap.thread_priorities[i]     = (uint8_t)engine->threads[i].priority;
        snap.thread_exec_ctx[i]       = (uint8_t)engine->threads[i].exec_ctx;
        snap.thread_sys_lock_depth[i] = (uint8_t)engine->threads[i].sys_lock_depth;
    }

    for (int i = 0; i < snap.resource_count; i++) {
        snap.mutex_owners[i]   = (int8_t)engine->resources[i].owner;
        snap.sem_counts[i]     = (int8_t)engine->resources[i].count;
        snap.resource_alive[i] = engine->resources[i].alive ? 1u : 0u;
    }

    return snap;
}

bool smash_scenario_validate(const smash_scenario_t *scenario,
                              char *msg, size_t msg_len) {

    if (!scenario) {
        snprintf(msg, msg_len, "scenario is NULL");
        return false;
    }
    if (scenario->thread_count <= 0 ||
        scenario->thread_count > SMASH_MAX_THREADS) {
        snprintf(msg, msg_len,
                 "thread_count %d out of range [1,%d]",
                 scenario->thread_count, SMASH_MAX_THREADS);
        return false;
    }
    if (scenario->resource_count < 0 ||
        scenario->resource_count > SMASH_MAX_RESOURCES) {
        snprintf(msg, msg_len,
                 "resource_count %d out of range [0,%d]",
                 scenario->resource_count, SMASH_MAX_RESOURCES);
        return false;
    }
    for (int t = 0; t < scenario->thread_count; t++) {
        if (scenario->step_count[t] < 0 ||
            scenario->step_count[t] > SMASH_MAX_STEPS) {
            snprintf(msg, msg_len,
                     "T%d step_count %d out of range [0,%d]",
                     t, scenario->step_count[t], SMASH_MAX_STEPS);
            return false;
        }
        for (int s = 0; s < scenario->step_count[t]; s++) {
            int rid = scenario->steps[t][s].resource_id;
            if (rid != SMASH_NO_RESOURCE &&
                (rid < 0 || rid >= scenario->resource_count)) {
                snprintf(msg, msg_len,
                         "T%d step %d: resource_id %d out of range", t, s, rid);
                return false;
            }
        }
    }
    return true;
}
