/*
 * Femto-QUIC, a simple wrapper library for PicoQUIC
 * Based on the PicoQUIC samples by Christian Huitema
 * 
 * Author: David Munstein (2025)
 */

#ifndef FEMTO_MP_H
#define FEMTO_MP_H

#include "femto.h"


/* === FEMTO MULTIPATH TYPES === */

typedef struct st_femto_mp_client {
    int num_paths;
    int server_ports[MAX_SERVERS];
    char server_names[MAX_SERVERS][MAX_SERVER_NAME_LENGTH];
    int server_ifs[MAX_SERVERS];
    femto_client_t *clients;
} femto_mp_client_t;

typedef struct st_femto_mp_server {
    int num_paths;
    int ports[MAX_SERVERS];
    femto_server_t *servers;
} femto_mp_server_t;

typedef struct st_femto_mp_user_ctx_t {
    int path_id;
    void *user_ctx;
} femto_mp_user_ctx_t;

/* femto_mp_client.c */

int create_femto_mp_client(femto_mp_client_t *mp_client, char *servers, char const* alpn, char const* sni,
                        void *callback_fn(uint8_t *, size_t, void *), void *user_ctx);

int create_femto_mp_client_bind_if(femto_mp_client_t *mp_client, char *servers, char const* alpn, char const* sni,
                        void *callback_fn(uint8_t *, size_t, void *), void *user_ctx, char* if_names);

void destroy_femto_mp_client(femto_mp_client_t *mp_client);

int send_data_mp(femto_mp_client_t *mp_client, uint8_t *data, size_t len, int path_id);

uint64_t get_mp_path_rtt(femto_mp_client_t *client, int path_id);

/* femto_mp_server.c */

int create_femto_mp_server(femto_mp_server_t *mp_server, int num_paths, int *ports, const char* alpn,
                        const char* server_cert, const char* server_key,
                        void *callback_fn(uint8_t *, size_t, void *, uint8_t *, size_t *, picoquic_cnx_t *), void *user_ctx);

// int start_femto_mp_server_loop(femto_mp_server_t *mp_server);

void destroy_femto_mp_server(femto_mp_server_t *mp_server);

int send_data_to_client_mp(femto_mp_server_t *mp_server, int path_id, picoquic_cnx_t *cnx, uint8_t *data, size_t len);

#endif
