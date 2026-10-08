/*
 * AuToSyndeSiS - ATSSS-like path selection C library
 * Mode: Load-Balancing (static, round robin)
 * 
 * Author: David Munstein (2025)
 */

#include "autosyndesis.h"
#include "autosyndesis_lb_rr.h"

#include <stdlib.h>
#include <time.h>
#include <string.h>


/**
 * Create a shuffled array based on the share array (randomized order of paths)
 * Example 1: share = {4, 2} -> result = {0, 0, 1, 0, 0, 1}
 * Example 2: share = {3, 2, 7} -> result = {2, 0, 1, 2, 0, 2, 1, 2, 2, 0, 2, 2}
 * @param[in] share the share array
 * @param[in] share_size the size of the share array
 * @param[out] result the array to fill with the shuffled values
 */
void create_shuffled_share_array(uint8_t* share, int share_size, uint8_t* result) {
    // Calculate the total length l
    int len = 0;
    for (int i = 0; i < share_size; i++) {
        len += share[i];
    }
    memset(result, 0, len);
    // Fill the array based on the share
    int index = 0;
    for (int i = 0; i < share_size; i++) {
        for (int j = 0; j < share[i]; j++) {
            result[index++] = i;
        }
    }

    // Shuffle the array to randomize positions
    srand(time(NULL));
    for (int i = len - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int temp = result[i];
        result[i] = result[j];
        result[j] = temp;
    }
}


/**
 * Determine the path to use for the next packet based on the current AuToSyndeSiS state
 * Mode: Load-Balancing (static, round robin)
 * 
 * Example: num_paths = 2 , share = { 5, 3 }
 * 5 packets will be sent on path 0, 3 packets will be sent on path 1 in a random order
 */
int autosyndesis_determine_path_loadbalance_rr(autosyndesis_state_t* state)
{
    autosyndesis_lb_rr_props_t *props = (autosyndesis_lb_rr_props_t *)state->mode_properties;
    int next_path = 0;
    int temp_counter = 0;
    
    // calculate the share sum once and store it
    if (!props->initialized || props->change_share) {
        props->share_sum = 0;
        for (int i = 0; i < state->num_paths; i++) {
            props->share_sum += props->share[i];
        }
        if (props->share_sum > MAX_PATHS * MAX_PATHS) {
            DEBUG_PRINT("ERROR: Share sum must not exceed MAX_PATHS^2\n");
            return 0;
        }
        create_shuffled_share_array(props->share, state->num_paths, props->packet_order);

        props->rr_sent_counter = 0;
        props->initialized = 1;
        props->change_share = 0;
    }

    next_path = props->packet_order[props->rr_sent_counter];
    //printf("PACKET INDEX:%d\n", props->rr_sent_counter);
    props->rr_sent_counter++;
    if (props->rr_sent_counter >= props->share_sum) {
        props->rr_sent_counter = 0;
        //printf("ARRAY WRAPPED AROUND\n");
    }

    return next_path;
}
