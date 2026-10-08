/*
 * Femto-QUIC, a simple wrapper library for PicoQUIC
 * Based on the PicoQUIC samples by Christian Huitema
 * 
 * Author: David Munstein (2025)
 */

#include "femto_mp.h"


/**
 * Create a new Femto-QUIC multipath client
 * @param[out] mp_client the multipath client structure to be created
 * @param[in] num_paths the number of paths to create
 * @param[in] ports the list of ports to connect to (one per path)
 * @param[in] alpn the ALPN to use for the connection
 * @param[in] callback_fn the callback function to be called when data is received
 * @return 0 if successful
 */
int create_femto_mp_server(femto_mp_server_t *mp_server, int num_paths, int *ports, const char* alpn,
                        const char* server_cert, const char* server_key,
                        void *callback_fn(uint8_t *, size_t, void *, uint8_t *, size_t *, picoquic_cnx_t *), void *user_ctx)
{
    int ret = 0;
    femto_mp_user_ctx_t *user_ctx_ptr;

    mp_server->num_paths = num_paths;
    memcpy(mp_server->ports, ports, sizeof(int) * num_paths);
    mp_server->servers = (femto_server_t *)malloc(sizeof(femto_server_t) * num_paths);
    for (int i = 0; i < num_paths; i++) {
        user_ctx_ptr = (femto_mp_user_ctx_t *)malloc(sizeof(femto_mp_user_ctx_t));
        user_ctx_ptr->path_id = i;
        user_ctx_ptr->user_ctx = user_ctx;
        ret = create_femto_server(&mp_server->servers[i], ports[i], 0, alpn, server_cert, server_key, callback_fn, user_ctx_ptr);
        if (ret != 0) break;
    }

    return ret;
}

// /**
//  * Start the Femto-QUIC multipath server loop for receiving data in a background thread
//  * Callback function is called from the background thread when data is received
//  * @param[in] mp_server the multipath server structure
//  * @return 0 if successful
//  */
// int start_femto_mp_server_loop(femto_mp_server_t *mp_server)
// {
//     for (int i = 0; i < mp_server->num_paths; i++) {
//         start_femto_server_loop(&mp_server->servers[i]);
//     }

//     return 0;
// }

/**
 * Destroy a Femto-QUIC multipath server
 * @param[in] mp_server the multipath server structure to be destroyed
 */
void destroy_femto_mp_server(femto_mp_server_t *mp_server)
{
    for (int i = 0; i < mp_server->num_paths; i++) {
        free(mp_server->servers[i].server_ctx->user_ctx);
        destroy_femto_server(&mp_server->servers[i]);
    }
    free(mp_server->servers);
}


/**
 * Send data to a client on a specific path
 * Ensure that the cnx exist on the path before calling this function
 * @param[in] mp_server the multipath server structure
 * @param[in] path_id the path to send the data on
 * @param[in] cnx the connection to send the data on
 * @param[in] data the data to send
 * @param[in] len the length of the data
 */
int send_data_to_client_mp(femto_mp_server_t *mp_server, int path_id, picoquic_cnx_t *cnx, uint8_t *data, size_t len)
{
    return send_data_to_client(&mp_server->servers[path_id], 0, cnx, data, len);
}
