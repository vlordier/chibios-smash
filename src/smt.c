/*
 * SMASH - SMT-LIB export
 *
 * Exports scheduling constraints in SMT-LIB2 format for Z3.
 * Encodes: ordering constraints, mutex exclusivity, semaphore bounds.
 */

#include "smash.h"

void smash_smt_export(const smash_trace_t *trace,
                      const smash_scenario_t *scenario, FILE *out) {

    fprintf(out, "; SMASH SMT-LIB2 export\n");
    fprintf(out, "; Scenario: %s\n", scenario->name);
    fprintf(out, "; Schedule length: %d\n\n", trace->schedule_len);

    fprintf(out, "(set-logic QF_LIA)\n\n");

    /* Declare step variables: step_T<tid>_<pc> = global order position. */
    for (int t = 0; t < scenario->thread_count; t++) {
        for (int s = 0; s < scenario->step_count[t]; s++) {
            fprintf(out, "(declare-const step_T%d_%d Int)\n", t, s);
        }
    }
    fprintf(out, "\n");

    /* All steps are non-negative. */
    for (int t = 0; t < scenario->thread_count; t++) {
        for (int s = 0; s < scenario->step_count[t]; s++) {
            fprintf(out, "(assert (>= step_T%d_%d 0))\n", t, s);
        }
    }
    fprintf(out, "\n");

    /* Per-thread program order: step_T<t>_<s> < step_T<t>_<s+1>. */
    fprintf(out, "; Program order\n");
    for (int t = 0; t < scenario->thread_count; t++) {
        for (int s = 0; s + 1 < scenario->step_count[t]; s++) {
            fprintf(out, "(assert (< step_T%d_%d step_T%d_%d))\n",
                    t, s, t, s + 1);
        }
    }
    fprintf(out, "\n");

    /* All steps have distinct global positions. */
    fprintf(out, "; Distinctness\n");
    fprintf(out, "(assert (distinct");
    for (int t = 0; t < scenario->thread_count; t++) {
        for (int s = 0; s < scenario->step_count[t]; s++) {
            fprintf(out, " step_T%d_%d", t, s);
        }
    }
    fprintf(out, "))\n\n");

    /* Mutex exclusivity: lock-unlock pairs must not overlap across threads. */
    fprintf(out, "; Mutex exclusivity constraints\n");
    for (int t1 = 0; t1 < scenario->thread_count; t1++) {
        for (int s1 = 0; s1 < scenario->step_count[t1]; s1++) {
            if (scenario->steps[t1][s1].type != ACT_MUTEX_LOCK) continue;

            int res1 = scenario->steps[t1][s1].resource_id;

            /* Find matching unlock. */
            int u1 = -1;
            for (int k = s1 + 1; k < scenario->step_count[t1]; k++) {
                if (scenario->steps[t1][k].type == ACT_MUTEX_UNLOCK &&
                    scenario->steps[t1][k].resource_id == res1) {
                    u1 = k;
                    break;
                }
            }
            if (u1 < 0) continue;

            /* For every other thread's lock of the same mutex. */
            for (int t2 = t1 + 1; t2 < scenario->thread_count; t2++) {
                for (int s2 = 0; s2 < scenario->step_count[t2]; s2++) {
                    if (scenario->steps[t2][s2].type != ACT_MUTEX_LOCK) continue;
                    if (scenario->steps[t2][s2].resource_id != res1) continue;

                    int u2 = -1;
                    for (int k = s2 + 1; k < scenario->step_count[t2]; k++) {
                        if (scenario->steps[t2][k].type == ACT_MUTEX_UNLOCK &&
                            scenario->steps[t2][k].resource_id == res1) {
                            u2 = k;
                            break;
                        }
                    }
                    if (u2 < 0) continue;

                    /* Non-overlap: T1 unlocks before T2 locks,
                     * or T2 unlocks before T1 locks. */
                    fprintf(out,
                            "(assert (or (< step_T%d_%d step_T%d_%d) "
                            "(< step_T%d_%d step_T%d_%d)))\n",
                            t1, u1, t2, s2,
                            t2, u2, t1, s1);
                }
            }
        }
    }
    fprintf(out, "\n");

    /* Semaphore ordering: for each wait on an initially-empty semaphore,
     * at least one signal must happen before it.  Emit a proper disjunctive
     * assertion (not a comment) so the solver actually enforces it. */
    fprintf(out, "; Semaphore ordering constraints\n");
    for (int t1 = 0; t1 < scenario->thread_count; t1++) {
        for (int s1 = 0; s1 < scenario->step_count[t1]; s1++) {
            if (scenario->steps[t1][s1].type != ACT_SEM_WAIT) continue;
            int res = scenario->steps[t1][s1].resource_id;
            if (scenario->sem_init[res] > 0) continue;

            /* Collect all signals for this semaphore. */
            int signals[SMASH_MAX_THREADS * SMASH_MAX_STEPS][2];
            int nsig = 0;
            for (int t2 = 0; t2 < scenario->thread_count; t2++) {
                for (int s2 = 0; s2 < scenario->step_count[t2]; s2++) {
                    if (scenario->steps[t2][s2].type != ACT_SEM_SIGNAL) continue;
                    if (scenario->steps[t2][s2].resource_id != res) continue;
                    if (nsig < SMASH_MAX_THREADS * SMASH_MAX_STEPS) {
                        signals[nsig][0] = t2;
                        signals[nsig][1] = s2;
                        nsig++;
                    }
                }
            }
            if (nsig == 0) continue;

            /* (or (< sig0 wait) (< sig1 wait) ...) */
            fprintf(out, "; SEM %d: wait T%d_%d requires a prior signal\n",
                    res, t1, s1);
            fprintf(out, "(assert (or");
            for (int k = 0; k < nsig; k++) {
                fprintf(out, " (< step_T%d_%d step_T%d_%d)",
                        signals[k][0], signals[k][1], t1, s1);
            }
            fprintf(out, "))\n");
        }
    }

    fprintf(out, "\n(check-sat)\n(get-model)\n");
}
