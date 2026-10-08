/*
 * deduplication module 
 * t racks received packet sequence numbers to detect and discard duplicates
 * 
 * Author: Abebe S. Feleke
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "deduplication.h"

// hash function for sequence number
static inline uint32_t dedup_hash(uint32_t seq_num)
{

    return seq_num % DEDUP_HASH_TABLE_SIZE;
}

int dedup_init(dedup_state_t *state)
{
    if (state == NULL) {
        return -1;
    }

    memset(state, 0, sizeof(dedup_state_t));
    state->total_entries = 0;
    state->highest_seq_seen = 0;
    state->lowest_seq_seen = UINT32_MAX;
    state->wraparound_detected = false;

    // initialize hash table buckets to NULL
    for (int i = 0; i < DEDUP_HASH_TABLE_SIZE; i++) {
        state->hash_table[i] = NULL;
    }

    return 0;
}

bool dedup_check_and_add(dedup_state_t *state, uint32_t seq_num, uint8_t path_id, uint64_t arrival_time)
{
    if (state == NULL) {
        return false;  // treat as new if state invalid
    }

    uint32_t hash = dedup_hash(seq_num);
    dedup_entry_t *entry = state->hash_table[hash];

    // check if sequence number already exists in this bucket
    while (entry != NULL) {
        if (entry->seq_num == seq_num) {
            // duplicate found
            return true;
        }
        entry = entry->next;
    }

    // new sequence number - add it
    dedup_entry_t *new_entry = (dedup_entry_t *)malloc(sizeof(dedup_entry_t));
    if (new_entry == NULL) {
        // memory allocation failed - treat as new to avoid dropping packets
        printf("Warning: Failed to allocate memory for deduplication entry (seq_num=%u)\n", seq_num);
        return false;
    }

    new_entry->seq_num = seq_num;
    new_entry->first_arrival_time = arrival_time;
    new_entry->first_path_id = path_id;
    new_entry->next = state->hash_table[hash];
    state->hash_table[hash] = new_entry;
    state->total_entries++;

    // update tracking statistics
    if (seq_num > state->highest_seq_seen) {
        // check for wraparound: if new seq is much smaller than highest, likely wraparound
        if (state->highest_seq_seen > UINT32_MAX - 1000000 && seq_num < 1000000) {
            state->wraparound_detected = true;
        }
        state->highest_seq_seen = seq_num;
    }
    if (seq_num < state->lowest_seq_seen) {
        state->lowest_seq_seen = seq_num;
    }

    return false;  // new packet, not a duplicate
}

void dedup_cleanup(dedup_state_t *state)
{
    if (state == NULL) {
        return;
    }

    // free all entries in hash table
    for (int i = 0; i < DEDUP_HASH_TABLE_SIZE; i++) {
        dedup_entry_t *entry = state->hash_table[i];
        while (entry != NULL) {
            dedup_entry_t *next = entry->next;
            free(entry);
            entry = next;
        }
        state->hash_table[i] = NULL;
    }

    // reset state
    state->total_entries = 0;
    state->highest_seq_seen = 0;
    state->lowest_seq_seen = UINT32_MAX;
    state->wraparound_detected = false;
}

void dedup_get_stats(dedup_state_t *state, uint32_t *total_entries)
{
    if (state == NULL || total_entries == NULL) {
        return;
    }
    *total_entries = state->total_entries;
}
