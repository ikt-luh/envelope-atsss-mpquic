/*
 * AuToSyndeSiS - ATSSS-like path selection C library
 *
 * Author: David Munstein (2025)
 */

#include <string.h>
#include "autosyndesis.h"

// Initialize the AuToSyndeSiS state structure with the number of paths. Default mode is active-standby.
int autosyndesis_init(autosyndesis_state_t* state, int num_paths)
{
    if (num_paths > MAX_PATHS || num_paths < 1) {
        DEBUG_PRINT("Error (autosyndesis_init): Invalid number of paths: %d. Supported is 1 <= paths <= %d\n", num_paths, MAX_PATHS);
        return -1;
    }
    state->num_paths = num_paths;

    state->current_mode = MODE_ACTIVE_STANDBY;
    state->last_path_id = 0;
    state->packet_counter = 0;

    for (int i = 0; i < num_paths; i++) {
        state->path_stats[i].index = 0;
        memset(state->path_stats[i].rtt_us, 0, sizeof(state->path_stats[i].rtt_us));
    }

    return 0;
}

// Change the ATSSS mode for the AuToSyndeSiS state
// mode_properties is a pointer to a structure that contains the properties for the mode
// - MODE_ACTIVE_STANDBY: autosyndesis_actv_stdby_props_t
// - MODE_SMALLEST_DELAY: TODO
// - MODE_LOAD_BALANCING: TODO
// - MODE_MP_DUPLICATION: TODO
int autosyndesis_set_mode(autosyndesis_state_t* state, autosyndesis_atsss_mode mode, void *mode_properties)
{
    if (mode < MODE_ACTIVE_STANDBY || mode > MODE_MP_DUPLICATION) {
        DEBUG_PRINT("Error (autosyndesis_set_mode): Invalid mode: %d\n", mode);
        return -1;
    }

    state->current_mode = mode;
    state->mode_properties = mode_properties;
    DEBUG_PRINT("Info (autosyndesis_set_mode): Mode set to %d\n", mode);

    return 0;
}

// Determine the path to use for the next packet based on the current AuToSyndeSiS state
int autosyndesis_determine_path(autosyndesis_state_t* state)
{
    int next_path = 0;

    switch (state->current_mode)
    {
    case MODE_ACTIVE_STANDBY:
        next_path = autosyndesis_determine_path_active_standby(state);
        break;
    case MODE_SMALLEST_DELAY:
    case MODE_MP_DUPLICATION:  // opportunistic redundant (use AuToSyndeSiS only for retranmissions)
        next_path = autosyndesis_determine_path_smallest_delay(state);
        break;
    case MODE_LOAD_BALANCING:
        next_path = autosyndesis_determine_path_loadbalance_rr(state);
        break;
    default:
        break;
    }

    return next_path;
}

// Update the link properties for a given path
int autosyndesis_update_link_properties(autosyndesis_state_t* state, int path_id, uint32_t delay)
{
    if (path_id >= state->num_paths || path_id < 0) {
        DEBUG_PRINT("Error (autosyndesis_update_link_properties): Invalid path ID: %d\n", path_id);
        return -1;
    }

    state->path_stats[path_id].index = (state->path_stats[path_id].index + 1) % LINK_STAT_BUFFER_SIZE;
    state->path_stats[path_id].rtt_us[state->path_stats[path_id].index] = delay;
    return 0;
}
