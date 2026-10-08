/*
 * deduplication module 
 * tracks received packet sequence numbers to detect and discard duplicates
 * 
 * Author: Abebe S. Feleke
 */

#ifndef DEDUPLICATION_H
#define DEDUPLICATION_H

#include <stdint.h>
#include <stdbool.h>

#define DEDUP_HASH_TABLE_SIZE 4096 
#define DEDUP_MAX_SEQ_PER_BUCKET 16  

typedef struct dedup_entry_t {
    uint32_t seq_num;
    uint64_t first_arrival_time;
    uint8_t first_path_id;
    struct dedup_entry_t *next;  // for chaining in hash table
} dedup_entry_t;

typedef struct dedup_state_t {
    dedup_entry_t *hash_table[DEDUP_HASH_TABLE_SIZE];
    uint32_t total_entries;
    uint32_t highest_seq_seen;
    uint32_t lowest_seq_seen;
    bool wraparound_detected;
} dedup_state_t;

/**
 * initialize deduplication 
 * @param state 
 * @return
 */
int dedup_init(dedup_state_t *state);

/**
 * check if a sequence number has been seen before
 * if not seen, add it to the tracking structure
 * @param state 
 * @param seq_num 
 * @param path_id
 * @param arrival_time 
 * @return 
 */
bool dedup_check_and_add(dedup_state_t *state, uint32_t seq_num, uint8_t path_id, uint64_t arrival_time);

/**
 * cleanup deduplication state and free all resources
 * @param state 
 */
void dedup_cleanup(dedup_state_t *state);

/**
 * get statistics about deduplication state
 * @param state 
 * @param total_entries
 * @param duplicates_detected
 */
void dedup_get_stats(dedup_state_t *state, uint32_t *total_entries);

#endif
