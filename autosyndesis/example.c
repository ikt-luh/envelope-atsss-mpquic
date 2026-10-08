/*
 * AuToSyndeSiS demo of path selection using Femto-QUIC (PicoQUIC)
 * Example application
 * 
 * Author: David Munstein (2025)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "femto.h"
#include "femto_mp.h"
#include "autosyndesis.h"
#include "autosyndesis_actv_stdby.h"
#include "autosyndesis_lb_rr.h"
#include "autosyndesis_smallest_delay.h"


// atsss_femto_example
#define ATSSS_QUIC_ALPN "femto_example"
#define ATSSS_QUIC_SNI "test.example.com"


void *mp_client_callback(uint8_t *data, size_t len, void *callback_ctx)
{
    int *path_id_ptr = (int*)callback_ctx;

    printf("Received a response from the server (len=%lu, path=%d): <%s>\n", len, *path_id_ptr, data);
}

int main(int argc, char **argv)
{
    int ret;
    autosyndesis_state_t atsss_state;
    autosyndesis_actv_stdby_props_t atsss_actv_stdby_props = { 0 };
    autosyndesis_lb_rr_props_t atsss_lb_rr_props = { 0 };
    autosyndesis_minrtt_props_t atsss_minrtt_props = { 0 };
    femto_mp_client_t mp_client;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <server_list>\n", argv[0]);
        return 1;
    }

    // Initialize AuToSyndeSiS
    ret = autosyndesis_init(&atsss_state, 2);
    if (ret != 0) {
        fprintf(stderr, "Could not initialize AuToSyndeSiS. ReturnCode=%d\n", ret);
        return ret;
    }

    // Create the Femto-QUIC multipath client
    ret = create_femto_mp_client(&mp_client, argv[1], ATSSS_QUIC_ALPN, ATSSS_QUIC_SNI, &mp_client_callback, NULL);
    if (ret != 0) {
        fprintf(stderr, "Could not create Femto-QUIC multipath client. ReturnCode=%d\n", ret);
        return ret;
    }

    // Set AuToSyndeSiS mode to Active-Standby
    atsss_actv_stdby_props.default_link = 0;
    atsss_lb_rr_props.share[0] = 5; atsss_lb_rr_props.share[1] = 3; atsss_lb_rr_props.initial_path = 0;
    atsss_minrtt_props.switch_margin_us = 2000;
    //autosyndesis_set_mode(&atsss_state, MODE_ACTIVE_STANDBY, &atsss_actv_stdby_props);
    //autosyndesis_set_mode(&atsss_state, MODE_SMALLEST_DELAY, &atsss_minrtt_props);
    //autosyndesis_set_mode(&atsss_state, MODE_MP_DUPLICATION, NULL);
    autosyndesis_set_mode(&atsss_state, MODE_LOAD_BALANCING, &atsss_lb_rr_props);
    if (ret != 0) {
        fprintf(stderr, "Could not set AuToSyndeSiS mode. ReturnCode=%d\n", ret);
        return ret;
    }

    int counter = 1;
    int path_id;
    char send_buffer[100];
    if (atsss_state.current_mode == MODE_MP_DUPLICATION) {
        while (1) {
            sprintf(send_buffer, "Hello World (%d)!", counter++);
            for (int i = 0; i < atsss_state.num_paths; i++) {
                send_data_mp(&mp_client, send_buffer, strlen(send_buffer) + 1, i);
                printf("Sent message over path %d: <%s>\n", i, send_buffer);
            }
            printf("Sent message over both paths: <%s>\n", send_buffer);
            usleep(100000);
        }
    } else {
        while (1) {
            sprintf(send_buffer, "Hello World (%d)!", counter++);
            path_id = autosyndesis_determine_path(&atsss_state);
            send_data_mp(&mp_client, send_buffer, strlen(send_buffer) + 1, path_id);
            printf("Sent message over path %d: <%s>\n", path_id, send_buffer);

            // send a message over both paths every 5th packet to update the RTT for all paths
            if (counter % 5 == 0) {
                send_data_mp(&mp_client, "ping0", 6, 0);
                send_data_mp(&mp_client, "ping1", 6, 1);
            }

            usleep(100000);

            for (int i = 0; i < 2; i++) {
                uint64_t rtt = get_mp_path_rtt(&mp_client, i);
                if (rtt > 0) {
                    autosyndesis_update_link_properties(&atsss_state, i, rtt);
                    printf("Link %d: RTT=%luus\n", i, rtt);
                }
            }

        }
    }

    destroy_femto_mp_client(&mp_client);

    return 0;
}
