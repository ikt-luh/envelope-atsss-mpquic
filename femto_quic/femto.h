/*
 * Femto-QUIC, a simple wrapper library for PicoQUIC
 * Based on the PicoQUIC samples by Christian Huitema
 * 
 * Author: David Munstein (2025)
 */

#ifndef FEMTO_H
#define FEMTO_H

#include <stdio.h>
#include "femto_types.h"


#define FEMTO_CLIENT_TICKET_STORE "sample_ticket_store.bin";
#define FEMTO_CLIENT_TOKEN_STORE "sample_token_store.bin";

#define MAX_SERVERS 10
#define MAX_SERVER_NAME_LENGTH 100

#ifdef ENABLE_DEBUG
    #define DEBUG_PRINT(...) printf(__VA_ARGS__)
#else
    #define DEBUG_PRINT(...)
#endif

/* femto_utils.c */

int parse_server_list(const char *server_string, char ip_addresses[][MAX_SERVER_NAME_LENGTH], int *ports, int max_servers);

int parse_port_list(const char *port_string, int *ports, int max_ports);

/* femto_client.c */

int create_femto_client(femto_client_t *client, char *servers, char const* alpn, char const* sni,
                        void *callback_fn(uint8_t *, size_t, void *), void *user_ctx);

int create_femto_client_bind_if(femto_client_t *client, char *servers, char const* alpn, char const* sni,
                        void *callback_fn(uint8_t *, size_t, void *), void *user_ctx, char* if_name);

void destroy_femto_client(femto_client_t *client);

int send_data(femto_client_t *client, uint8_t *data, size_t len, int path_id);

int send_datagram(femto_client_t *client, uint8_t *data, size_t len, int path_id);

uint64_t get_path_rtt(femto_client_t *client, int path_id);

void register_femto_client_status_callback(femto_client_t *client, void *callback_fn(uint8_t status_type, void *status_data, void *user_ctx));

void enable_femto_client_sslkeylogfile(femto_client_t *client, char *keylogfile);


/* Dynamic path management - femto_client.c */

int femto_add_path_dynamic(femto_client_t *client, int if_index, 
    const char *server_ip, int server_port);

int femto_remove_path_dynamic(femto_client_t *client, int if_index);

int femto_get_active_path_count(femto_client_t *client);

int femto_list_paths(femto_client_t *client, int *if_indices, int max_paths);

void femto_update_dynamic_path_id(femto_client_ctx_t *client_ctx, uint64_t unique_path_id);

void femto_mark_dynamic_path_deleted(femto_client_ctx_t *client_ctx, uint64_t unique_path_id);

void femto_mark_dynamic_path_suspended(femto_client_ctx_t *client_ctx, uint64_t unique_path_id);


/* femto_server.c */

int create_femto_server(femto_server_t *server, int port, int mp_enabled, const char* alpn,
                        const char* server_cert, const char* server_key,
                        void *callback_fn(uint8_t *, size_t, void *, uint8_t *, size_t *, picoquic_cnx_t *), void *user_ctx);

// int start_femto_server_loop(femto_server_t *server);

void destroy_femto_server(femto_server_t *server);

int send_data_to_client(femto_server_t *server, int path_id, picoquic_cnx_t *cnx, uint8_t *data, size_t len);

void register_femto_server_status_callback(femto_server_t *server, void *callback_fn(uint8_t status_type, void *status_data, void *user_ctx));

void enable_femto_server_sslkeylogfile(femto_server_t *server, char *keylogfile);


#endif
