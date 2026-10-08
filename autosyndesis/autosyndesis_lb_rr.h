/*
 * AuToSyndeSiS - ATSSS-like path selection C library
 * Mode: Load-Balancing (static, round robin)
 * 
 * Author: David Munstein (2025)
 */

#ifndef AUTOSYNDESIS_LB_RR_H
#define AUTOSYNDESIS_LB_RR_H

#include <stdint.h>
#include "autosyndesis.h"

/**
 * Structure containing the properties for the Round-Robin static Load-Balancing mode in st_autosyndesis_state_t::mode_properties
 * @param share the share of packets to send on each path (sum must not exceed 255)
 * @param initial_path the path to start with
 * @param initialized whether the mode has been initialized - needs to be set to 0 before the first packet is sent and when the share array is changed
 * @param share_sum (internal) the sum of the share array
 * @param rr_sent_counter (internal) the current packet counter
 */
typedef struct st_autosyndesis_lb_rr_props_t {
    uint8_t share[MAX_PATHS];
    uint8_t initial_path;
    uint8_t packet_order[MAX_PATHS * MAX_PATHS];
    uint8_t initialized;
    uint8_t share_sum;
    uint8_t rr_sent_counter;
    uint8_t change_share;
} autosyndesis_lb_rr_props_t;

#endif
