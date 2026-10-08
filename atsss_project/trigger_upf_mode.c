/*
 * Smart-Multipath-Project
 * Trigger script for the User Plane Function (UPF) mode to select the AuToSyndeSiS mode
 * Uses AuToSyndeSis, Femto-QUIC and PicoQUIC
 *
 * Usage: ./trigger_upf_mode <new mode> [<args>]
 * 
 * Author: David Munstein (2025)
 */

#include <stdio.h>
#include <stdint.h>
#include "trigger_upf_mode.h"

void print_usage()
{
    printf("Usage: ./trigger_upf_mode <new mode> [<args>]\n");
    printf("Modes:\n");
    printf("  0: Active-Standby\n");
    printf("     Parameters: <default link>\n");
    printf("     Example: ./trigger_upf_mode 0 0\n");
    printf("  1: Smallest Delay / MinRTT\n");
    printf("     Parameters: <switch margin in microseconds>\n");
    printf("     Example: ./trigger_upf_mode 1 2000\n");
    printf("  2: Load Balancing Round Robin\n");
    printf("     Parameters: <number of paths> <shares for each path, comma separated no space>\n");
    printf("     Example: ./trigger_upf_mode 2 2 1,1\n");
    printf("  3: Multipath Duplication / Opportunistic Redundant\n");
    printf("     Parameters: <switch margin in microseconds for retransmissions MinRTT>\n");
    printf("     Example: ./trigger_upf_mode 3 2000\n");
}


int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    uint8_t mode = atoi(argv[1]);

    uint8_t as_default_link = 0;
    uint32_t minrtt_margin_us = 0;
    uint8_t lb_rr_num_paths = 0;
    uint8_t *lb_rr_shares = NULL;

    switch(mode) {
    case 0:  // Active-Standby
        if (argc != 3) {
            printf("Invalid number of arguments for mode 0\n");
            print_usage();
            return 1;
        }
        as_default_link = atoi(argv[2]);
        printf("Mode %i: Default link = %d\n", mode, as_default_link);
        break;
    case 1:  // Smallest Delay / MinRTT
    case 3:  // Multipath Duplication / Opportunistic Redundant
        if (argc != 3) {
            printf("Invalid number of arguments for mode 1 or 3\n");
            print_usage();
            return 1;
        }
        minrtt_margin_us = atoi(argv[2]);
        printf("Mode %i: Switch margin = %d\n", mode, minrtt_margin_us);
        break;
    case 2:  // Load Balancing Round Robin
        if (argc != 4) {
            printf("Invalid number of arguments for mode 2\n");
            print_usage();
            return 1;
        }
        lb_rr_num_paths = atoi(argv[2]);
        lb_rr_shares = (uint8_t *)malloc(lb_rr_num_paths * sizeof(uint8_t));
        char *share_str = argv[3];
        char *token = strtok(share_str, ",");
        for (int i = 0; i < lb_rr_num_paths; i++) {
            lb_rr_shares[i] = atoi(token);
            token = strtok(NULL, ",");
        }
        printf("Mode %i: Number of paths = %d, Shares = { ", mode, lb_rr_num_paths);
        for (int i = 0; i < lb_rr_num_paths; i++) {
            printf("%d ", lb_rr_shares[i]);
        }
        printf("}\n");
        break;
    default:
        printf("Invalid mode: %d\n", mode);
        print_usage();
        return 1;
    }

    trigger_fifo_create_if_missing();

    trigger_fifo_send_data(mode, as_default_link, minrtt_margin_us, lb_rr_num_paths, lb_rr_shares);

    free(lb_rr_shares);

    return 0;
}