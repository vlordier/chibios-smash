/*
 * SMASH - State hashing and coverage tracking
 */

#include "smash.h"

/* FNV-1a 64-bit. The non-zero seed guarantees hash != 0 for any input,
 * which lets us use 0 as the "empty" sentinel in the hash table. */
static uint64_t fnv1a(const void *data, size_t len) {

    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 0xcbf29ce484222325ULL;

    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    /* Guarantee non-zero (only affects the astronomically unlikely zero hash). */
    return h ? h : 1ULL;
}

uint64_t smash_state_hash(const smash_state_snapshot_t *snap) {

    uint8_t buf[sizeof(smash_state_snapshot_t)];
    size_t off = 0;

    memcpy(buf + off, snap->thread_states, (size_t)snap->thread_count);
    off += (size_t)snap->thread_count;
    memcpy(buf + off, snap->thread_pcs, (size_t)snap->thread_count);
    off += (size_t)snap->thread_count;
    memcpy(buf + off, snap->thread_priorities, (size_t)snap->thread_count);
    off += (size_t)snap->thread_count;
    memcpy(buf + off, snap->mutex_owners, (size_t)snap->resource_count);
    off += (size_t)snap->resource_count;
    memcpy(buf + off, snap->sem_counts, (size_t)snap->resource_count);
    off += (size_t)snap->resource_count;

    return fnv1a(buf, off);
}

bool smash_state_equal(const smash_state_snapshot_t *a,
                       const smash_state_snapshot_t *b) {

    if (a->thread_count != b->thread_count) return false;
    if (a->resource_count != b->resource_count) return false;

    for (int i = 0; i < a->thread_count; i++) {
        if (a->thread_states[i]     != b->thread_states[i])     return false;
        if (a->thread_pcs[i]        != b->thread_pcs[i])        return false;
        if (a->thread_priorities[i] != b->thread_priorities[i]) return false;
    }
    for (int i = 0; i < a->resource_count; i++) {
        if (a->mutex_owners[i] != b->mutex_owners[i]) return false;
        if (a->sem_counts[i] != b->sem_counts[i]) return false;
    }
    return true;
}

/* Open-addressing lookup: O(1) average, O(n) worst case. */
bool smash_state_visited(smash_engine_t *engine, uint64_t hash) {

    uint64_t slot = hash & (SMASH_STATE_HT_SIZE - 1U);
    for (uint64_t i = 0; i < SMASH_STATE_HT_SIZE; i++) {
        uint64_t idx = (slot + i) & (SMASH_STATE_HT_SIZE - 1U);
        if (engine->state_ht[idx] == 0)   return false;  /* empty: not found */
        if (engine->state_ht[idx] == hash) return true;
    }
    return false; /* table full — treat as not visited */
}

void smash_state_mark_visited(smash_engine_t *engine, uint64_t hash) {

    if (engine->state_hash_count >= SMASH_MAX_STATES) return;

    uint64_t slot = hash & (SMASH_STATE_HT_SIZE - 1U);
    for (uint64_t i = 0; i < SMASH_STATE_HT_SIZE; i++) {
        uint64_t idx = (slot + i) & (SMASH_STATE_HT_SIZE - 1U);
        if (engine->state_ht[idx] == 0) {
            engine->state_ht[idx] = hash;    /* no cast — stays uint64_t */
            engine->state_hash_count++;
            return;
        }
        if (engine->state_ht[idx] == hash) return; /* already present */
    }
}
