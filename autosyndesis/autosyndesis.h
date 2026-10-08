/*
 * AuToSyndeSiS - ATSSS-like path selection C library
 *
 * Author: David Munstein (2025)
 */

#ifndef AUTOSYNDESIS_H
#define AUTOSYNDESIS_H

#include <stdint.h>
#include <stdio.h>
#include <femto.h>

#define MAX_PATHS 10
#define LINK_DOWN_THRESHOLD 2000000
#define LINK_STAT_BUFFER_SIZE 16
#define DEBUG 1

typedef enum {
    MODE_ACTIVE_STANDBY,  // Active-Standby
    MODE_SMALLEST_DELAY,  // MinRTT / Smallest Delay
    MODE_LOAD_BALANCING,  // Round Robin
    MODE_MP_DUPLICATION  // Opportunistic Redundant / Multipath Duplication
} autosyndesis_atsss_mode;

/* Structure for storing the statistics for each path */
typedef struct st_autosyndesis_link_stat_t {
    uint8_t index;  // index for the circular buffer
    uint64_t rtt_us[LINK_STAT_BUFFER_SIZE];  // 0 = unknown or < 1µs , >LINK_DOWN_THRESHOLD = link down
} autosyndesis_link_stat_t;

/* Structure which defines one AuToSyndeSiS instance */
typedef struct st_autosyndesis_state_t {
    autosyndesis_atsss_mode current_mode;  // current ATSSS mode
    void *mode_properties;  // properties for the current mode
    int num_paths;  // number of paths managed by the ATSSS instance
    uint8_t last_path_id;  // path used for the last packet sent
    uint64_t packet_counter;  // counter for the number of packets sent
    autosyndesis_link_stat_t path_stats[MAX_PATHS];  // statistics for each path
} autosyndesis_state_t;

/* autosyndesis.c */

int autosyndesis_init(autosyndesis_state_t* state, int num_paths);
int autosyndesis_set_mode(autosyndesis_state_t* state, autosyndesis_atsss_mode mode, void *mode_properties);
int autosyndesis_determine_path(autosyndesis_state_t* state);
int autosyndesis_update_link_properties(autosyndesis_state_t* state, int path_id, uint32_t delay);

/* autosyndesis_*.c */

int autosyndesis_determine_path_active_standby(autosyndesis_state_t* state);
int autosyndesis_determine_path_smallest_delay(autosyndesis_state_t* state);
int autosyndesis_determine_path_loadbalance_rr(autosyndesis_state_t* state);

#endif
