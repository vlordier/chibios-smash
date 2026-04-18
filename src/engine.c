/*
 * SMASH - Execution engine
 * Deterministic scheduler + step execution
 */

#include "smash.h"

void smash_engine_init(smash_engine_t *engine, const smash_scenario_t *scenario) {

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
        engine->threads[i].owned_mutex_count = 0;
    }

    /* Init resources. */
    for (int i = 0; i < sc->resource_count; i++) {
        engine->resources[i].type         = sc->res_types[i];
        engine->resources[i].id           = i;
        engine->resources[i].owner        = -1;
        engine->resources[i].owner_orig_prio = -1;
        engine->resources[i].count        = sc->sem_init[i];
        engine->resources[i].waiter_count = 0;
    }

    smash_trace_init(&engine->trace);
    engine->step_counter = 0;
    engine->failed = false;
    engine->fail_msg[0] = '\0';
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

    smash_trace_log(&engine->trace, engine->step_counter,
                    EVT_SCHEDULE, tid, -1, t->priority);

    switch (act.type) {
    case ACT_MUTEX_LOCK:
        if (smash_mutex_lock(engine, tid, act.resource_id)) {
            t->pc++;
        }
        /* If blocked, pc stays (will retry when unblocked). */
        break;

    case ACT_MUTEX_UNLOCK:
        smash_mutex_unlock(engine, tid, act.resource_id);
        t->pc++;
        break;

    case ACT_SEM_WAIT:
        if (smash_sem_wait(engine, tid, act.resource_id)) {
            t->pc++;
        }
        break;

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
    }

    engine->step_counter++;
    return true;
}

bool smash_run_schedule(smash_engine_t *engine, const int *schedule, int len) {

    smash_engine_reset(engine);

    for (int i = 0; i < len; i++) {
        if (smash_all_done(engine)) break;

        engine->trace.schedule[engine->trace.schedule_len++] = schedule[i];

        if (!smash_execute_step(engine, schedule[i])) {
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
        snap.thread_states[i] = (uint8_t)engine->threads[i].state;
        snap.thread_pcs[i]    = (uint8_t)engine->threads[i].pc;
    }

    for (int i = 0; i < snap.resource_count; i++) {
        snap.mutex_owners[i] = (int8_t)engine->resources[i].owner;
        snap.sem_counts[i]   = (int8_t)engine->resources[i].count;
    }

    return snap;
}
