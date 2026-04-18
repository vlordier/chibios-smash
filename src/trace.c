/*
 * SMASH - Trace recording, replay, dump, and minimization
 */

#include "smash.h"

static const char *event_name(smash_event_type_t type) {
    switch (type) {
    case EVT_SCHEDULE:           return "SCHEDULE";
    case EVT_MUTEX_LOCK_ATTEMPT: return "MTX_LOCK_ATTEMPT";
    case EVT_MUTEX_LOCK_ACQUIRED:return "MTX_LOCK_ACQUIRED";
    case EVT_MUTEX_LOCK_BLOCKED: return "MTX_LOCK_BLOCKED";
    case EVT_MUTEX_UNLOCK:       return "MTX_UNLOCK";
    case EVT_SEM_WAIT:           return "SEM_WAIT";
    case EVT_SEM_WAIT_BLOCKED:   return "SEM_WAIT_BLOCKED";
    case EVT_SEM_SIGNAL:         return "SEM_SIGNAL";
    case EVT_SEM_WAKEUP:         return "SEM_WAKEUP";
    case EVT_YIELD:              return "YIELD";
    case EVT_THREAD_DONE:        return "THREAD_DONE";
    case EVT_DEADLOCK:           return "DEADLOCK";
    case EVT_INVARIANT_FAIL:     return "INVARIANT_FAIL";
    }
    return "UNKNOWN";
}

void smash_trace_init(smash_trace_t *trace) {
    trace->count = 0;
    trace->schedule_len = 0;
}

void smash_trace_log(smash_trace_t *trace, uint32_t step,
                     smash_event_type_t type, int tid, int res_id, int arg) {

    if (trace->count >= SMASH_MAX_TRACE) return;

    trace->events[trace->count++] = (smash_trace_event_t){
        .step        = step,
        .type        = type,
        .tid         = tid,
        .resource_id = res_id,
        .arg         = arg
    };
}

void smash_trace_dump(const smash_trace_t *trace, FILE *out) {

    fprintf(out, "=== TRACE (%d events, schedule len %d) ===\n",
            trace->count, trace->schedule_len);

    fprintf(out, "Schedule: [");
    for (int i = 0; i < trace->schedule_len; i++) {
        fprintf(out, "%sT%d", i ? "," : "", trace->schedule[i]);
    }
    fprintf(out, "]\n\n");

    for (int i = 0; i < trace->count; i++) {
        const smash_trace_event_t *e = &trace->events[i];
        /* tid == -1 for system-level events (e.g. DEADLOCK). */
        if (e->tid >= 0) {
            fprintf(out, "%4u  T%-2d  %-20s", e->step, e->tid, event_name(e->type));
        } else {
            fprintf(out, "%4u  ---  %-20s", e->step, event_name(e->type));
        }
        if (e->resource_id >= 0) {
            fprintf(out, "  res=%d", e->resource_id);
        }
        if (e->arg != 0) {
            fprintf(out, "  arg=%d", e->arg);
        }
        fprintf(out, "\n");
    }
    fprintf(out, "=== END TRACE ===\n");
}

void smash_trace_dump_json(const smash_trace_t *trace, FILE *out) {

    fprintf(out, "{\n  \"schedule\": [");
    for (int i = 0; i < trace->schedule_len; i++) {
        fprintf(out, "%s%d", i ? "," : "", trace->schedule[i]);
    }
    fprintf(out, "],\n  \"events\": [\n");

    for (int i = 0; i < trace->count; i++) {
        const smash_trace_event_t *e = &trace->events[i];
        fprintf(out, "    {\"step\":%u,\"type\":\"%s\",\"tid\":%d",
                e->step, event_name(e->type), e->tid);
        if (e->resource_id >= 0) {
            fprintf(out, ",\"resource\":%d", e->resource_id);
        }
        if (e->arg != 0) {
            fprintf(out, ",\"arg\":%d", e->arg);
        }
        fprintf(out, "}%s\n", (i < trace->count - 1) ? "," : "");
    }
    fprintf(out, "  ]\n}\n");
}

int smash_trace_minimize(smash_trace_t *trace, smash_engine_t *engine,
                         const smash_scenario_t *scenario) {

    if (trace->schedule_len == 0) return 0;

    int original_len = trace->schedule_len;
    int schedule_copy[SMASH_MAX_DEPTH];
    memcpy(schedule_copy, trace->schedule,
           (size_t)original_len * sizeof(int));

    /* Delta debugging: try removing chunks of decreasing size. */
    for (int chunk = original_len / 2; chunk >= 1; chunk /= 2) {
        for (int start = 0; start + chunk <= trace->schedule_len; ) {
            /* Try schedule with chunk removed. */
            int new_len = trace->schedule_len - chunk;
            int trial[SMASH_MAX_DEPTH];
            int j = 0;
            for (int i = 0; i < trace->schedule_len; i++) {
                if (i < start || i >= start + chunk) {
                    trial[j++] = trace->schedule[i];
                }
            }

            /* Run and check if it still fails (hard model fault, invariant
             * violation, or deadlock — all three must be covered). */
            smash_engine_init(engine, scenario);
            smash_run_schedule(engine, trial, new_len);

            bool still_fails = engine->failed;
            if (!still_fails) {
                char chk_msg[256];
                still_fails = !smash_check_all(engine, chk_msg, sizeof(chk_msg));
            }
            if (!still_fails && !smash_all_done(engine)) {
                /* Detect deadlock: schedule ended but threads are still blocked
                 * with no runnable successor. */
                int rn[SMASH_MAX_THREADS];
                still_fails = (smash_collect_runnable(engine, rn, SMASH_MAX_THREADS) == 0);
            }

            if (still_fails) {
                /* Still fails: keep shorter schedule. */
                memcpy(trace->schedule, trial, (size_t)new_len * sizeof(int));
                trace->schedule_len = new_len;
                /* Don't advance start, try removing from same position. */
            } else {
                start += chunk;
            }
        }
    }

    return original_len - trace->schedule_len;
}
