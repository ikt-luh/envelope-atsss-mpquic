/*
 * AuToSyndeSiS - ATSSS-like path selection C library
 * Mode: Smallest Delay / MinRTT
 * 
 * Author: David Munstein (2025)
 */

#include "autosyndesis.h"
#include "autosyndesis_smallest_delay.h"

int autosyndesis_determine_path_smallest_delay(autosyndesis_state_t* state)
{
    autosyndesis_minrtt_props_t *props = (autosyndesis_minrtt_props_t *)state->mode_properties;
    int current_path = props->last_path;
    int best_path = current_path;
    uint32_t current_rtt = state->path_stats[current_path].rtt_us[state->path_stats[current_path].index];
    uint32_t smallest_rtt = current_rtt;


    for (int i = 0; i < state->num_paths; i++) {
        uint32_t rtt = state->path_stats[i].rtt_us[state->path_stats[i].index];
        if (rtt < smallest_rtt) {
            smallest_rtt = rtt;
            best_path = i;
        }
    }


    if (best_path != current_path &&
        smallest_rtt + props->switch_margin_us < current_rtt) {
        DEBUG_PRINT("Switching from path %d (%u us) to path %d (%u us)\n",
                     current_path, current_rtt, best_path, smallest_rtt);
        props->last_path = best_path;
    }

    if (smallest_rtt > LINK_DOWN_THRESHOLD) {
        DEBUG_PRINT("Warning (autosyndesis_determine_path_smallest_delay): all paths are down\n");
        props->last_path = 0;
    }

    return props->last_path;
}
/*
int autosyndesis_determine_path_smallest_delay(autosyndesis_state_t* state)
{
    autosyndesis_minrtt_props_t *props = (autosyndesis_minrtt_props_t *)state->mode_properties;
    int next_path = props->last_path;
    uint64_t margin;
    uint64_t smallest_rtt = UINT64_MAX;

    // find the path with the smallest RTT
    for (int i = 0; i < state->num_paths; i++) {
        if (state->path_stats[i].rtt_us[state->path_stats[i].index] < smallest_rtt) {
            smallest_rtt = state->path_stats[i].rtt_us[state->path_stats[i].index];
            next_path = i;
        }
    }

    // don't bother switching if the current path rtt is below the switch margin
    if (state->path_stats[props->last_path].rtt_us[state->path_stats[props->last_path].index] < props->switch_margin_us) {
        return props->last_path;
    }

    // only switch if the new path is significantly better (by switch_margin)
    if (props->last_path != next_path && smallest_rtt < state->path_stats[props->last_path].rtt_us[state->path_stats[props->last_path].index] - props->switch_margin_us) {
	uint32_t difference = state->path_stats[props->last_path].rtt_us[state->path_stats[props->last_path].index] - props->switch_margin_us;
	printf("Diff: %u\n", difference);
        printf("Info (autosyndesis_determine_path_smallest_delay): Switching from path %d to path %d\n", props->last_path, next_path);
        props->last_path = next_path;
    } else {
        next_path = props->last_path;
    }

    if (smallest_rtt > LINK_DOWN_THRESHOLD) {
        DEBUG_PRINT("Warning (autosyndesis_determine_path_smallest_delay): All paths are down\n");
        next_path = 0;
    }

    return next_path;
}
*/
