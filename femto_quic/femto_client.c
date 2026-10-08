/*
 * Femto-QUIC, a simple wrapper library for PicoQUIC
 * Based on the PicoQUIC samples by Christian Huitema
 * 
 * Author: David Munstein (2025)
 */

#include <picoquic_internal.h>
#include <stdint.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <net/if.h>
#include <ifaddrs.h>
#include "femto.h"
#include "femto_interface_monitor.h"


/* === FEMTO CLIENT INTERNAL FUNCTIONS === */

// create a new stream for sending a QueueElement's data to the server
static int femto_client_create_stream(picoquic_cnx_t* cnx,
    femto_client_ctx_t* client_ctx, QueueElement *element)
{
    int ret = 0;
    uint64_t unique_path_id;
    femto_client_stream_ctx_t* stream_ctx = (femto_client_stream_ctx_t*)
        malloc(sizeof(femto_client_stream_ctx_t));

    if (stream_ctx == NULL) {
        fprintf(stdout, "Memory Error, cannot create stream");
        ret = -1;
    }
    else {
        memset(stream_ctx, 0, sizeof(femto_client_stream_ctx_t));
        if (client_ctx->first_stream == NULL) {
            client_ctx->first_stream = stream_ctx;
            client_ctx->last_stream = stream_ctx;
        }
        else {
            client_ctx->last_stream->next_stream = stream_ctx;
            client_ctx->last_stream = stream_ctx;
        }

        stream_ctx->stream_id = picoquic_get_next_local_stream_id(client_ctx->cnx, 0);

        // Future TODO: Implement zero-copy
        stream_ctx->send_data_buffer = (uint8_t*)malloc(element->data_len);
        memcpy(stream_ctx->send_data_buffer, element->data_buffer, element->data_len);
        stream_ctx->send_data_len = element->data_len;
        stream_ctx->sent_bytes = 0;

        ret = picoquic_mark_active_stream(cnx, stream_ctx->stream_id, 1, stream_ctx);
        if (ret != 0) {
            fprintf(stdout, "Error %d, cannot initialize stream\n", ret);
        }
        client_ctx->active_streams++;
        //printf("DEBUG: Activated stream %lu. Total active streams: %d\n", stream_ctx->stream_id, client_ctx->active_streams);

        // set the stream path affinity
        if (cnx->is_multipath_enabled && element->path_id > -1
                && element->path_id <= client_ctx->mp_nb_alt_paths
                && cnx->path[element->path_id] != NULL) {
            unique_path_id = cnx->path[element->path_id]->unique_path_id;

            ret = picoquic_set_stream_path_affinity(cnx, stream_ctx->stream_id, unique_path_id);
            if (ret != 0) {
                printf("Error %d, cannot set stream path affinity to path=%d (unique_id=%lu) for stream %lu\n", ret, element->path_id, unique_path_id, stream_ctx->stream_id);
            }
        }
    }

    return ret;
}

femto_client_stream_ctx_t * femto_client_create_stream_context(femto_client_ctx_t* server_ctx, uint64_t stream_id)
{
    femto_client_stream_ctx_t* stream_ctx = (femto_client_stream_ctx_t*)malloc(sizeof(femto_client_stream_ctx_t));

    if (stream_ctx != NULL) {
        memset(stream_ctx, 0, sizeof(femto_client_stream_ctx_t));

        if (server_ctx->last_stream == NULL) {
            server_ctx->last_stream = stream_ctx;
            server_ctx->first_stream = stream_ctx;
        }
        else {
            //stream_ctx->previous_stream = server_ctx->last_stream;
            server_ctx->last_stream->next_stream = stream_ctx;
            server_ctx->last_stream = stream_ctx;
        }
        stream_ctx->stream_id = stream_id;
    }

    return stream_ctx;
}

// delete a stream context, free the memory allocations
static void femto_client_free_context(femto_client_ctx_t* client_ctx)
{
    femto_client_stream_ctx_t* stream_ctx;

    while ((stream_ctx = client_ctx->first_stream) != NULL) {
        client_ctx->first_stream = stream_ctx->next_stream;
        if (stream_ctx->receive_data_buffer != NULL) {
            free(stream_ctx->receive_data_buffer);  // TODO: This still caused a segfault
        }
        free(stream_ctx);
    }
    client_ctx->last_stream = NULL;
}

int femto_client_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
    int ret = 0;
    femto_client_ctx_t* client_ctx = (femto_client_ctx_t*)callback_ctx;
    femto_client_stream_ctx_t* stream_ctx = (femto_client_stream_ctx_t*)v_stream_ctx;

    // initialize multipath (once): probe paths
    if (!client_ctx->mp_init_complete && picoquic_get_cnx_state(cnx) == picoquic_state_ready) {
        for (int i = 0; i < client_ctx->mp_nb_alt_paths; i++) {
            if ((ret = picoquic_probe_new_path_ex(cnx, (struct sockaddr*)client_ctx->server_address,
            (struct sockaddr*)&client_ctx->mp_alt_ip[i], client_ctx->mp_src_if[i], picoquic_get_quic_time(cnx->quic), 0)) != 0) {
                printf("Probe new path %d failed with exit code %d\n", i, ret);
            }
        }
        client_ctx->mp_init_complete = 1;
        
        // Mark that dynamic path management is ready
        // Note: Interface monitoring can be started by the application using
        // femto_init_interface_monitor() and femto_start_interface_monitor()
        printf("[Multipath] Initial paths probed. Dynamic path management ready.\n");
    }

#ifdef FEMTO_DEBUG
    printf("Available paths:\n");
    for (int i = 0; i < cnx->nb_paths; i++) {
        if (cnx->path[i] == NULL) {
            printf("- Path %i is NULL\n", i);
            continue;
        }
        printf("- Unique Path ID for path %i is %lu\n", i, cnx->path[i]->unique_path_id);
    }
    printf("\n");
#endif

    if (client_ctx == NULL) {
        /* This should never happen, because the callback context for the client is initialized 
         * when creating the client connection. */
        printf("client callback: client context is null\n");
        return -1;
    }

    if (ret == 0) {
        switch (fin_or_event) {
        case picoquic_callback_stream_data:
        case picoquic_callback_stream_fin:
            /* Data arrival on stream #x, maybe with fin mark */
            if (stream_ctx == NULL) {
                /* Create and initialize stream context */
                stream_ctx = femto_client_create_stream_context(client_ctx, stream_id);
                if (picoquic_set_app_stream_ctx(cnx, stream_id, stream_ctx) != 0) {
                    /* Internal error */
                    (void) picoquic_reset_stream(cnx, stream_id, 0);
                    return(-1);
                }
            }

            if (stream_ctx == NULL) {
                /* This is unexpected, as all contexts were declared when initializing the
                 * connection. */
                printf("client callback: picoquic_callback_stream_fin: stream context is null\n");
                return -1;
            }
            // else if (!stream_ctx->is_data_sent) {
            //     /* Unexpected: should not receive data before sending the file name to the server */
            //     printf("client callback: picoquic_callback_stream_fin: data received before sending data\n");
            //     //return -1;
            // }
            else if (stream_ctx->is_stream_reset || stream_ctx->is_stream_finished) {
                /* Unexpected: receive after fin */
                printf("client callback: picoquic_callback_stream_fin: stream reset %d %d\n", stream_ctx->is_stream_reset, stream_ctx->is_stream_finished);
                return -1;
            }
            else
            {
                // create a buffer for receiving data if not done yet
                if (stream_ctx->receive_data_buffer == NULL) {
                    stream_ctx->receive_buffer_size = BUFFER_SIZE;  // TODO: Make this a variable/define
                    stream_ctx->receive_data_buffer = (uint8_t*)malloc(stream_ctx->receive_buffer_size);
                    stream_ctx->received_bytes = 0;

                    if (stream_ctx->receive_data_buffer == NULL) {
                        printf("Could not allocate memory for receive buffer\n");
                        ret = -1;
                    }
                }

                // copy received data into the receive buffer
                if (ret == 0 && length > 0) {
                    if (stream_ctx->received_bytes + length > stream_ctx->receive_buffer_size) {
                        printf("client callback: receive buffer full error: ");
                        printf("received_bytes=%lu , length=%lu , receive_buffer_size=%lu\n",
                                stream_ctx->received_bytes, length, stream_ctx->receive_buffer_size);
                        ret = -1;
                    }

                    if (ret == 0) {
                        memcpy(stream_ctx->receive_data_buffer + stream_ctx->bytes_received, bytes, length);
                        stream_ctx->bytes_received += length;
                    }
                }

                // when receiving data is completed, call the client callback
                if (ret == 0 && fin_or_event == picoquic_callback_stream_fin) {
                    stream_ctx->is_stream_finished = 1;

                    client_ctx->client_callback_fn(stream_ctx->receive_data_buffer, stream_ctx->bytes_received, client_ctx->user_ctx);

                    free(stream_ctx->receive_data_buffer);
                    stream_ctx->receive_data_buffer = NULL;
                    //free(stream_ctx);
                }
            }
            break;
        case picoquic_callback_stop_sending: /* Should not happen, treated as reset */
            /* Mark stream as abandoned, close the file, etc. */
            // picoquic_reset_stream(cnx, stream_id, 0);
            /* Fall through */
        case picoquic_callback_stream_reset: /* Server reset stream #x */
            if (stream_ctx == NULL) {
                /* This is unexpected, as all contexts were declared when initializing the
                 * connection. */
                printf("client callback: picoquic_callback_stream_reset: stream context is null\n");
                return -1;
            }
            else if (stream_ctx->is_stream_reset || stream_ctx->is_stream_finished) {
                /* Unexpected: receive after fin */
                printf("client callback: picoquic_callback_stream_reset: stream reset %d %d\n", stream_ctx->is_stream_reset, stream_ctx->is_stream_finished);
                return -1;
            }
            else {
                stream_ctx->remote_error = picoquic_get_remote_stream_error(cnx, stream_id);
                stream_ctx->is_stream_reset = 1;
            }
            break;
        case picoquic_callback_stateless_reset:
        case picoquic_callback_close: /* Received connection close */
        case picoquic_callback_application_close: /* Received application close */
            client_ctx->is_disconnected = 1;
            /* Remove the application callback */
            picoquic_set_callback(cnx, NULL, NULL);
            break;
        case picoquic_callback_version_negotiation:
            /* The client did not get the right version.
             * TODO: some form of negotiation?
             */
            printf("Received a version negotiation request:");
            for (size_t byte_index = 0; byte_index + 4 <= length; byte_index += 4) {
                uint32_t vn = 0;
                for (int i = 0; i < 4; i++) {
                    vn <<= 8;
                    vn += bytes[byte_index + i];
                }
                printf("%s%08x", (byte_index == 0) ? " " : ", ", vn);
            }
            fprintf(stdout, "\n");
            break;
        case picoquic_callback_stream_gap:
            /* This callback is never used. */
            break;
        case picoquic_callback_prepare_to_send:
            /* Active sending API */
            if (stream_ctx == NULL) {
                /* Decidedly unexpected */
                printf("client callback: picoquic_callback_prepare_to_send: stream context is null\n");
                return -1;
            }
            // send data while available (until all send buffer content is sent)
            else if (stream_ctx->sent_bytes < stream_ctx->send_data_len){
                uint8_t* buffer;
                size_t available = stream_ctx->send_data_len - stream_ctx->sent_bytes;
                int is_fin = 1;

                /* The length parameter marks the space available in the packet */
                if (available > length) {
                    available = length;
                    is_fin = 0;
                    //printf("client callback: last name chunk sent\n");
                }
                /* Needs to retrieve a pointer to the actual buffer 
                 * the "bytes" parameter points to the sending context 
                 */
                buffer = picoquic_provide_stream_data_buffer(bytes, available, is_fin, !is_fin);
                if (buffer != NULL) {
                    memcpy(buffer, stream_ctx->send_data_buffer + stream_ctx->sent_bytes, available);
                    stream_ctx->sent_bytes += available;
                    stream_ctx->is_data_sent = is_fin;
                    if (is_fin) {
                        client_ctx->active_streams--;
                        //printf("DEBUG: Stream %lu finished. Total active streams: %d\n", stream_ctx->stream_id, client_ctx->active_streams);
                        free(stream_ctx->send_data_buffer);
                        stream_ctx->send_data_buffer = NULL;
                        //free(stream_ctx);
                    }
                }
                else {
                    printf("\nError, could not get data buffer.\n");
                    ret = -1;
                }
            }
            else {
                /* Nothing to send, just return */
            }
            break;
        case picoquic_callback_almost_ready:
            break;
        case picoquic_callback_path_available:
            /* a new path is available or a suspended path is available again */
            printf("[Path Callback] Path available: unique_path_id=%lu\n", stream_id);
            femto_update_dynamic_path_id(client_ctx, stream_id);
            if (client_ctx->status_callback_fn != NULL) {
                client_ctx->status_callback_fn(60 + fin_or_event, &stream_id, client_ctx->user_ctx);
            }
            break;
        case picoquic_callback_path_suspended:
            /* an available path is suspended */
            printf("[Path Callback] Path suspended: unique_path_id=%lu\n", stream_id);
            femto_mark_dynamic_path_suspended(client_ctx, stream_id);
            if (client_ctx->status_callback_fn != NULL) {
                client_ctx->status_callback_fn(60 + fin_or_event, &stream_id, client_ctx->user_ctx);
            }
            break;
        case picoquic_callback_path_deleted:
            /* an existing path has been deleted */
            printf("[Path Callback] Path deleted: unique_path_id=%lu\n", stream_id);
            femto_mark_dynamic_path_deleted(client_ctx, stream_id);
            if (client_ctx->status_callback_fn != NULL) {
                client_ctx->status_callback_fn(60 + fin_or_event, &stream_id, client_ctx->user_ctx);
            }
            break;
        case picoquic_callback_path_quality_changed:
            /* Path parameters like RTT, data rate or packet loss rate have changed */
            if (client_ctx->status_callback_fn != NULL) {
                client_ctx->status_callback_fn(60 + fin_or_event, &stream_id, client_ctx->user_ctx);
            }
            break;
        default:
            /* unexpected -- just ignore. */
            break;
        }
    }

    return ret;
}

static int femto_client_configure(picoquic_quic_config_t *config, picoquic_packet_loop_param_t *param,
    int enable_multipath, char *servers)
{
    int ret = 0;

    if (enable_multipath) {
        param->local_port = (uint16_t)picoquic_uniform_random(30000) + 20000;
        param->extra_socket_required = 1;

        config->multipath_option = 1;
        config->multipath_alt_config = malloc(sizeof(char) * (strlen(servers) + 1));
        strcpy(config->multipath_alt_config, servers);
    }

    return ret;
}

static int femto_client_init(char const* server_name, int server_port, char const* alpn, char const* sni,
    char const* ticket_store_filename, char const* token_store_filename,
    struct sockaddr_storage * server_address, picoquic_quic_t** quic, picoquic_cnx_t** cnx, femto_client_ctx_t *client_ctx,
    picoquic_quic_config_t* config)
{
    int ret = 0;
    uint64_t current_time = picoquic_current_time();

    *quic = NULL;
    *cnx = NULL;

    /* Get the server's address */
    if (ret == 0) {
        int is_name = 0;

        ret = picoquic_get_server_address(server_name, server_port, server_address, &is_name);
        if (ret != 0) {
            printf("Cannot get the IP address for <%s> port <%d>", server_name, server_port);
        }
        else if (is_name) {
            sni = server_name;
        }
    }

    /* Create a QUIC context. It could be used for many connections, but in this sample we
     * will use it for just one connection.
     * The sample code exercises just a small subset of the QUIC context configuration options:
     * - use files to store tickets and tokens in order to manage retry and 0-RTT
     * - set the congestion control algorithm to BBR
     */
    if (ret == 0) {
        config->alpn = alpn;
        config->ticket_file_name = ticket_store_filename;

        *quic = picoquic_create_and_configure(config, NULL, NULL, current_time, NULL);

        picoquic_enable_path_callbacks_default(*quic, 1);

        if (*quic == NULL) {
            printf("Could not create quic context\n");
            ret = -1;
        }
        else {
            if (picoquic_load_retry_tokens(*quic, token_store_filename) != 0) {
                printf("No token file present. Will create one as <%s>.\n", token_store_filename);
            }

            picoquic_set_default_congestion_algorithm(*quic, picoquic_bbr_algorithm);
        }
    }

    /* Initialize the callback context and create the connection context.
     * We use minimal options on the client side, keeping the transport
     * parameter values set by default for picoquic. This could be fixed later.
     */
    if (ret == 0) {
        /* Create a client connection */
        *cnx = picoquic_create_cnx(*quic, picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)server_address, current_time, 0, sni, alpn, 1);

        if (*cnx == NULL) {
            printf("Could not create connection context\n");
            ret = -1;
        }
        else {
            /* Document connection in client's context */
            client_ctx->cnx = *cnx;
            /* Set the client callback context */
            picoquic_set_callback(*cnx, femto_client_callback, client_ctx);
            /* Client connection parameters could be set here, before starting the connection. */
            ret = picoquic_start_client_cnx(*cnx);
            if (ret < 0) {
                printf("Could not activate connection\n");
            }
            else {
                /* Printing out the initial CID, which is used to identify log files */
                picoquic_connection_id_t icid = picoquic_get_initial_cnxid(*cnx);
                printf("Initial connection ID: ");
                for (uint8_t i = 0; i < icid.id_len; i++) {
                    printf("%02x", icid.id[i]);
                }
                printf("\n");
            }
        }
    }

    return ret;
}

// called by femto_client_loop_cb, triggered by picoquic_wake_up_network_thread
// checks the queue for new elements and creates streams for them
static int femto_client_wakeup(femto_client_ctx_t* client_ctx)
{
    int ret = 0;
    uint64_t stream_id;
    QueueElement element;

    // must use while loop because performance is bad if only one element is moved per wakeup
    // TODO: Only have one queue for stream ctxs instead of one for elements and one for stream ctxs
    // TODO: Then only perform the stream creation/activation here instead of copying the data
    while (!isQueueEmpty(client_ctx->q)) {
    //if (!isQueueEmpty(client_ctx->q)) {
        dequeue(client_ctx->q, &element);

        // STREAM
        if (element.send_mode == 0) {
            ret = femto_client_create_stream(client_ctx->cnx, client_ctx, &element);
            if (ret < 0) {
                printf("\nCould not initiate stream\n");
                break;
            }
        }
        // DATAGRAM - TODO!
        else {
            ret = picoquic_queue_datagram_frame(client_ctx->cnx, element.data_len, element.data_buffer);
            if (ret != 0) {
                printf("\nCould not queue datagram frame\n");
            }
        }
    }

    if (client_ctx->is_closing_requested && !client_ctx->is_closing) {
        picoquic_close(client_ctx->cnx, 0);
    }

    return ret;
}

static int femto_client_loop_cb(picoquic_quic_t* quic, picoquic_packet_loop_cb_enum cb_mode, 
    void* callback_ctx, void * callback_arg)
{
    int ret = 0;
    femto_client_ctx_t* client_ctx = (femto_client_ctx_t*)callback_ctx;

    if (client_ctx == NULL) {
        ret = PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }
    else {
        switch (cb_mode) {
        case picoquic_packet_loop_ready:
            break;
        case picoquic_packet_loop_wake_up:
            ret = femto_client_wakeup(client_ctx);
            break;
        case picoquic_packet_loop_after_receive:
            break;
        case picoquic_packet_loop_after_send:
            if (client_ctx->is_disconnected) {
                ret = PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
            }
            break;
        case picoquic_packet_loop_port_update:
            break;
        default:
            ret = PICOQUIC_ERROR_UNEXPECTED_ERROR;
            break;
        }
    }

    return ret;
}


/* === FEMTO CLIENT PUBLIC FUNCTIONS === */

/**
 * Create a new Femto-QUIC client
 * @param[out] client the client structure to be initialized
 * @param[in] servers a comma-separated list of servers and ports, e.g. "10.0.1.2:4443,10.0.2.2:1"
 * (only the first port is considered with draft-ietf-quic-multipath, the later ports are "interface" numbers)
 * @param[in] alpn the application layer protocol name
 * @param[in] sni the server name indication
 * @param[in] callback_fn the callback function to be called when data is received, (uint8_t *data, size_t len, void *user_ctx)
 * @param[in] user_ctx pointer to a custom context to be passed to the callback function
 * @param[in] mp_enabled enable multipath
 * @return 0 if successful
 */
int create_femto_client(femto_client_t *client, char *servers, char const* alpn, char const* sni,
                        void *callback_fn(uint8_t *, size_t, void *), void *user_ctx)
{
    int ret = 0;
    int *thread_ret = malloc(sizeof(int));
    struct sockaddr_storage *server_address = (struct sockaddr_storage *)malloc(sizeof(struct sockaddr_storage));
    picoquic_packet_loop_param_t *param = malloc(sizeof(picoquic_packet_loop_param_t));
    memset(param, 0, sizeof(*param));

    char const* ticket_store_filename = FEMTO_CLIENT_TICKET_STORE;
    char const* token_store_filename = FEMTO_CLIENT_TOKEN_STORE;
    // For a future MP implementation using picoquic_probe_new_path_ex (TODO)
    char server_names[MAX_SERVERS][MAX_SERVER_NAME_LENGTH];
    int server_ports[MAX_SERVERS];
    int server_count = 0;

    client->client_ctx = (femto_client_ctx_t *)malloc(sizeof(femto_client_ctx_t));
    memset(client->client_ctx, 0, sizeof(femto_client_ctx_t));
    client->client_ctx->q = (Queue *)malloc(sizeof(Queue));
    initQueue(client->client_ctx->q);

    client->client_ctx->user_ctx = user_ctx;

    client->config = (picoquic_quic_config_t *)malloc(sizeof(picoquic_quic_config_t));
    picoquic_config_init(client->config);


    server_count = parse_server_list(servers, server_names, server_ports, MAX_SERVERS);  // TODO
    if (server_count <= 0) {
        printf("Could not parse the servers list\n");
        return 2;
    }
    if (server_count > 1) {
        client->multipath_enabled = 1;
    }

    client->client_ctx->client_callback_fn = callback_fn;

    femto_client_configure(client->config, param, client->multipath_enabled, servers);

    client->client_ctx->mp_nb_alt_paths = server_count - 1;
    client->client_ctx->mp_src_if = (int *)malloc(client->client_ctx->mp_nb_alt_paths * sizeof(int));
    client->client_ctx->mp_alt_ip = (struct sockaddr_storage *)malloc(client->client_ctx->mp_nb_alt_paths * sizeof(struct sockaddr_storage));
    struct sockaddr_storage ip;
    for (int i = 0; i < client->client_ctx->mp_nb_alt_paths; i++) {
        if (picoquic_store_text_addr(&ip, server_names[i + 1], 0) != 0) {
            printf("Invalid IP (picoquic_store_text_addr): %s\n", server_names[i + 1]);
            return 3;
        }
        memcpy(client->client_ctx->mp_alt_ip + i, &ip, sizeof(struct sockaddr_storage));
        /* In this legacy format, subsequent entries encode the source interface id in "port". */
        client->client_ctx->mp_src_if[i] = server_ports[i + 1];
    }

    ret = femto_client_init(server_names[0], server_ports[0], alpn, sni,
        ticket_store_filename, token_store_filename,
        server_address, &client->quic, &client->cnx, client->client_ctx,
        client->config);

    if (ret != 0) {
        printf("Could not initialize the client\n");
        return 1;
    }

    param->local_af = server_address->ss_family;
    client->client_ctx->server_address = server_address;

    // start the background thread for PicoQUIC networking
    client->thread_ctx = picoquic_start_custom_network_thread(client->quic, param,
        picoquic_internal_thread_create, picoquic_internal_thread_delete,
        picoquic_internal_thread_setname, "femto_client_thread",  // TODO: maybe include path id in thread name, what is it needed for?
        femto_client_loop_cb,
        client->client_ctx, thread_ret);

    // TODO: handle thread_ret

    return ret;
}


/**
 * Create a new Femto-QUIC client and bind to a specific interface (TODO)
 * @param[out] client the client structure to be initialized
 * @param[in] servers a comma-separated list of servers and ports, e.g. "10.0.1.2:4443,10.0.2.2:1"
 * (only the first port is considered with draft-ietf-quic-multipath, the later ports are "interface" numbers)
 * @param[in] alpn the application layer protocol name
 * @param[in] sni the server name indication
 * @param[in] callback_fn the callback function to be called when data is received, (uint8_t *data, size_t len, void *user_ctx)
 * @param[in] user_ctx pointer to a custom context to be passed to the callback function
 * @param[in] if_name the name of the interface to bind to (e.g. "eth0")
 * @return 0 if successful
 */
int create_femto_client_bind_if(femto_client_t *client, char *servers, char const* alpn, char const* sni,
    void *callback_fn(uint8_t *, size_t, void *), void *user_ctx, char* if_name)
{
    int ret = 0;
    int *thread_ret = malloc(sizeof(int));
    struct sockaddr_storage *server_address = (struct sockaddr_storage *)malloc(sizeof(struct sockaddr_storage));
    picoquic_packet_loop_param_t *param = malloc(sizeof(picoquic_packet_loop_param_t));
    memset(param, 0, sizeof(*param));

    char const* ticket_store_filename = FEMTO_CLIENT_TICKET_STORE;
    char const* token_store_filename = FEMTO_CLIENT_TOKEN_STORE;
    // For a future MP implementation using picoquic_probe_new_path_ex (TODO)
    char server_names[MAX_SERVERS][MAX_SERVER_NAME_LENGTH];
    int server_ports[MAX_SERVERS];
    int server_count = 0;

    client->client_ctx = (femto_client_ctx_t *)malloc(sizeof(femto_client_ctx_t));
    memset(client->client_ctx, 0, sizeof(femto_client_ctx_t));
    client->client_ctx->q = (Queue *)malloc(sizeof(Queue));
    initQueue(client->client_ctx->q);

    client->client_ctx->user_ctx = user_ctx;

    client->config = (picoquic_quic_config_t *)malloc(sizeof(picoquic_quic_config_t));
    picoquic_config_init(client->config);

    server_count = parse_server_list(servers, server_names, server_ports, MAX_SERVERS);  // TODO
    if (server_count <= 0) {
        printf("Could not parse the servers list\n");
        return 2;
    }
    
    /* only enable PicoQUIC multipath when we actually have multiple endpoints specified.
     * when used from femto_mp_client (multi-cnx), each femto_client gets a single server entry
     * and enabling multipath unnecessarily requests extra sockets and can break the secondary connection. */
    client->multipath_enabled = (server_count > 1) ? 1 : 0;

    client->client_ctx->client_callback_fn = callback_fn;

    femto_client_configure(client->config, param, client->multipath_enabled, servers);
    param->src_if = if_nametoindex(if_name);
    printf("bound to interface: %d\n", param->src_if);

    // Set initial alt paths (can be 0 if starting with single path)
    client->client_ctx->mp_nb_alt_paths = (server_count > 1) ? server_count - 1 : 0;
    client->client_ctx->mp_src_if = (int *)malloc(client->client_ctx->mp_nb_alt_paths * sizeof(int));
    client->client_ctx->mp_alt_ip = (struct sockaddr_storage *)malloc(client->client_ctx->mp_nb_alt_paths * sizeof(struct sockaddr_storage));
    struct sockaddr_storage ip;
    for (int i = 0; i < client->client_ctx->mp_nb_alt_paths; i++) {
        if (picoquic_store_text_addr(&ip, server_names[i + 1], 0) != 0) {
            printf("Invalid IP (picoquic_store_text_addr): %s\n", server_names[i + 1]);
            return 3;
        }
        memcpy(client->client_ctx->mp_alt_ip + i, &ip, sizeof(struct sockaddr_storage));
        client->client_ctx->mp_src_if[i] = if_name ? atoi(if_name) : 0;
    }

    // initialize dynamic path management
    client->client_ctx->mp_dynamic_paths = (dynamic_path_info_t *)malloc(MAX_DYNAMIC_PATHS * sizeof(dynamic_path_info_t));
    memset(client->client_ctx->mp_dynamic_paths, 0, MAX_DYNAMIC_PATHS * sizeof(dynamic_path_info_t));
    client->client_ctx->mp_dynamic_path_count = 0;
    client->client_ctx->mp_monitor_running = 0;
    pthread_mutex_init(&client->client_ctx->mp_path_mutex, NULL);
    
    // register initial paths in dynamic path tracking for unified management
    // path 0 (primary path)
    if (server_count >= 1) {
        int idx = client->client_ctx->mp_dynamic_path_count;
        client->client_ctx->mp_dynamic_paths[idx].if_index = if_name ? atoi(if_name) : 0;
        femto_get_interface_name(if_name ? atoi(if_name) : 0, client->client_ctx->mp_dynamic_paths[idx].if_name,
                                  sizeof(client->client_ctx->mp_dynamic_paths[idx].if_name));
        client->client_ctx->mp_dynamic_paths[idx].is_initial = 1;
        client->client_ctx->mp_dynamic_paths[idx].is_active = 0;  // will be set when connection ready
        client->client_ctx->mp_dynamic_paths[idx].picoquic_path_index = 0;  // primary path is always index 0
        client->client_ctx->mp_dynamic_path_count++;
        printf("[Path Tracking] Registered initial path 0 on interface %d (%s)\n", 
               if_name ? atoi(if_name) : 0, client->client_ctx->mp_dynamic_paths[idx].if_name);
    }
    
    // register additional initial paths (alt paths)
    for (int i = 0; i < client->client_ctx->mp_nb_alt_paths; i++) {
        int idx = client->client_ctx->mp_dynamic_path_count;
        client->client_ctx->mp_dynamic_paths[idx].if_index = client->client_ctx->mp_src_if[i];
        femto_get_interface_name(client->client_ctx->mp_src_if[i], client->client_ctx->mp_dynamic_paths[idx].if_name,
                                  sizeof(client->client_ctx->mp_dynamic_paths[idx].if_name));
        memcpy(&client->client_ctx->mp_dynamic_paths[idx].server_ip, 
               &client->client_ctx->mp_alt_ip[i], sizeof(struct sockaddr_storage));
        client->client_ctx->mp_dynamic_paths[idx].is_initial = 1;
        client->client_ctx->mp_dynamic_paths[idx].is_active = 0;  // will be set when path probed
        client->client_ctx->mp_dynamic_paths[idx].picoquic_path_index = i + 1;  // alt paths start at index 1
        client->client_ctx->mp_dynamic_path_count++;
        printf("[Path Tracking] Registered initial alt path %d on interface %d (%s)\n", 
               i + 1, client->client_ctx->mp_src_if[i], client->client_ctx->mp_dynamic_paths[idx].if_name);
    }

    ret = femto_client_init(server_names[0], server_ports[0], alpn, sni,
    ticket_store_filename, token_store_filename,
    server_address, &client->quic, &client->cnx, client->client_ctx,
    client->config);

    if (ret != 0) {
        printf("Could not initialize the client\n");
        return 1;
    }

    param->local_af = server_address->ss_family;
    client->client_ctx->server_address = server_address;

    // start the background thread for PicoQUIC networking
    client->thread_ctx = picoquic_start_custom_network_thread(client->quic, param,
    picoquic_internal_thread_create, picoquic_internal_thread_delete,
    picoquic_internal_thread_setname, "femto_client_thread",  // TODO: maybe include path id in thread name, what is it needed for?
    femto_client_loop_cb,
    client->client_ctx, thread_ret);

    // TODO: handle thread_ret

    return ret;
}


/**
 * Destroy a Femto-QUIC client
 * @param[in] client the client structure to be destroyed
 */
void destroy_femto_client(femto_client_t *client)
{
    if (client == NULL) {
        return;
    }
    femto_client_ctx_t *client_ctx = client->client_ctx;
    picoquic_network_thread_ctx_t *thread_ctx = client->thread_ctx;

    if (client_ctx == NULL) {
        return;
    }

    // Stop interface monitor if running
    if (client_ctx->mp_monitor_running) {
        client_ctx->mp_monitor_running = 0;
        // Note: The monitor thread will exit on its own when socket is closed
    }

    if (thread_ctx != NULL) {
        while (!thread_ctx->thread_is_closed) {
            if (client_ctx->is_disconnected) {
                break;
            }

            client_ctx->is_closing_requested = 1;
            picoquic_wake_up_network_thread(thread_ctx);

#ifdef _WINDOWS
                Sleep(10);
#else
                usleep(10000);
#endif
        }

        picoquic_delete_network_thread(thread_ctx);
        client->thread_ctx = NULL;
    }

    if (client->quic != NULL) {
        picoquic_free(client->quic);
        client->quic = NULL;
    }

    // clean up dynamic path management resources
    if (client_ctx->mp_dynamic_paths != NULL) {
        free(client_ctx->mp_dynamic_paths);
        client_ctx->mp_dynamic_paths = NULL;
    }
    pthread_mutex_destroy(&client_ctx->mp_path_mutex);

    free(client->client_ctx->server_address);
    femto_client_free_context(client_ctx);

    if (client->config != NULL) {
        if (client->config->multipath_alt_config != NULL) {
            free(client->config->multipath_alt_config);
            client->config->multipath_alt_config = NULL;
        }
        free(client->config);
        client->config = NULL;
    }
}

/**
 * Send data to the server in stream mode
 * @param[in] client the client structure
 * @param[in] data the data to be sent
 * @param[in] len the length of the data
 * @param[in] path_id the path ID, not used in the current implementation
 * @return 0 if successful, 1 otherwise
 */
int send_data(femto_client_t *client, uint8_t *data, size_t len, int path_id)
{
    femto_client_ctx_t *client_ctx = client->client_ctx;
    picoquic_network_thread_ctx_t *thread_ctx = client->thread_ctx;
 
    if (client_ctx == NULL || thread_ctx == NULL) {
        printf("send_data: ERROR, client_ctx or thread_ctx is NULL. This should not happen!\n");
    }

    QueueElement element;

    if (!client_ctx->is_closing_requested && !client_ctx->is_disconnected) {
        memcpy(element.data_buffer, data, len);
        element.data_len = len;
        element.path_id = path_id;
        element.send_mode = 0;  // stream mode

        enqueue(client_ctx->q, element);
        picoquic_wake_up_network_thread(thread_ctx);
    } else {
        return 1;
    }

    return 0;
}

/**
 * Send data to the server in datagram mode
 * @param[in] client the client structure
 * @param[in] data the data to be sent
 * @param[in] len the length of the data
 * @param[in] path_id the path ID, not used in the current implementation
 * @return 0 if successful, 1 otherwise
 */
int send_datagram(femto_client_t *client, uint8_t *data, size_t len, int path_id)
{
    femto_client_ctx_t *client_ctx = client->client_ctx;
    picoquic_network_thread_ctx_t *thread_ctx = client->thread_ctx;
 
    QueueElement element;

    if (!client_ctx->is_closing_requested && !client_ctx->is_disconnected) {
        memcpy(element.data_buffer, data, len);
        element.data_len = len;
        element.path_id = path_id;
        element.send_mode = 1;  // datagram mode

        enqueue(client_ctx->q, element);
        picoquic_wake_up_network_thread(thread_ctx);
    } else {
        return 1;
    }

    return 0;
}

/**
 * Get the RTT of a specific path
 * @param[in] client the client structure
 * @param[in] path_id the path ID (PicoQUIC internal path ID)
 * @return the smoothed RTT of the path in microseconds
 */
uint64_t get_path_rtt(femto_client_t *client, int path_id)
{
    picoquic_path_t *path = client->cnx->path[path_id];
    picoquic_path_quality_t quality;

    picoquic_get_path_quality(client->cnx, path_id, &quality);

    return quality.rtt;
}

/**
 * Register a callback function to receive status updates from the client.
 * This includes multipath path status updates.
 * The lifetime of the status_data is only valid during the callback function.
 * @param[in] client the client structure
 * @param[in] callback_fn the callback function to be called when a status update is received
 */
void register_femto_client_status_callback(femto_client_t *client, void *callback_fn(uint8_t status_type, void *status_data, void *user_ctx))
{
    client->client_ctx->status_callback_fn = callback_fn;
}

/**
 * Enable the Femto-QUIC key log file.
 * This is useful for debugging purposes, e.g. to decrypt QUIC traffic with Wireshark.
 * @param[in] client the client structure
 * @param[in] keylogfile the key log file path
 */
void enable_femto_client_sslkeylogfile(femto_client_t *client, char *keylogfile)
{
    picoquic_set_key_log_file(client->quic, keylogfile);
}


/* === DYNAMIC PATH MANAGEMENT FUNCTIONS === */

/**
 * get interface name from interface index (wrapper for femto_interface_monitor function)
 * @param[in] if_index the interface index
 * @param[out] if_name buffer to store interface name (at least 32 bytes)
 * @return 0 on success, -1 on failure
 */
static int femto_client_get_interface_name(int if_index, char *if_name)
{
    // use the public function from femto_interface_monitor
    return femto_get_interface_name(if_index, if_name, 32);
}

/**
 * Find a dynamic path by interface index
 * @param[in] client_ctx the client context
 * @param[in] if_index the interface index to find
 * @return the index in mp_dynamic_paths array, or -1 if not found
 */
static int femto_find_dynamic_path(femto_client_ctx_t *client_ctx, int if_index)
{
    // first try exact interface index match
    for (int i = 0; i < client_ctx->mp_dynamic_path_count; i++) {
        if (client_ctx->mp_dynamic_paths[i].if_index == if_index) {
            return i;
        }
    }
    
    // if not found, try matching by interface name (handles index changes on reconnect)
    char if_name[32];
    if (femto_client_get_interface_name(if_index, if_name) == 0) {
        for (int i = 0; i < client_ctx->mp_dynamic_path_count; i++) {
            if (strcmp(client_ctx->mp_dynamic_paths[i].if_name, if_name) == 0) {
                // update the cached interface index
                printf("femto_find_dynamic_path: interface %s index changed %d -> %d\n",
                       if_name, client_ctx->mp_dynamic_paths[i].if_index, if_index);
                client_ctx->mp_dynamic_paths[i].if_index = if_index;
                return i;
            }
        }
    }
    
    return -1;
}

/**
 * find a dynamic path by interface name
 * @param[in] client_ctx the client context
 * @param[in] if_name the interface name to find
 * @return the index in mp_dynamic_paths array, or -1 if not found
 */
static int femto_find_dynamic_path_by_name(femto_client_ctx_t *client_ctx, const char *if_name)
{
    for (int i = 0; i < client_ctx->mp_dynamic_path_count; i++) {
        if (strcmp(client_ctx->mp_dynamic_paths[i].if_name, if_name) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * find a dynamic path by PicoQUIC unique path ID
 * @param[in] client_ctx the client context
 * @param[in] unique_path_id the unique path ID to find
 * @return the index in mp_dynamic_paths array, or -1 if not found
 */
static int femto_find_dynamic_path_by_id(femto_client_ctx_t *client_ctx, uint64_t unique_path_id)
{
    for (int i = 0; i < client_ctx->mp_dynamic_path_count; i++) {
        if (client_ctx->mp_dynamic_paths[i].unique_path_id == unique_path_id) {
            return i;
        }
    }
    return -1;
}

/**
 * Add a new path dynamically at runtime
 * @param[in] client the client structure
 * @param[in] if_index the interface index to use for the new path
 * @param[in] server_ip the server IP address string
 * @param[in] server_port the server port (not used for multipath, but kept for future)
 * @return 0 on success, -1 if interface invalid, -2 if probing fails, -3 if path exists
 */
int femto_add_path_dynamic(femto_client_t *client, int if_index, 
                           const char *server_ip, int server_port)
{
    femto_client_ctx_t *client_ctx = client->client_ctx;
    int ret = 0;
    struct sockaddr_storage peer_addr;
    struct sockaddr_storage local_addr;
    char if_name[32] = "";
    
    if (client_ctx == NULL || client->cnx == NULL) {
        printf("femto_add_path_dynamic: client not initialized\n");
        return -1;
    }

    // get interface name (more stable than index)
    if (femto_client_get_interface_name(if_index, if_name) != 0) {
        printf("femto_add_path_dynamic: could not get name for interface %d\n", if_index);
        return -1;
    }

    // check if interface is available
    if (!femto_check_interface_available(if_index)) {
        printf("femto_add_path_dynamic: interface %d (%s) is not available\n", if_index, if_name);
        return -1;
    }

    // Lock mutex for thread safety
    pthread_mutex_lock(&client_ctx->mp_path_mutex);

    // Check if path already exists (by name, not index - handles reconnection case)
    int existing_idx = femto_find_dynamic_path_by_name(client_ctx, if_name);
    if (existing_idx >= 0) {
        // Path exists - check if it's still active
        if (client_ctx->mp_dynamic_paths[existing_idx].is_active) {
            printf("femto_add_path_dynamic: active path for interface %s already exists\n", if_name);
            pthread_mutex_unlock(&client_ctx->mp_path_mutex);
            return -3;
        }
        // Path exists but is suspended/deleted - update index and try to reactivate
        printf("femto_add_path_dynamic: reactivating suspended path for interface %s (old index=%d, new index=%d)\n",
               if_name, client_ctx->mp_dynamic_paths[existing_idx].if_index, if_index);
        client_ctx->mp_dynamic_paths[existing_idx].if_index = if_index;
    }

    // Check if we have room for more paths
    if (existing_idx < 0 && client_ctx->mp_dynamic_path_count >= MAX_DYNAMIC_PATHS) {
        printf("femto_add_path_dynamic: maximum dynamic paths reached\n");
        pthread_mutex_unlock(&client_ctx->mp_path_mutex);
        return -2;
    }

    // Convert server IP to sockaddr_storage
    if (picoquic_store_text_addr(&peer_addr, server_ip, server_port) != 0) {
        printf("femto_add_path_dynamic: invalid server IP %s\n", server_ip);
        pthread_mutex_unlock(&client_ctx->mp_path_mutex);
        return -1;
    }

    // Get local IP for the interface (prefers IPv4 over IPv6 link-local)
    if (femto_get_interface_ip(if_index, &local_addr) != 0) {
        printf("femto_add_path_dynamic: could not get valid IPv4 for interface %d (%s)\n", if_index, if_name);
        pthread_mutex_unlock(&client_ctx->mp_path_mutex);
        return -1;
    }
    
    // Verify we got an IPv4 address, not IPv6 link-local
    if (local_addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&local_addr;
        if (IN6_IS_ADDR_LINKLOCAL(&addr6->sin6_addr)) {
            printf("femto_add_path_dynamic: only IPv6 link-local available for interface %d (%s), waiting for IPv4\n", 
                   if_index, if_name);
            pthread_mutex_unlock(&client_ctx->mp_path_mutex);
            return -1;
        }
    }

    // Probe the new path
    ret = picoquic_probe_new_path_ex(client->cnx, 
                                      (struct sockaddr*)&peer_addr,
                                      (struct sockaddr*)&local_addr,
                                      if_index,
                                      picoquic_get_quic_time(client->quic),
                                      0);
    
    if (ret != 0) {
        printf("femto_add_path_dynamic: picoquic_probe_new_path_ex failed with %d\n", ret);
        pthread_mutex_unlock(&client_ctx->mp_path_mutex);
        return -2;
    }

    // add to dynamic paths tracking (or update existing entry)
    int idx = (existing_idx >= 0) ? existing_idx : client_ctx->mp_dynamic_path_count;
    client_ctx->mp_dynamic_paths[idx].if_index = if_index;
    strncpy(client_ctx->mp_dynamic_paths[idx].if_name, if_name, sizeof(client_ctx->mp_dynamic_paths[idx].if_name) - 1);
    client_ctx->mp_dynamic_paths[idx].if_name[sizeof(client_ctx->mp_dynamic_paths[idx].if_name) - 1] = '\0';
    memcpy(&client_ctx->mp_dynamic_paths[idx].server_ip, &peer_addr, sizeof(peer_addr));
    memcpy(&client_ctx->mp_dynamic_paths[idx].local_ip, &local_addr, sizeof(local_addr));
    client_ctx->mp_dynamic_paths[idx].unique_path_id = 0;  // Will be set in callback
    client_ctx->mp_dynamic_paths[idx].is_active = 0;  // Will be set when path is available
    client_ctx->mp_dynamic_paths[idx].is_initial = 0;
    client_ctx->mp_dynamic_paths[idx].picoquic_path_index = -1;
    client_ctx->mp_dynamic_paths[idx].added_time = time(NULL);
    
    if (existing_idx < 0) {
        client_ctx->mp_dynamic_path_count++;
    }

    printf("femto_add_path_dynamic: initiated path probe for interface %d (%s) (pending confirmation)\n", if_index, if_name);

    pthread_mutex_unlock(&client_ctx->mp_path_mutex);
    return 0;
}

/**
 * Remove a path dynamically at runtime
 * @param[in] client the client structure
 * @param[in] if_index the interface index of the path to remove
 * @return 0 on success, -1 if path not found, -2 if removal fails
 */
int femto_remove_path_dynamic(femto_client_t *client, int if_index)
{
    femto_client_ctx_t *client_ctx = client->client_ctx;
    int ret = 0;
    char if_name[32] = "";
    
    if (client_ctx == NULL || client->cnx == NULL) {
        printf("femto_remove_path_dynamic: client not initialized\n");
        return -1;
    }

    // Try to get interface name (might fail if interface is already gone)
    femto_client_get_interface_name(if_index, if_name);

    pthread_mutex_lock(&client_ctx->mp_path_mutex);

    // find the path (by index first, then by name)
    int path_idx = femto_find_dynamic_path(client_ctx, if_index);
    if (path_idx < 0 && if_name[0] != '\0') {
        path_idx = femto_find_dynamic_path_by_name(client_ctx, if_name);
    }
    
    if (path_idx < 0) {
        printf("femto_remove_path_dynamic: path for interface %d (%s) not found in tracking array\n", 
               if_index, if_name[0] ? if_name : "unknown");
        pthread_mutex_unlock(&client_ctx->mp_path_mutex);
        return -1;
    }

    uint64_t unique_path_id = client_ctx->mp_dynamic_paths[path_idx].unique_path_id;
    int was_active = client_ctx->mp_dynamic_paths[path_idx].is_active;
    int is_initial = client_ctx->mp_dynamic_paths[path_idx].is_initial;
    
    printf("femto_remove_path_dynamic: removing path for interface %d (%s), unique_id=%lu, active=%d, initial=%d\n",
           if_index, client_ctx->mp_dynamic_paths[path_idx].if_name, unique_path_id, was_active, is_initial);
    
    // Only abandon if path was actually established (has valid unique_path_id)
    if (unique_path_id != 0 && was_active) {
        ret = picoquic_abandon_path(client->cnx, 
                                     unique_path_id,
                                     0,  
                                     "interface down",
                                     picoquic_get_quic_time(client->quic));
        
        if (ret != 0) {
            printf("femto_remove_path_dynamic: picoquic_abandon_path failed with %d (may already be abandoned)\n", ret);
            // Don't return error - path might already be abandoned by QUIC layer
        }
    }

    // For initial paths, don't remove from array - just mark as inactive
    // This allows re-adding the path when interface comes back
    if (is_initial) {
        client_ctx->mp_dynamic_paths[path_idx].is_active = 0;
        client_ctx->mp_dynamic_paths[path_idx].unique_path_id = 0;
        printf("femto_remove_path_dynamic: marked initial path for interface %s as inactive (kept in tracking)\n",
               client_ctx->mp_dynamic_paths[path_idx].if_name);
    } else {
        // for dynamically added paths, remove from tracking array
        for (int i = path_idx; i < client_ctx->mp_dynamic_path_count - 1; i++) {
            client_ctx->mp_dynamic_paths[i] = client_ctx->mp_dynamic_paths[i + 1];
        }
        client_ctx->mp_dynamic_path_count--;
        printf("femto_remove_path_dynamic: removed dynamic path for interface %d from tracking\n", if_index);
    }

    pthread_mutex_unlock(&client_ctx->mp_path_mutex);
    return 0;
}

/**
 * Get the number of currently active paths (including dynamic ones)
 * @param[in] client the client structure
 * @return number of active paths
 */
int femto_get_active_path_count(femto_client_t *client)
{
    if (client == NULL || client->cnx == NULL) {
        return 0;
    }
    return client->cnx->nb_paths;
}

/**
 * List all path interface indices
 * @param[in] client the client structure
 * @param[out] if_indices array to store interface indices
 * @param[in] max_paths maximum number of paths to list
 * @return number of paths listed
 */
int femto_list_paths(femto_client_t *client, int *if_indices, int max_paths)
{
    femto_client_ctx_t *client_ctx = client->client_ctx;
    int count = 0;

    if (client_ctx == NULL) {
        return 0;
    }

    pthread_mutex_lock(&client_ctx->mp_path_mutex);

    // Add initial paths
    for (int i = 0; i < client_ctx->mp_nb_alt_paths && count < max_paths; i++) {
        if_indices[count++] = client_ctx->mp_src_if[i];
    }

    // Add dynamic paths
    for (int i = 0; i < client_ctx->mp_dynamic_path_count && count < max_paths; i++) {
        if_indices[count++] = client_ctx->mp_dynamic_paths[i].if_index;
    }

    pthread_mutex_unlock(&client_ctx->mp_path_mutex);
    return count;
}

/**
 * Update a dynamic path's unique_path_id when it becomes available
 * Called from path_available callback
 * @param[in] client_ctx the client context
 * @param[in] unique_path_id the PicoQUIC unique path ID
 */
void femto_update_dynamic_path_id(femto_client_ctx_t *client_ctx, uint64_t unique_path_id)
{
    pthread_mutex_lock(&client_ctx->mp_path_mutex);

    // First, check if this unique_path_id is already known (path re-activation)
    int existing_idx = femto_find_dynamic_path_by_id(client_ctx, unique_path_id);
    if (existing_idx >= 0) {
        client_ctx->mp_dynamic_paths[existing_idx].is_active = 1;
        printf("femto_update_dynamic_path_id: path for interface %s reactivated (id=%lu)\n",
               client_ctx->mp_dynamic_paths[existing_idx].if_name, unique_path_id);
        pthread_mutex_unlock(&client_ctx->mp_path_mutex);
        return;
    }

    // Find the most recently added/modified path that doesn't have a unique_path_id yet
    // Or find an inactive initial path that matches
    for (int i = client_ctx->mp_dynamic_path_count - 1; i >= 0; i--) {
        if (client_ctx->mp_dynamic_paths[i].unique_path_id == 0 || 
            (!client_ctx->mp_dynamic_paths[i].is_active && client_ctx->mp_dynamic_paths[i].is_initial)) {
            client_ctx->mp_dynamic_paths[i].unique_path_id = unique_path_id;
            client_ctx->mp_dynamic_paths[i].is_active = 1;
            printf("femto_update_dynamic_path_id: path for interface %d (%s) is now active (id=%lu, initial=%d)\n",
                   client_ctx->mp_dynamic_paths[i].if_index, 
                   client_ctx->mp_dynamic_paths[i].if_name,
                   unique_path_id,
                   client_ctx->mp_dynamic_paths[i].is_initial);
            break;
        }
    }

    pthread_mutex_unlock(&client_ctx->mp_path_mutex);
}

/**
 * Mark a dynamic path as inactive/deleted
 * Called from path_deleted callback
 * @param[in] client_ctx the client context
 * @param[in] unique_path_id the PicoQUIC unique path ID
 */
void femto_mark_dynamic_path_deleted(femto_client_ctx_t *client_ctx, uint64_t unique_path_id)
{
    pthread_mutex_lock(&client_ctx->mp_path_mutex);

    for (int i = 0; i < client_ctx->mp_dynamic_path_count; i++) {
        if (client_ctx->mp_dynamic_paths[i].unique_path_id == unique_path_id) {
            // For initial paths, keep in tracking but mark as inactive for potential reactivation
            if (client_ctx->mp_dynamic_paths[i].is_initial) {
                client_ctx->mp_dynamic_paths[i].is_active = 0;
                client_ctx->mp_dynamic_paths[i].unique_path_id = 0;  // Clear so it can be reassigned
                printf("femto_mark_dynamic_path_deleted: initial path for interface %s marked inactive (id was %lu)\n",
                       client_ctx->mp_dynamic_paths[i].if_name, unique_path_id);
            } else {
                // For dynamic paths, remove from tracking array
                printf("femto_mark_dynamic_path_deleted: removing dynamic path for interface %s (id=%lu)\n",
                       client_ctx->mp_dynamic_paths[i].if_name, unique_path_id);
                for (int j = i; j < client_ctx->mp_dynamic_path_count - 1; j++) {
                    client_ctx->mp_dynamic_paths[j] = client_ctx->mp_dynamic_paths[j + 1];
                }
                client_ctx->mp_dynamic_path_count--;
            }
            break;
        }
    }

    pthread_mutex_unlock(&client_ctx->mp_path_mutex);
}

/**
 * Mark a dynamic path as suspended
 * Called from path_suspended callback
 * @param[in] client_ctx the client context
 * @param[in] unique_path_id the PicoQUIC unique path ID
 */
void femto_mark_dynamic_path_suspended(femto_client_ctx_t *client_ctx, uint64_t unique_path_id)
{
    pthread_mutex_lock(&client_ctx->mp_path_mutex);

    for (int i = 0; i < client_ctx->mp_dynamic_path_count; i++) {
        if (client_ctx->mp_dynamic_paths[i].unique_path_id == unique_path_id) {
            client_ctx->mp_dynamic_paths[i].is_active = 0;
            printf("femto_mark_dynamic_path_suspended: path for interface %s (id=%lu) is now suspended\n",
                   client_ctx->mp_dynamic_paths[i].if_name, unique_path_id);
            break;
        }
    }

    pthread_mutex_unlock(&client_ctx->mp_path_mutex);
}
