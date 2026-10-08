/*
 * AuToSyndeSiS - ATSSS-like path selection C library
 * Mode: Smallest Delay / MinRTT
 * 
 * Author: David Munstein (2025)
 */

#ifndef AUTOSYNDESIS_SMALLEST_DELAY_H
#define AUTOSYNDESIS_SMALLEST_DELAY_H

#include <stdint.h>

/**
 * Structure containing the properties and state for the MinRTT mode in st_autosyndesis_state_t::mode_properties
 * Must be initialized with 0 before the first packet is sent
 * @param switch_margin_us the margin for switching to another path
 * @param last_path the last path used
 */
typedef struct st_autosyndesis_minrtt_props_t {
    uint64_t switch_margin_us;
    int last_path;
} autosyndesis_minrtt_props_t;

#endif
