/*
 * AuToSyndeSiS - ATSSS-like path selection C library
 * Mode: Active-Standby
 * 
 * Author: David Munstein (2025)
 */

#include "autosyndesis.h"
#include "autosyndesis_actv_stdby.h"

int autosyndesis_determine_path_active_standby(autosyndesis_state_t* state)
{
    autosyndesis_actv_stdby_props_t* props = (autosyndesis_actv_stdby_props_t*) state->mode_properties;

    if (props->down_threshold == 0) props->down_threshold = LINK_DOWN_THRESHOLD;

    int next_path = props->default_link;

    // check if the latest rtt is above the threshold and switch to the next path
    while (state->path_stats[next_path].rtt_us[state->path_stats[next_path].index] > props->down_threshold) {
        DEBUG_PRINT("Info (autosyndesis_determine_path_active_standby): Default link %d is down\n", next_path);
        next_path = (next_path + 1) % state->num_paths;
    
        if (next_path == props->default_link) {
            DEBUG_PRINT("Warning (autosyndesis_determine_path_active_standby): All paths are down\n");
            next_path = 0;
            break;
        }
    }

    return next_path;
}
