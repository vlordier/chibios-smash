/*
 * SMASH - State hashing and coverage tracking
 */

#include "smash.h"

/* FNV-1a 64-bit hash. */
static uint64_t fnv1a(const void *data, size_t len) {

    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 0xcbf29ce484222325ULL;

    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

uint64_t smash_state_hash(const smash_state_snapshot_t *snap) {

    /* Hash the meaningful portion of the snapshot. */
    size_t len = sizeof(uint8_t) * (size_t)snap->thread_count * 2 +
                 sizeof(int8_t) * (size_t)snap->resource_count * 2;
    uint8_t buf[sizeof(smash_state_snapshot_t)];
    size_t off = 0;

    memcpy(buf + off, snap->thread_states, (size_t)snap->thread_count);
    off += (size_t)snap->thread_count;
    memcpy(buf + off, snap->thread_pcs, (size_t)snap->thread_count);
    off += (size_t)snap->thread_count;
    memcpy(buf + off, snap->mutex_owners, (size_t)snap->resource_count);
    off += (size_t)snap->resource_count;
    memcpy(buf + off, snap->sem_counts, (size_t)snap->resource_count);
    off += (size_t)snap->resource_count;

    (void)len;
    return fnv1a(buf, off);
}

bool smash_state_equal(const smash_state_snapshot_t *a,
                       const smash_state_snapshot_t *b) {

    if (a->thread_count != b->thread_count) return false;
    if (a->resource_count != b->resource_count) return false;

    for (int i = 0; i < a->thread_count; i++) {
        if (a->thread_states[i] != b->thread_states[i]) return false;
        if (a->thread_pcs[i] != b->thread_pcs[i]) return false;
    }
    for (int i = 0; i < a->resource_count; i++) {
        if (a->mutex_owners[i] != b->mutex_owners[i]) return false;
        if (a->sem_counts[i] != b->sem_counts[i]) return false;
    }
    return true;
}

bool smash_state_visited(smash_engine_t *engine, uint64_t hash) {

    for (int i = 0; i < engine->state_hash_count; i++) {
        if (engine->state_hashes[i] == hash) {
            return true;
        }
    }
    return false;
}

void smash_state_mark_visited(smash_engine_t *engine, uint64_t hash) {

    if (engine->state_hash_count < SMASH_MAX_STATES) {
        engine->state_hashes[engine->state_hash_count++] = (int)hash;
    }
}
