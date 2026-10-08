/*
 * Smart-Multipath-Project
 * 
 * Author: David Munstein (2025)
 */

#ifndef TRIGGER_UPF_MODE_H
#define TRIGGER_UPF_MODE_H

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <malloc.h>
#include <signal.h>


#define TRIGGER_UPF_MODE_FIFO "/tmp/trigger_upf_mode_fifo"


typedef struct {
    uint8_t mode;
    uint8_t as_default_link;
    uint32_t minrtt_margin_us;
    uint8_t lb_rr_num_paths;
    uint8_t lb_rr_shares[8];
} fifo_data_t;


// Create the FIFO if it does not exist
void trigger_fifo_create_if_missing() {
    struct stat st;
    if (stat(TRIGGER_UPF_MODE_FIFO, &st) == -1) {
        // FIFO does not exist, create it
        if (mkfifo(TRIGGER_UPF_MODE_FIFO, 0666) == -1) {
            printf("Error: Could not create FIFO at %s\n", TRIGGER_UPF_MODE_FIFO);
            exit(EXIT_FAILURE);
        }
        printf("FIFO created at %s\n", TRIGGER_UPF_MODE_FIFO);
    } else if (!S_ISFIFO(st.st_mode)) {
        // Path exists but is not a FIFO
        printf("Error: %s exists but is not a FIFO\n", TRIGGER_UPF_MODE_FIFO);
        exit(EXIT_FAILURE);
    }
}


// Remove the FIFO
void trigger_fifo_cleanup() {
    if (unlink(TRIGGER_UPF_MODE_FIFO) == -1) {
        printf("Error: Could not remove FIFO at %s\n", TRIGGER_UPF_MODE_FIFO);
    } else {
        printf("FIFO at %s removed\n", TRIGGER_UPF_MODE_FIFO);
    }
}


// Write data to the FIFO (AuToSyndeSiS mode trigger)
int trigger_fifo_send_data(uint8_t mode, uint8_t as_default_link, uint32_t minrtt_margin_us, 
                           uint8_t lb_rr_num_paths, uint8_t *lb_rr_shares) {
    // Open the FIFO for writing
    int fd = open(TRIGGER_UPF_MODE_FIFO, O_WRONLY);
    if (fd == -1) {
        printf("Error: Could not open FIFO at %s\n", TRIGGER_UPF_MODE_FIFO);
        return -1;
    }

    // Calculate the total size of the data to be sent
    size_t total_size = sizeof(fifo_data_t);

    // Allocate memory for the data structure
    fifo_data_t *data = (fifo_data_t *)malloc(total_size);
    if (!data) {
        printf("Error: Could not allocate memory for FIFO data\n");
        close(fd);
        return -2;
    }

    // Populate the data structure
    data->mode = mode;
    data->as_default_link = as_default_link;
    data->minrtt_margin_us = minrtt_margin_us;
    data->lb_rr_num_paths = lb_rr_num_paths;
    memcpy(data->lb_rr_shares, lb_rr_shares, lb_rr_num_paths * sizeof(uint8_t));

    // Write the data to the FIFO
    ssize_t bytes_written = write(fd, data, total_size);
    if (bytes_written == -1) {
        printf("Error: Could not write to FIFO\n");
        free(data);
        close(fd);
        return -3;
    }

    // Clean up
    free(data);
    close(fd);

    return 0; // Success
}


// Read data from the FIFO (AuToSyndeSiS mode trigger)
int trigger_fifo_receive_data(uint8_t *mode, uint8_t *as_default_link, uint32_t *minrtt_margin_us, 
                              uint8_t *lb_rr_num_paths, uint8_t **lb_rr_shares) {
    // Open the FIFO for reading
    int fd = open(TRIGGER_UPF_MODE_FIFO, O_RDONLY);
    if (fd == -1) {
        printf("Error: Could not open FIFO at %s\n", TRIGGER_UPF_MODE_FIFO);
        return -1;
    }

    // Read the fixed-size part of the structure
    fifo_data_t header;
    ssize_t bytes_read = read(fd, &header, sizeof(fifo_data_t));
    if (bytes_read == -1) {
        printf("Error: Could not read FIFO header\n");
        close(fd);
        return -2;
    }

    // Allocate memory for the lb_rr_shares array based on lb_rr_num_paths
    uint8_t *shares = (uint8_t *)malloc(header.lb_rr_num_paths * sizeof(uint8_t));
    if (!shares) {
        printf("Error: Could not allocate memory for FIFO shares\n");
        close(fd);
        return -3;
    }
    memcpy(shares, header.lb_rr_shares, header.lb_rr_num_paths * sizeof(uint8_t));

    printf("header.lb_rr_num_paths: %d\n Shares: {", header.lb_rr_num_paths);
    for (int i = 0; i < header.lb_rr_num_paths; i++) {
        printf(" %d", shares[i]);
    } printf(" }\n");

    // Populate the output parameters
    *mode = header.mode;
    *as_default_link = header.as_default_link;
    *minrtt_margin_us = header.minrtt_margin_us;
    *lb_rr_num_paths = header.lb_rr_num_paths;
    *lb_rr_shares = shares;

    // Clean up
    close(fd);

    return 0; // Success
}



#endif
