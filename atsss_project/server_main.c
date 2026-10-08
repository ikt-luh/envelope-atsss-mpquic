/*
 * Smart-Multipath-Project: Pipe-Stream for Video Transmission
 * Application Server
 * Uses AuToSyndeSis, Femto-QUIC and PicoQUIC
 * 
 * Author: David Munstein (2025)
 */

#include "femto.h"
#include "atsss_project.h"
#include <stdint.h>
#include <fcntl.h>
#include <signal.h>

#define SERVER_LOG_FILE_NAME "mpeval_server.log"


typedef struct {
    int measurement_interval;
    uint64_t last_measurement_time;
    uint64_t bytes_sum;
    uint64_t tcs_delay_sum_us;
    int inputfile;
    int outputfile;
} perf_server_ctx_t;

static void write_all(int fd, const void *buf, size_t count) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = count;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
        }
        left -= (size_t)w;
        p += w;
    }
}

void *server_callback(uint8_t *receive_buffer, size_t receive_len, void *user_ctx, uint8_t *response_buffer, size_t *response_len, picoquic_cnx_t *cnx)
{
    perf_server_ctx_t *perf_server_ctx = (perf_server_ctx_t *)user_ctx;

    /*if (receive_len != sizeof(eval_perf_pkg_t)) {
        printf("Received an invalid performance client data packet from the UE (receive_len=%lu)\n", receive_len);
        *response_len = 0;
        return NULL;
    }*/

    eval_perf_pkg_t *pkg_received = (eval_perf_pkg_t *)receive_buffer;

    pkg_received->timestamp_server = picoquic_current_time();

/*
    memcpy(response_buffer, receive_buffer, receive_len);
    *response_len = receive_len;
*/


    DEBUG_PRINT("[DEBUG: Performance Client] Received packet of size %lu (seq_num=%d). Responding... (TCS=%lu us)\n", receive_len, pkg_received->seq_num, (pkg_received->timestamp_server - pkg_received->timestamp_client));

    char filename[40];
    memcpy(filename, pkg_received->payload, 39);
    filename[39] = '\0';

    uint16_t payload_len = (pkg_received->payload[43] << 8) | pkg_received->payload[44];


    DEBUG_PRINT("Payload [%s] with size %d\n", filename, payload_len);
    if (pkg_received->payload[40] != ATSSS_WEBRTC_DATA_EMPTY){
        write_all(perf_server_ctx->outputfile, pkg_received->payload + 45, payload_len);
        DEBUG_PRINT("Got past write\n");
    }
/*
    response_buffer[0] = ATSSS_WEBRTC_DATA;   // Packet type
    response_buffer[1] = payload_len >> 8;    // High byte
    response_buffer[2] = payload_len & 0xFF;  // Low byte

    // Copy the actual payload back
    memcpy(response_buffer + 3, pkg_received->payload + 45, payload_len);

    // Total response length = header + payload
    *response_len = payload_len + 3;
*/
    ssize_t fifo_read_size = -11;//read(perf_server_ctx->inputfile, response_buffer + 3, 1500);
    DEBUG_PRINT("Got past read, fifo_read_size= %zd\n", fifo_read_size);
    if(fifo_read_size > 0){
        response_buffer[0] = ATSSS_WEBRTC_DATA;
        response_buffer[1] = fifo_read_size >> 8;
        response_buffer[2] = fifo_read_size;
        *response_len = fifo_read_size +3;
    } else if(fifo_read_size < 0 && errno == EAGAIN){
        memset(response_buffer, 0, sizeof(eval_perf_pkg_t));
        response_buffer[0] = ATSSS_WEBRTC_DATA_EMPTY;
        response_buffer[1] = 0;
        response_buffer[2] = 0;
        *response_len = sizeof(eval_perf_pkg_t);
    }
}


int main(int argc, char **argv)
{
    int ret;
    int running = 1;
    const int multipath_enabled = 0;
    femto_server_t server;
    perf_server_ctx_t perf_server_ctx = { 0 };
    perf_server_ctx.measurement_interval = 200;
    perf_server_ctx.outputfile = open(argv[4], O_WRONLY);
    //perf_server_ctx.inputfile = open(argv[5], O_RDONLY);
    //fcntl(perf_server_ctx.inputfile, F_SETFL, O_NONBLOCK);
    if (perf_server_ctx.outputfile == -1) {
        DEBUG_PRINT("Error: Could not open FIFO at %s\n", "/tmp/fifo_output");
        return -1;
    }

    if (argc != 6) {
        fprintf(stderr, "ATSSS Application Server Usage: %s <port> <cert_file> <key_file> <fifo_output> <fifo_input>\n", argv[0]);
        return 1;
    }
    DEBUG_PRINT("ATSSS Application Server: Starting on port <%s>...\n", argv[1]);


    ret = create_femto_server(&server, atoi(argv[1]), multipath_enabled, ATSSS_UPF_SERVER_ALPN, argv[2], argv[3], &server_callback, &perf_server_ctx);
    if (ret != 0) {
        fprintf(stderr, "Could not create Femto-QUIC server. ReturnCode=%d\n", ret);
        return ret;
    }

    while (running) {
        usleep(100000);
        // TODO
    }

    destroy_femto_server(&server);


    return 0;
}
