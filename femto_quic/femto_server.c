/*
 * Femto-QUIC, a simple wrapper library for PicoQUIC
 * Based on the PicoQUIC samples by Christian Huitema
 * 
 * Author: David Munstein (2025)
 */

#include <stdint.h>
#include <unistd.h>
#include <picoquic_internal.h>
#include "femto.h"


/* === FEMTO SERVER INTERNAL FUNCTIONS === */

femto_server_stream_ctx_t * femto_server_create_stream_context(femto_server_ctx_t* server_ctx, uint64_t stream_id)
{
    femto_server_stream_ctx_t* stream_ctx = (femto_server_stream_ctx_t*)malloc(sizeof(femto_server_stream_ctx_t));

    if (stream_ctx != NULL) {
        memset(stream_ctx, 0, sizeof(femto_server_stream_ctx_t));
        stream_ctx->response_buffer = (uint8_t*)malloc(BUFFER_SIZE);
        stream_ctx->receive_buffer = NULL;

        if (stream_ctx->response_buffer == NULL) {
            printf("femto_server_create_stream_context: response buffer could not be allocated!\n");
            free(stream_ctx);
            return NULL;
        }

        if (server_ctx->last_stream == NULL) {
            server_ctx->last_stream = stream_ctx;
            server_ctx->first_stream = stream_ctx;
        }
        else {
            stream_ctx->previous_stream = server_ctx->last_stream;
            server_ctx->last_stream->next_stream = stream_ctx;
            server_ctx->last_stream = stream_ctx;
        }
        stream_ctx->stream_id = stream_id;
    }

    return stream_ctx;
}

// used for sending data to the client from the server without previously receiving data from the client
static int femto_server_create_stream(picoquic_cnx_t* cnx,
    femto_server_ctx_t* server_ctx, QueueElement *element)
{
    int ret = 0;
    uint64_t stream_id;
    femto_server_stream_ctx_t* stream_ctx;

    stream_id = picoquic_get_next_local_stream_id(cnx, 1);  // TODO: is_unidir=?

    stream_ctx = femto_server_create_stream_context(server_ctx, stream_id);

    if (stream_ctx == NULL) {
        fprintf(stdout, "Memory Error, cannot create stream");
        ret = -1;
    }

    memcpy(stream_ctx->response_buffer, element->data_buffer, element->data_len);
    stream_ctx->response_size = element->data_len;
    stream_ctx->response_sent = 0;
    
    ret = picoquic_mark_active_stream(cnx, stream_ctx->stream_id, 1, stream_ctx);
    if (ret != 0) {
        fprintf(stdout, "Error %d, cannot initialize stream\n", ret);
    }

    return ret;
}

int femto_server_open_stream(femto_server_ctx_t* server_ctx, femto_server_stream_ctx_t* stream_ctx, picoquic_cnx_t* cnx)
{
    int ret = 0;

    server_ctx->server_callback_fn(stream_ctx->receive_buffer, stream_ctx->buffer_length, server_ctx->user_ctx,
                                   stream_ctx->response_buffer, &stream_ctx->response_size, cnx);

    stream_ctx->response_sent = 0;

    if (stream_ctx->response_size == 0) {
        ret = -1;
    }

    return ret;
}

void femto_server_delete_stream_context(femto_server_ctx_t* server_ctx, femto_server_stream_ctx_t* stream_ctx)
{
    if (stream_ctx->previous_stream == NULL) {
        server_ctx->first_stream = stream_ctx->next_stream;
    }
    else {
        stream_ctx->previous_stream->next_stream = stream_ctx->next_stream;
    }

    if (stream_ctx->next_stream == NULL) {
        server_ctx->last_stream = stream_ctx->previous_stream;
    }
    else {
        stream_ctx->next_stream->previous_stream = stream_ctx->previous_stream;
    }

    if (stream_ctx->receive_buffer != NULL) {
        free(stream_ctx->receive_buffer);
    }
    if (stream_ctx->response_buffer != NULL) {
        free(stream_ctx->response_buffer);
    }
    free(stream_ctx);
}

void femto_server_delete_context(femto_server_ctx_t* server_ctx)
{
    while (server_ctx->first_stream != NULL) {
        femto_server_delete_stream_context(server_ctx, server_ctx->first_stream);
    }

    free(server_ctx);
}

void femto_server_receive_datagram(picoquic_cnx_t* cnx, femto_server_ctx_t* ctx, const uint8_t* bytes, size_t length)
{
    // TODO Datagram
    printf("Femto Server: Received datagram of length %lu:", length);
    printf(" %s\n", bytes);
}

int femto_server_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
    int ret = 0;
    femto_server_ctx_t* server_ctx = (femto_server_ctx_t*)callback_ctx;
    femto_server_stream_ctx_t* stream_ctx = (femto_server_stream_ctx_t*)v_stream_ctx;

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

    /* If this is the first reference to the connection, the application context is set
     * to the default value defined for the server. This default value contains the pointer
     * to the file directory in which all files are defined.
     */
    if (callback_ctx == NULL || callback_ctx == picoquic_get_default_callback_context(picoquic_get_quic_ctx(cnx))) {
        server_ctx = (femto_server_ctx_t *)malloc(sizeof(femto_server_ctx_t));
        if (server_ctx == NULL) {
            /* cannot handle the connection */
            picoquic_close(cnx, PICOQUIC_ERROR_MEMORY);
            return -1;
        }
        else {
            femto_server_ctx_t* d_ctx = (femto_server_ctx_t*)picoquic_get_default_callback_context(picoquic_get_quic_ctx(cnx));
            if (d_ctx != NULL) {
                memcpy(server_ctx, d_ctx, sizeof(femto_server_ctx_t));
            }
            else {
                /* This really is an error case: the default connection context should never be NULL */
                memset(server_ctx, 0, sizeof(femto_server_ctx_t));
            }
            picoquic_set_callback(cnx, femto_server_callback, server_ctx);
        }
    }

    if (ret == 0) {
        switch (fin_or_event) {
        case picoquic_callback_stream_data:
        case picoquic_callback_stream_fin:
            /* Data arrival on stream #x, maybe with fin mark */
            if (stream_ctx == NULL) {
                /* Create and initialize stream context */
                stream_ctx = femto_server_create_stream_context(server_ctx, stream_id);
                if (picoquic_set_app_stream_ctx(cnx, stream_id, stream_ctx) != 0) {
                    /* Internal error */
                    (void) picoquic_reset_stream(cnx, stream_id, 0);
                    return(-1);
                }
            }

            if (stream_ctx == NULL) {
                /* Internal error */
                (void) picoquic_reset_stream(cnx, stream_id, 0);
                return(-1);
            }
            else if (stream_ctx->is_data_read) {
                /* Write after fin? */
                return(-1);
            }
            else {
                if (stream_ctx->receive_buffer == NULL) {
                    stream_ctx->receive_buffer = (uint8_t*)malloc(BUFFER_SIZE);
                    if (stream_ctx->receive_buffer == NULL) {
                        femto_server_delete_stream_context(server_ctx, stream_ctx);
                        (void) picoquic_reset_stream(cnx, stream_id, 0);
                        return -1;
                    }
                }
                /* Accumulate data */
                //size_t available = sizeof(stream_ctx->receive_buffer) - stream_ctx->buffer_length - 1;
                size_t available = BUFFER_SIZE - stream_ctx->buffer_length - 1;

                if (length > available) {
                    /* Name too long: reset stream! */
                    femto_server_delete_stream_context(server_ctx, stream_ctx);
                    (void) picoquic_reset_stream(cnx, stream_id, 0);
                }
                else {
                    if (length > 0) {
                        memcpy(stream_ctx->receive_buffer + stream_ctx->buffer_length, bytes, length);
                        stream_ctx->buffer_length += length;
                    }
                    if (fin_or_event == picoquic_callback_stream_fin) {
                        int stream_ret;

                        /* If fin, mark read, check the file, open it. Or reset if there is no such file */
                        stream_ctx->receive_buffer[stream_ctx->buffer_length + 1] = 0;
                        stream_ctx->is_data_read = 1;

                        stream_ret = femto_server_open_stream(server_ctx, stream_ctx, cnx);
                        free(stream_ctx->receive_buffer);
                        stream_ctx->receive_buffer = NULL;

                        if (stream_ret == 0) {
                            /* If data needs to be sent, set the context as active */
                            ret = picoquic_mark_active_stream(cnx, stream_id, 1, stream_ctx);
                        }
                        else {  // error or no response data to send
                            /* If the file could not be read, reset the stream */
                            femto_server_delete_stream_context(server_ctx, stream_ctx);
                            (void) picoquic_reset_stream(cnx, stream_id, stream_ret);
                        }
                    }
                }
            }
            break;
        case picoquic_callback_prepare_to_send:
            /* Active sending API */
            if (stream_ctx == NULL) {
                /* This should never happen */
            }
            else if (stream_ctx->response_sent >= stream_ctx->response_size) {}
            else {
                size_t available = stream_ctx->response_size - stream_ctx->response_sent;
                int is_fin = 1;
                uint8_t* buffer;

                if (available > length) {
                    available = length;
                    is_fin = 0;
                }
                
                buffer = picoquic_provide_stream_data_buffer(bytes, available, is_fin, !is_fin);
                if (buffer != NULL) {
                    memcpy(buffer, stream_ctx->response_buffer, available);
                    stream_ctx->response_sent += available;
                }
                else {
                /* Should never happen according to callback spec. */
                    ret = -1;
                }

                if (is_fin) {
                    free(stream_ctx->response_buffer);
                    stream_ctx->response_buffer = NULL;
                }
            }
            break;
        case picoquic_callback_datagram:
            femto_server_receive_datagram(cnx, server_ctx, bytes, length);
            break;
        case picoquic_callback_stream_reset: /* Client reset stream #x */
        case picoquic_callback_stop_sending: /* Client asks server to reset stream #x */
            if (stream_ctx != NULL) {
                /* Mark stream as abandoned, close the file, etc. */
                femto_server_delete_stream_context(server_ctx, stream_ctx);
                picoquic_reset_stream(cnx, stream_id, 0);
            }
            break;
        case picoquic_callback_stateless_reset: /* Received an error message */
        case picoquic_callback_close: /* Received connection close */
        case picoquic_callback_application_close: /* Received application close */
            /* Delete the server application context */
            server_ctx->is_disconnected = 1;
            femto_server_delete_context(server_ctx);
            picoquic_set_callback(cnx, NULL, NULL);
            break;
        case picoquic_callback_version_negotiation:
            /* The server should never receive a version negotiation response */
            break;
        case picoquic_callback_stream_gap:
            /* This callback is never used. */
            break;
        case picoquic_callback_almost_ready:
        case picoquic_callback_ready:
            /* Check that the transport parameters are what the sample expects */
            break;
        case picoquic_callback_path_available:
        case picoquic_callback_path_suspended:
        case picoquic_callback_path_deleted:
        case picoquic_callback_path_quality_changed:
            /* Multipath path changed - call status callback */
            if (server_ctx->status_callback_fn != NULL) {
                server_ctx->status_callback_fn(60 + fin_or_event, &stream_id, server_ctx->user_ctx);
            }
            break;
        default:
            /* unexpected */
            break;
        }
    }

    return ret;
}

static int femto_server_wakeup(femto_server_ctx_t* server_ctx)
{
    int ret = 0;
    uint64_t stream_id;
    QueueElement element;

    while (!isQueueEmpty(server_ctx->q)) {
        dequeue(server_ctx->q, &element);

        ret = femto_server_create_stream((picoquic_cnx_t*)element.additional_data, server_ctx, &element);
        if (ret < 0) {
            printf("\nCould not initiate stream\n");
            break;
        }
    }

    if (server_ctx->is_closing_requested) {
        server_ctx->is_disconnected = 1;  // TODO: remove, this is hacky!!!
        // picoquic_cnx_t* cnx = picoquic_get_first_cnx(server_ctx->quic);
        // while (cnx != NULL) {
        //     picoquic_close(cnx, 0);
        //     cnx = picoquic_get_next_cnx(cnx);
        // }
    }

    return ret;
}

static int femto_server_loop_cb(picoquic_quic_t* quic, picoquic_packet_loop_cb_enum cb_mode, 
    void* callback_ctx, void * callback_arg)
{
    int ret = 0;
    femto_server_ctx_t* server_ctx = (femto_server_ctx_t*)callback_ctx;

    if (server_ctx == NULL) {
        ret = PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }
    else {
        switch (cb_mode) {
        case picoquic_packet_loop_ready:
            break;
        case picoquic_packet_loop_wake_up:
            ret = femto_server_wakeup(server_ctx);
            break;
        case picoquic_packet_loop_after_receive:
            break;
        case picoquic_packet_loop_after_send:
            if (server_ctx->is_disconnected) {
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


static int femto_server_configure(uint32_t max_nb_connections, picoquic_quic_config_t *config,
    const char *server_cert, const char *server_key, const char *alpn, int enable_multipath)
{
    int ret = 0;

    config->nb_connections = max_nb_connections;
    picoquic_config_set_option(config, picoquic_option_ALPN, alpn);
    picoquic_config_set_option(config, picoquic_option_CERT, server_cert);
    picoquic_config_set_option(config, picoquic_option_KEY, server_key);

    if (enable_multipath) {
        config->multipath_option = 1;
    }

    return ret;
}


/* === FEMTO SERVER PUBLIC FUNCTIONS === */

/**
 * Create a new Femto-QUIC server
 * @param[out] server the server structure to be created
 * @param[in] port the port to listen on
 * @param[in] mp_enabled enable multipath
 * @param[in] alpn the ALPN to use for the connection
 * @param[in] server_cert the server certificate file path
 * @param[in] server_key the server key file path
 * @param[in] callback_fn the callback function to be called when data is received. Passes the received data and length, the callback context,
 * and a response buffer and length. (uint8_t *receive_buffer, size_t receive_len, void *user_ctx, uint8_t *response_buffer, size_t *response_len)
 * @param[in] user_ctx the user context to be passed to the callback function
 */
int create_femto_server(femto_server_t *server, int port, int mp_enabled, const char* alpn,
                        const char* server_cert, const char* server_key,
                        void *callback_fn(uint8_t *, size_t, void *, uint8_t *, size_t *, picoquic_cnx_t *), void *user_ctx)
{
    uint64_t current_time = 0;
    int *thread_ret = malloc(sizeof(int));
    picoquic_packet_loop_param_t *param = malloc(sizeof(picoquic_packet_loop_param_t));

    if (access(server_cert, F_OK) != 0) {
        printf("Cannot access certificate file: %s\n", server_cert);
        return -2;
    }
    if (access(server_key, F_OK) != 0) {
        printf("Cannot access key file: %s\n", server_key);
        return -3;
    }

    server->port = port;
    server->loop_thread = malloc(sizeof(pthread_t));

    server->server_ctx = (femto_server_ctx_t*)malloc(sizeof(femto_server_ctx_t));
    memset(server->server_ctx, 0, sizeof(femto_server_ctx_t));
    server->server_ctx->server_callback_fn = callback_fn;
    server->server_ctx->user_ctx = user_ctx;
    server->server_ctx->q = (Queue *)malloc(sizeof(Queue));
    initQueue(server->server_ctx->q);

    server->config = (picoquic_quic_config_t *)malloc(sizeof(picoquic_quic_config_t));
    picoquic_config_init(server->config);

    femto_server_configure(8, server->config, server_cert, server_key, alpn, mp_enabled);

    current_time = picoquic_current_time();
    server->quic = picoquic_create_and_configure(server->config, femto_server_callback, server->server_ctx,
                                                 current_time, NULL);


    if (server->quic == NULL) {
        printf("Could not create quic context\n");
        return -1;
    }

    picoquic_set_cookie_mode(server->quic, 2);
    picoquic_set_default_congestion_algorithm(server->quic, picoquic_bbr_algorithm);

    param->local_port = server->port;

    // start the background thread for PicoQUIC networking
    server->thread_ctx = picoquic_start_custom_network_thread(server->quic, param,
        picoquic_internal_thread_create, picoquic_internal_thread_delete,
        picoquic_internal_thread_setname, "femto_server_thread",  // TODO: maybe include path id in thread name, what is it needed for?
        femto_server_loop_cb,
        server->server_ctx, thread_ret);

    return 0;
}


/**
 * Destroy a Femto-QUIC server
 * @param[in] server the server structure to be destroyed
 */
void destroy_femto_server(femto_server_t *server)
{
    // TODO: Implement close signal to server loop (server->server_ctx->is_close_requested e.g.)
    //pthread_join(*server->loop_thread, NULL);
    femto_server_ctx_t *server_ctx = server->server_ctx;
    picoquic_network_thread_ctx_t *thread_ctx = server->thread_ctx;

    while (!thread_ctx->thread_is_closed) {
        if (server_ctx->is_disconnected) {
            break;
        }

        server_ctx->is_closing_requested = 1;
        picoquic_wake_up_network_thread(thread_ctx);

#ifdef _WINDOWS
            Sleep(10);
#else
            usleep(10000);
#endif
    }

    picoquic_delete_network_thread(thread_ctx);
    server->thread_ctx = NULL;

    if (server->quic != NULL) {
        picoquic_free(server->quic);
    }
}

/**
 * Send data to a client (without needing to receive data from the client first)
 * @param[in] server the server structure
 * @param[in] path_id the path to send the data on
 * @param[in] cnx the context of the client connection
 * @param[in] data the data to send
 * @param[in] len the length of the data
 * @return 0 if successful
 */
int send_data_to_client(femto_server_t *server, int path_id, picoquic_cnx_t *cnx, uint8_t *data, size_t len)
{
    femto_server_ctx_t *server_ctx = server->server_ctx;
    picoquic_network_thread_ctx_t *thread_ctx = server->thread_ctx;
 
    QueueElement element;

    if (!server_ctx->is_closing_requested && !server_ctx->is_disconnected) {
        memcpy(element.data_buffer, data, len);
        element.data_len = len;
        element.path_id = path_id;
        element.additional_data = cnx;

        enqueue(server_ctx->q, element);
        picoquic_wake_up_network_thread(thread_ctx);
    } else {
        return 1;
    }

    return 0;
}

/**
 * Register a callback function to receive status updates from the server.
 * This includes multipath path status updates.
 * The lifetime of the status_data is only valid during the callback function.
 * @param[in] server the server structure
 * @param[in] callback_fn the callback function to be called when a status update is received
 */
void register_femto_server_status_callback(femto_server_t *server, void *callback_fn(uint8_t status_type, void *status_data, void *user_ctx))
{
    server->server_ctx->status_callback_fn = callback_fn;
}

/**
 * Enable the Femto-QUIC key log file.
 * This is useful for debugging purposes, e.g. to decrypt QUIC traffic with Wireshark.
 * @param[in] server the server structure
 * @param[in] keylogfile the key log file path
 */
void enable_femto_server_sslkeylogfile(femto_server_t *server, char *keylogfile)
{
    picoquic_set_key_log_file(server->quic, keylogfile);
}
