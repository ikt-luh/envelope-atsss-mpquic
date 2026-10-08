/*
 * Femto-QUIC, a simple wrapper library for PicoQUIC
 * Based on the PicoQUIC samples by Christian Huitema
 * 
 * Author: David Munstein (2025)
 */

#include "femto_mp.h"
#include <net/if.h>


/**
 * Create a new Femto-QUIC multipath client
 * @param[out] mp_client the multipath client structure to be created
 * @param[in] servers the list of servers to connect to, e.g. "10.0.1.2:4443,10.0.2.2:4444"
 * @param[in] alpn the ALPN to use for the connection
 * @param[in] sni the SNI to use for the connection
 * @param[in] callback_fn the callback function to be called when data is received
 * @param[in] user_ctx the user context to be passed to the callback function
 * @return 0 if successful
 */
int create_femto_mp_client(femto_mp_client_t *mp_client, char *servers, char const* alpn, char const* sni,
                        void *callback_fn(uint8_t *, size_t, void *), void *user_ctx)
{
    int server_count;
    char server_name[MAX_SERVER_NAME_LENGTH * 2];
    femto_mp_user_ctx_t *user_ctx_ptr;

    // determine list of servers
    server_count = parse_server_list(servers, mp_client->server_names, mp_client->server_ports, MAX_SERVERS);
    if (server_count <= 0) {
        printf("Could not parse the servers list\n");
        return 2;
    }
    mp_client->num_paths = server_count;

    mp_client->clients = (femto_client_t *)malloc(sizeof(femto_client_t) * server_count);
    for (int i = 0; i < server_count; i++) {
        sprintf(server_name, "%s:%d/%d", mp_client->server_names[i], mp_client->server_ports[i], mp_client->server_ifs[i]);
        user_ctx_ptr = (femto_mp_user_ctx_t *)malloc(sizeof(femto_mp_user_ctx_t));
        user_ctx_ptr->path_id = i;
        user_ctx_ptr->user_ctx = user_ctx;
        create_femto_client(&mp_client->clients[i], server_name, alpn, sni, callback_fn, user_ctx_ptr);
    }

    return 0;
}


/**
 * Create a new Femto-QUIC multipath client and bind interfaces
 * @param[out] mp_client the multipath client structure to be created
 * @param[in] servers the list of servers to connect to, e.g. "10.0.1.2:4443,10.0.2.2:4444"
 * @param[in] alpn the ALPN to use for the connection
 * @param[in] sni the SNI to use for the connection
 * @param[in] callback_fn the callback function to be called when data is received
 * @param[in] user_ctx the user context to be passed to the callback function
 * @param[in] if_names the names of the interfaces to bind to, e.g. "eth0,eth1"
 * @return 0 if successful
 */
int create_femto_mp_client_bind_if(femto_mp_client_t *mp_client, char *servers, char const* alpn, char const* sni,
                        void *callback_fn(uint8_t *, size_t, void *), void *user_ctx, char* if_names)
{
    int server_count;
    char server_name[MAX_SERVER_NAME_LENGTH * 2];
    femto_mp_user_ctx_t *user_ctx_ptr;

    // determine list of servers
    server_count = parse_server_list(servers, mp_client->server_names, mp_client->server_ports, MAX_SERVERS);
    if (server_count <= 0) {
        printf("Could not parse the servers list\n");
        return 2;
    }
    mp_client->num_paths = server_count;

    char if_names_copy[strlen(if_names) + 1];
    strcpy(if_names_copy, if_names);
    char* if_name_token = strtok(if_names_copy, ",");

    mp_client->clients = (femto_client_t *)malloc(sizeof(femto_client_t) * server_count);
    for (int i = 0; i < server_count; i++) {
        sprintf(server_name, "%s:%d", mp_client->server_names[i], mp_client->server_ports[i]);
        user_ctx_ptr = (femto_mp_user_ctx_t *)malloc(sizeof(femto_mp_user_ctx_t));
        user_ctx_ptr->path_id = i;
        user_ctx_ptr->user_ctx = user_ctx;
        mp_client->server_ifs[i] = if_nametoindex(if_name_token);
        create_femto_client_bind_if(&mp_client->clients[i], server_name, alpn, sni, callback_fn, user_ctx_ptr, if_name_token);
        if_name_token = strtok(NULL, ",");
    }

    return 0;
}

/**
 * Destroy a Femto-QUIC multipath client
 * @param[in] mp_client the multipath client structure to be destroyed
 */
void destroy_femto_mp_client(femto_mp_client_t *mp_client)
{
    for (int i = 0; i < mp_client->num_paths; i++) {
        free(mp_client->clients[i].client_ctx->user_ctx);
        destroy_femto_client(&mp_client->clients[i]);
    }
    free(mp_client->clients);
}

/**
 * Send data to the server using a specific path
 * @param[in] mp_client the multipath client structure
 * @param[in] data the data to be sent
 * @param[in] len the length of the data
 * @param[in] path_id the path ID
 * @return 0 if successful, -1 if the path ID is invalid, 1 otherwise
 */
int send_data_mp(femto_mp_client_t *mp_client, uint8_t *data, size_t len, int path_id)
{
    if (path_id < 0 || path_id >= mp_client->num_paths) {
        return -1;
    }

    return send_data(&mp_client->clients[path_id], data, len, 0);
}

/**
 * Get the round-trip time for a specific path
 * @param[in] mp_client the multipath client structure
 * @param[in] path_id the path ID (Femto-QUIC MP path ID, not PicoQUIC internal one)
 * @return the round-trip time in microseconds or 0 if the path ID is invalid
 */
uint64_t get_mp_path_rtt(femto_mp_client_t *mp_client, int path_id)
{
    if (path_id < 0 || path_id >= mp_client->num_paths) {
        return 0;
    }
    return get_path_rtt(&mp_client->clients[path_id], 0);  // use PicoQUIC internal path ID 0
}
