/*
 * AuToSyndeSiS - ATSSS-like path selection C library
 * Mode: Active-Standby
 * 
 * Author: David Munstein (2025)
 */

#ifndef AUTOSYNDESIS_ACTV_STDBY_H
#define AUTOSYNDESIS_ACTV_STDBY_H

#include <stdint.h>

/* Structure containing the properties for the Active-Standby mode in st_autosyndesis_state_t::mode_properties */
typedef struct st_autosyndesis_actv_stdby_props_t {
    uint8_t default_link;
    uint64_t down_threshold;
} autosyndesis_actv_stdby_props_t;

#endif