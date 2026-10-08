/*
 * Femto-QUIC, a simple wrapper library for PicoQUIC
 * Based on the PicoQUIC samples by Christian Huitema
 * 
 * Author: David Munstein (2025)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <autoqlog.h>
#include "femto.h"
#include "femto_mp.h"

#define FEMTO_EXAMPLE_SNI "test.example.com"
#define FEMTO_EXAMPLE_ALPN "femto_example"

/* === CLIENT EXAMPLE === */

void *client_callback(uint8_t *data, size_t len, void *callback_ctx)
{
    // lifetime of data is only valid during this function call
    printf("Received a response from the server (len=%lu): <%s>\n", len, data);
}

int client_example(char *servers)
{
    int ret;
    femto_client_t client = { 0 };

    ret = create_femto_client(&client, servers, FEMTO_EXAMPLE_ALPN, FEMTO_EXAMPLE_SNI, &client_callback, NULL);
    if (ret != 0) {
        fprintf(stderr, "Could not create Femto-QUIC client. ReturnCode=%d\n", ret);
        return ret;
    }

    // Store SSL secrets in sslkeylogfile.txt - only for debugging!
    enable_femto_client_sslkeylogfile(&client, "sslkeylogfile.txt");

    // Hack to wait for the server to probe all paths
    usleep(100000);

    printf("Press Enter to send data to the server...\n");
    while (1) {
        getchar();
        send_data(&client, "Hello World!", 13, 0);
        printf("Data sent to server (path=0): <Hello World!>\n");
        getchar();
        send_data(&client, "Hello World!", 13, 1);
        printf("Data sent to server (path=1): <Hello World!>\n");
    }

    // send_data(&client, "Hello World!", 13, 0);
    // printf("Data sent to server: <Hello World!>\n");

    // usleep(50000);

    // send_data(&client, "Hello World!", 13, 1);
    // printf("Data sent to server: <Hello World!>\n");

    // printf("Press Enter to exit...\n");
    // getchar();

    destroy_femto_client(&client);

    return 0;
}


/* === SERVER EXAMPLE === */

void *server_callback(uint8_t *receive_buffer, size_t receive_len, void *user_ctx, uint8_t *response_buffer, size_t *response_len, picoquic_cnx_t *cnx)
{
    picoquic_cnx_t **last_cnx = (picoquic_cnx_t**)user_ctx;

    printf("Received data from client (len=%lu): <%s>. Responding...\n", receive_len, receive_buffer);

    strcpy(response_buffer, "Test Response! Received: ");
    strncat(response_buffer, receive_buffer, receive_len);
    *response_len = strlen(response_buffer);

    *last_cnx = cnx;
}

int server_example(int port, const char* server_cert, const char* server_key, int mp_enabled)
{
    int ret;
    femto_server_t server;
    picoquic_cnx_t *last_cnx = NULL;  // connection to send data to. NULL until connection is opened by the client

    ret = create_femto_server(&server, port, mp_enabled, FEMTO_EXAMPLE_ALPN, server_cert, server_key, &server_callback, &last_cnx);
    if (ret != 0) {
        fprintf(stderr, "Could not create Femto-QUIC server. ReturnCode=%d\n", ret);
        return ret;
    }

    // send data to client every second (without receiving data first)
    while (1) {
        usleep(1000000);
        if (last_cnx == NULL) continue;  // TODO detect when connection is closed
        //printf("Sending <Hello from server!> to client...\n");

        //send_data_to_client(&server, 0, last_cnx, "Hello from server!", 19);
    }

    destroy_femto_server(&server);

    return 0;
}


/* === MULTIPATH CLIENT EXAMPLE === */

void *mp_client_callback(uint8_t *data, size_t len, void *callback_ctx)
{
    femto_mp_user_ctx_t *user_ctx = (femto_mp_user_ctx_t*)callback_ctx;
    // lifetime of data is only valid during this function call
    printf("Received a response from the server (len=%lu, path=%d): <%s>\n", len, user_ctx->path_id, data);
}

int mp_client_example(char *servers)
{
    int ret;
    femto_mp_client_t mp_client = { 0 };
    char data_string[256];

    ret = create_femto_mp_client(&mp_client, servers, FEMTO_EXAMPLE_ALPN, FEMTO_EXAMPLE_SNI, &mp_client_callback, NULL);
    if (ret != 0) {
        fprintf(stderr, "Could not create Femto-QUIC multipath client. ReturnCode=%d\n", ret);
        return ret;
    }

    picoquic_set_key_log_file_from_env(mp_client.clients[0].quic);
    picoquic_set_qlog(mp_client.clients[0].quic, ".");
    picoquic_set_log_level(mp_client.clients[0].quic, 1);

    send_data_mp(&mp_client, "Hello World on path 0", 22, 0);
    printf("Data sent to server (path=0): <Hello World on path 0>\n");
    send_data_mp(&mp_client, "Hello World on path 1", 22, 1);
    printf("Data sent to server (path=1): <Hello World on path 1>\n");

    printf("Press Enter to exit...\n");
    getchar();

    destroy_femto_mp_client(&mp_client);

    return 0;
}


/* === MULTIPATH SERVER EXAMPLE === */

void *mp_server_callback(uint8_t *receive_buffer, size_t receive_len, void *cb_ctx, uint8_t *response_buffer, size_t *response_len, picoquic_cnx_t *cnx)
{
    femto_mp_user_ctx_t *mp_user_ctx = (femto_mp_user_ctx_t*)cb_ctx;

    printf("Received data from client (len=%lu, path=%d): <%s>. Responding...\n", receive_len, mp_user_ctx->path_id, receive_buffer);

    strcpy(response_buffer, "Test Response! Received: ");
    strncat(response_buffer, receive_buffer, receive_len);
    *response_len = strlen(response_buffer);
}

int mp_server_example(char *port_list, const char* server_cert, const char* server_key)
{
    int ret;
    int num_paths;
    int ports[MAX_SERVERS];
    femto_mp_server_t mp_server;

    num_paths = parse_port_list(port_list, ports, MAX_SERVERS);
    if (num_paths < 1) {
        fprintf(stderr, "Invalid port list\n");
        return -1;
    }

    ret = create_femto_mp_server(&mp_server, num_paths, ports, FEMTO_EXAMPLE_ALPN, server_cert, server_key, &mp_server_callback, NULL);
    if (ret != 0) {
        fprintf(stderr, "Could not create Femto-QUIC multipath server. ReturnCode=%d\n", ret);
        return ret;
    }

    //start_femto_mp_server_loop(&mp_server);

    printf("Press Enter to exit...\n");
    getchar();

    destroy_femto_mp_server(&mp_server);
}


/* === MAIN === */

int main(int argc, char **argv)
{
    int ret = 0;

    if (argc < 2) {
        ret = 1;
    }
    /* === Client === */
    else if (strcmp(argv[1], "client") == 0) {
        if (argc < 3) {
            ret = 1;
        }
        else {
            printf("Starting Femto-QUIC client with servers <%s>\n", argv[2]);
            ret = client_example(argv[2]);
        }
    }
    /* === Server === */
    else if (strcmp(argv[1], "server") == 0) {
        if (argc < 6) {
            ret = 1;
        }
        else {
            if (access(argv[3], F_OK) == -1 || access(argv[4], F_OK) == -1) {
                fprintf(stderr, "Certificate or key file does not exist\n");
                return 1;
            }

            printf("Starting Femto-QUIC server on port %d\n", atoi(argv[2]));
            ret = server_example(atoi(argv[2]), argv[3], argv[4], atoi(argv[5]));
        }
    }
    /* === Multipath Client === */
    else if (strcmp(argv[1], "mpclient") == 0) {
        if (argc < 3) {
            ret = 1;
        }
        else {
            printf("Starting Femto-QUIC multipath client with servers <%s>\n", argv[2]);
            ret = mp_client_example(argv[2]);
        }
    }
    /* === Multipath Server === */
    else if (strcmp(argv[1], "mpserver") == 0) {
        if (argc < 5) {
            ret = 1;
        }
        else {
            if (access(argv[3], F_OK) == -1 || access(argv[4], F_OK) == -1) {
                fprintf(stderr, "Certificate or key file does not exist\n");
                return 1;
            }

            printf("Starting Femto-QUIC multipath server\n");
            ret = mp_server_example(argv[2], argv[3], argv[4]);
        }
    }
    /* === Usage === */
    if (ret != 0) {
        printf(" === Femto-QUIC Example Usage === \n");
        printf(" Client Usage: %s client <list of servers>\n", argv[0]);
        printf(" - list of servers: e.g., 10.0.1.2:4443,10.0.2.2:0\n");
        printf(" - specify server port with first ip (e.g., 4443), multipath interface with other ips (e.g., 0)\n");
        printf(" Server Usage: %s server <port> <cert_file> <key_file> <mp_enabled 1/0>\n", argv[0]);
        printf(" Multipath Client Usage: %s mpclient <list of servers>\n", argv[0]);
        printf(" - list of servers: e.g., 10.0.1.2:4443,10.0.2.2:4444\n");
        printf(" Multipath Server Usage: %s mpserver <list of ports> <cert_file> <key_file>\n", argv[0]);
        printf(" - list of ports: e.g., 4443,4444\n");
    }
}
